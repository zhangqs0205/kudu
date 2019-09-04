// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/collector/metrics_collector.h"

#include <string.h>

#include <cmath>
#include <functional>
#include <list>
#include <ostream>
#include <set>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <rapidjson/rapidjson.h>

#include "kudu/collector/collector_util.h"
#include "kudu/collector/nodes_checker.h"
#include "kudu/collector/reporter_base.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/walltime.h"
#include "kudu/util/curl_util.h"
#include "kudu/util/debug/trace_event.h"
#include "kudu/util/faststring.h"
#include "kudu/util/jsonreader.h"
#include "kudu/util/monotime.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"
#include "kudu/util/string_case.h"
#include "kudu/util/thread.h"
#include "kudu/util/threadpool.h"
#include "kudu/util/trace.h"
#include "kudu/util/zlib.h"

DEFINE_string(collector_attributes, "",
              "Entity attributes to collect (semicolon-separated list of entity attribute "
              "name and values). e.g. attr_name1:attr_val1,attr_val2;attr_name2:attr_val3");
DEFINE_string(collector_cluster_level_metrics, "on_disk_size,on_disk_data_size",
              "Metric names which should be merged and pushed to cluster level view "
              "(comma-separated list of metric names)");
DEFINE_bool(collector_ignore_hosttable_level_metrics, false,
            "Whether to ignore to report host-table level metrics.");
DEFINE_string(collector_metrics, "",
              "Metrics to collect (comma-separated list of metric names)");
DEFINE_string(collector_metrics_types_for_test, "",
              "Only for test, used to initialize metric_types_by_entity_type_");
DEFINE_bool(collector_request_merged_metrics, true,
            "Whether to request merged metrics and exclude unmerged metrics from server");
DEFINE_string(collector_table_names, "",
              "Table names to collect (comma-separated list of table names)");

DECLARE_string(collector_cluster_name);
DECLARE_int32(collector_interval_sec);
DECLARE_int32(collector_timeout_sec);
DECLARE_int32(collector_warn_threshold_ms);

using rapidjson::Value;
using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;
using std::unordered_map;
using strings::Substitute;

namespace kudu {
namespace collector {

const set<string> MetricsCollector::kRegisterPercentiles = {"percentile_99"};

MetricsCollector::MetricsCollector(scoped_refptr<NodesChecker> nodes_checker,
                                   scoped_refptr<ReporterBase> reporter)
  : initialized_(false),
    nodes_checker_(std::move(nodes_checker)),
    reporter_(std::move(reporter)),
    stop_background_threads_latch_(1) {
}

MetricsCollector::~MetricsCollector() {
  Shutdown();
}

Status MetricsCollector::Init() {
  CHECK(!initialized_);

  RETURN_NOT_OK(ValidateTableFilter(FLAGS_collector_attributes, FLAGS_collector_table_names));
  RETURN_NOT_OK(InitMetrics());
  RETURN_NOT_OK(InitFilters());
  CHECK(attributes_filter_.empty());  // TODO(yingchun) disable now
  RETURN_NOT_OK(InitMetricsUrlParameters());
  RETURN_NOT_OK(InitClusterLevelMetrics());

  initialized_ = true;
  return Status::OK();
}

Status MetricsCollector::Start() {
  CHECK(initialized_);

  RETURN_NOT_OK(StartMetricCollectorThread());

  return Status::OK();
}

void MetricsCollector::Shutdown() {
  if (initialized_) {
    string name = ToString();
    LOG(INFO) << name << " shutting down...";

    stop_background_threads_latch_.CountDown();

    if (metric_collector_thread_) {
      metric_collector_thread_->Join();
    }

    LOG(INFO) << name << " shutdown complete.";
  }
}

string MetricsCollector::ToString() const {
  return "MetricsCollector";
}

Status MetricsCollector::StartMetricCollectorThread() {
  return Thread::Create("server", "metric-collector", &MetricsCollector::MetricCollectorThread,
                        this, &metric_collector_thread_);
}

void MetricsCollector::MetricCollectorThread() {
  MonoTime collect_time;
  do {
    collect_time = MonoTime::Now();
    WARN_NOT_OK(CollectAndReportMetrics(), "Unable to collect metrics");
    collect_time += MonoDelta::FromSeconds(FLAGS_collector_interval_sec);
  } while (!RunOnceMode() && !stop_background_threads_latch_.WaitUntil(collect_time));
  LOG(INFO) << "MetricCollectorThread exit";
}

Status MetricsCollector::UpdateThreadPool(int32_t thread_count) {
  if (host_metric_collector_thread_pool_ &&
      host_metric_collector_thread_pool_->num_threads() == thread_count) {
    return Status::OK();
  }

  if (host_metric_collector_thread_pool_) {
    host_metric_collector_thread_pool_->Shutdown();
  }
  TRACE("Old thread pool shutdown");

  RETURN_NOT_OK(ThreadPoolBuilder("host-metric-collector")
      .set_min_threads(thread_count)
      .set_max_threads(thread_count)
      .set_idle_timeout(MonoDelta::FromMilliseconds(1))
      .Build(&host_metric_collector_thread_pool_));
  TRACE("New thread pool built");

  return Status::OK();
}

Status MetricsCollector::ValidateTableFilter(const string& attribute_filter,
                                             const string& /*table_filter*/) {
  if (attribute_filter.empty()) {
    return Status::OK();
  }

  return Status::InvalidArgument("attribute filter is not supported now");
}

Status MetricsCollector::InitMetrics() {
  string resp;
  if (PREDICT_TRUE(FLAGS_collector_metrics_types_for_test.empty())) {
    RETURN_NOT_OK(GetMetrics(nodes_checker_->GetFirstNode() + "/metrics?include_schema=1", &resp));
  } else {
    resp = FLAGS_collector_metrics_types_for_test;
  }
  JsonReader r(resp);
  RETURN_NOT_OK(r.Init());
  vector<const Value*> entities;
  RETURN_NOT_OK(r.ExtractObjectArray(r.root(), nullptr, &entities));

  map<string, MetricTypes> metric_types_by_entity_type;
  bool tablet_entity_inited = false;
  bool server_entity_inited = false;
  for (const Value* entity : entities) {
    string entity_type;
    CHECK_OK(r.ExtractString(entity, "type", &entity_type));
    if (entity_type == "tablet" || entity_type == "table") {
      if (tablet_entity_inited) continue;
      EmplaceOrDie(&metric_types_by_entity_type, std::make_pair("tablet", MetricTypes()));
      auto& tablet_metric_types = FindOrDie(metric_types_by_entity_type, "tablet");
      ExtractMetricTypes(r, entity, &tablet_metric_types);
      tablet_entity_inited = true;
    } else if (entity_type == "server") {
      if (server_entity_inited) continue;
      EmplaceOrDie(&metric_types_by_entity_type, std::make_pair("server", MetricTypes()));
      auto& server_metric_types = FindOrDie(metric_types_by_entity_type, "server");
      ExtractMetricTypes(r, entity, &server_metric_types);
      server_entity_inited = true;
    } else {
      LOG(WARNING) << "unhandled entity type " << entity_type;
    }
  }
  metric_types_by_entity_type_.swap(metric_types_by_entity_type);
  return Status::OK();
}

Status MetricsCollector::ExtractMetricTypes(const JsonReader& r,
                                            const Value* entity,
                                            MetricTypes* metric_types) {
  CHECK(metric_types);
  vector<const Value*> metrics;
  RETURN_NOT_OK(r.ExtractObjectArray(entity, "metrics", &metrics));
  for (const Value* metric : metrics) {
    string name;
    RETURN_NOT_OK(r.ExtractString(metric, "name", &name));
    string type;
    RETURN_NOT_OK(r.ExtractString(metric, "type", &type));
    string upper_type;
    ToUpperCase(type, &upper_type);
    EmplaceOrDie(metric_types, std::make_pair(name, upper_type));
  }
  return Status::OK();
}

Status MetricsCollector::InitFilters() {
  unordered_map<string, set<string>> attributes_filter;
  vector<string> attribute_values_by_name_list =
      Split(FLAGS_collector_attributes, ";", strings::SkipEmpty());
  for (const auto& attribute_values_by_name : attribute_values_by_name_list) {
    vector<string> attribute_name_and_values =
        Split(attribute_values_by_name, ":", strings::SkipEmpty());
    CHECK_EQ(attribute_name_and_values.size(), 2);
    set<string> values(Split(attribute_name_and_values[1], ",", strings::SkipEmpty()));
    CHECK(!values.empty());
    EmplaceOrDie(&attributes_filter, std::make_pair(attribute_name_and_values[0], values));
  }
  attributes_filter_.swap(attributes_filter);
  return Status::OK();
}

Status MetricsCollector::InitMetricsUrlParameters() {
  metric_url_parameters_ = "/metrics?compact=1";
  if (!FLAGS_collector_metrics.empty()) {
    metric_url_parameters_ += "&metrics=" + FLAGS_collector_metrics;
  }
  if (FLAGS_collector_request_merged_metrics) {
    metric_url_parameters_ += "&origin=false&merge=true";
  } else {
    LOG(FATAL) << "Non-merge mode is not supported now, you should set "
                  "FLAGS_collector_request_merged_metrics to true if you "
                  "want collector work well";
  }

  // TODO(yingchun) This is supported since version 1.10
  if (!attributes_filter_.empty()) {
    metric_url_parameters_ += "&attributes=";
  }
  for (const auto& attribute_filter : attributes_filter_) {
    for (const auto& value : attribute_filter.second) {
      metric_url_parameters_ += Substitute("$0,$1,", attribute_filter.first, value);
    }
  }
  // TODO(yingchun) This is supported since internal version 1.8.0
  if (!FLAGS_collector_table_names.empty()) {
    metric_url_parameters_ += "&table_names=" + FLAGS_collector_table_names;
  }
  return Status::OK();
}

Status MetricsCollector::InitClusterLevelMetrics() {
  Metrics cluster_metrics;
  vector<string> metric_names =
      Split(FLAGS_collector_cluster_level_metrics, ",", strings::SkipEmpty());
  for (const auto& metric_name : metric_names) {
    cluster_metrics[metric_name] = 0;
  }
  cluster_metrics_.swap(cluster_metrics);
  return Status::OK();
}

Status MetricsCollector::CollectAndReportMetrics() {
  LOG(INFO) << "Start to CollectAndReportMetrics";
  MonoTime start(MonoTime::Now());
  scoped_refptr<Trace> trace(new Trace);
  ADOPT_TRACE(trace.get());
  TRACE_EVENT0("collector", "MetricsCollector::CollectAndReportMetrics");
  TRACE("init");
  vector<string> tserver_http_addrs = nodes_checker_->GetNodes();
  TRACE("Nodes got");
  RETURN_NOT_OK(UpdateThreadPool(static_cast<int32_t>(tserver_http_addrs.size())));
  vector<TablesMetrics> hosts_metrics_by_table_name(tserver_http_addrs.size());
  vector<TablesHistMetrics> hosts_hist_metrics_by_table_name(tserver_http_addrs.size());
  for (int i = 0; i < tserver_http_addrs.size(); ++i) {
    RETURN_NOT_OK(host_metric_collector_thread_pool_->SubmitFunc(
      std::bind(&MetricsCollector::CollectAndReportHostLevelMetrics,
                this,
                tserver_http_addrs[i] + metric_url_parameters_,
                &hosts_metrics_by_table_name[i],
                &hosts_hist_metrics_by_table_name[i])));
  }
  TRACE("Thead pool jobs submitted");
  host_metric_collector_thread_pool_->Wait();
  TRACE("Thead pool jobs done");

  // Merge to table level metrics.
  TablesMetrics metrics_by_table_name;
  TablesHistMetrics hist_metrics_by_table_name;
  RETURN_NOT_OK(MergeToTableLevelMetrics(hosts_metrics_by_table_name,
                                         hosts_hist_metrics_by_table_name,
                                         &metrics_by_table_name,
                                         &hist_metrics_by_table_name));

  // Merge to cluster level metrics.
  Metrics cluster_metrics(cluster_metrics_);
  RETURN_NOT_OK(MergeToClusterLevelMetrics(metrics_by_table_name,
                                           hist_metrics_by_table_name,
                                           &cluster_metrics));

  auto timestamp = static_cast<uint64_t>(WallTime_Now());

  // Push table level metrics.
  RETURN_NOT_OK(ReportTableLevelMetrics(timestamp,
                                        metrics_by_table_name,
                                        hist_metrics_by_table_name));

  // Push cluster level metrics.
  RETURN_NOT_OK(ReportClusterLevelMetrics(timestamp, cluster_metrics));

  int64_t elapsed_ms = (MonoTime::Now() - start).ToMilliseconds();
  if (elapsed_ms > FLAGS_collector_warn_threshold_ms) {
    if (Trace::CurrentTrace()) {
      LOG(WARNING) << "Trace:" << std::endl
                   << Trace::CurrentTrace()->DumpToString();
    }
  }

  return Status::OK();
}

Status MetricsCollector::MergeToTableLevelMetrics(
  const vector<TablesMetrics>& hosts_metrics_by_table_name,
  const vector<TablesHistMetrics>& hosts_hist_metrics_by_table_name,
  TablesMetrics* metrics_by_table_name,
  TablesHistMetrics* hist_metrics_by_table_name) {
  CHECK(metrics_by_table_name);
  CHECK(hist_metrics_by_table_name);

  // GAUGE/COUNTER type metrics.
  int metrics_count = 0;
  for (const auto& host_metrics_by_table_name : hosts_metrics_by_table_name) {
    for (const auto& table_metrics1 : host_metrics_by_table_name) {
      const auto& table_name = table_metrics1.first;
      const auto& metrics = table_metrics1.second;
      metrics_count += metrics.size();
      if (EmplaceIfNotPresent(metrics_by_table_name, std::make_pair(table_name, metrics))) {
        continue;
      }
      // This table has been fetched by some other tserver.
      auto& table_metrics = FindOrDie(*metrics_by_table_name, table_name);
      for (const auto& metric_value : metrics) {
        const auto& metric = metric_value.first;
        const auto& value = metric_value.second;
        if (EmplaceIfNotPresent(&table_metrics, std::make_pair(metric, value))) {
          continue;
        }
        // This metric has been fetched by some other tserver.
        auto& old_value = FindOrDie(table_metrics, metric);
        old_value += value;
      }
    }
  }
  TRACE(Substitute("Table GAUGE/COUNTER type metrics merged, count $0", metrics_count));

  // HISTOGRAM type metrics.
  metrics_count = 0;
  for (const auto& host_hist_metrics_by_table_name : hosts_hist_metrics_by_table_name) {
    for (const auto& table_hist_metrics1 : host_hist_metrics_by_table_name) {
      const auto& table_name = table_hist_metrics1.first;
      const auto& metrics = table_hist_metrics1.second;
      metrics_count += metrics.size();
      if (EmplaceIfNotPresent(hist_metrics_by_table_name, std::make_pair(table_name, metrics))) {
        continue;
      }
      // This table has been fetched by some other tserver.
      auto& table_hist_metrics = FindOrDie(*hist_metrics_by_table_name, table_name);
      for (const auto& metric_hist_values : metrics) {
        const auto& metric = metric_hist_values.first;
        const auto& hist_values = metric_hist_values.second;
        if (EmplaceIfNotPresent(&table_hist_metrics, std::make_pair(metric, hist_values))) {
          continue;
        }
        // This metric has been fetched by some other tserver.
        auto& old_hist_values = FindOrDie(table_hist_metrics, metric);
        for (auto& hist_value : hist_values) {
          old_hist_values.emplace_back(hist_value);
        }
      }
    }
  }
  TRACE(Substitute("Table HISTOGRAM type metrics merged, count $0", metrics_count));

  return Status::OK();
}

Status MetricsCollector::MergeToClusterLevelMetrics(
    const TablesMetrics& metrics_by_table_name,
    const TablesHistMetrics& /*hist_metrics_by_table_name*/,
    Metrics* cluster_metrics) {
  CHECK(cluster_metrics);
  if (!cluster_metrics->empty()) {
    for (const auto& table_metrics : metrics_by_table_name) {
      for (auto& cluster_metric : *cluster_metrics) {
        auto *find = FindOrNull(table_metrics.second, cluster_metric.first);
        if (find) {
          cluster_metric.second += *find;
        }
      }
    }
  }
  TRACE(Substitute("Cluster metrics merged, count $0", cluster_metrics->size()));

  return Status::OK();
}

Status MetricsCollector::GetNumberMetricValue(const rapidjson::Value *metric,
                                              const string &metric_name /*metric_name*/,
                                              int64_t *result) const {
  CHECK(result);
  if (metric->IsUint64() || metric->IsInt64() || metric->IsUint() || metric->IsInt()) {
    *result = metric->GetInt64();
    return Status::OK();
  }

  if (metric->IsDouble()) {
    double result_temp = metric->GetDouble();
    // Multiply by 1000000 and convert to int64_t to avoid much data loss and keep compatibility
    // with monitor system like Falcon.
    *result = static_cast<int64_t>(result_temp * 1000000);
    return Status::OK();
  }

  return Status::NotSupported(Substitute("unsupported metric $0", metric_name));
}

Status MetricsCollector::GetStringMetricValue(const Value* metric,
                                              const string& metric_name,
                                              int64_t* result) const {
  CHECK(result);
  string value(metric->GetString());
  if (metric_name == "state") {
    return ConvertStateToInt(value, result);
  }
  return Status::NotSupported(Substitute("unsupported metric $0", metric_name));
}

Status MetricsCollector::ConvertStateToInt(const string& value, int64_t* result) {
  CHECK(result);
  // TODO(yingchun) Here, table state is merged by several original tablet states, which is
  // contacted by several sub-strings, like 'RUNNING', 'BOOTSTRAPPING', etc. It's tricky to
  // fetch state now, we will improve in server side later.
  const char* running = "RUNNING";
  if (value.empty() || value.size() % strlen(running) != 0) {
    *result = 0;
    return Status::OK();
  }
  for (int i = 0; i < value.size(); i += strlen(running)) {
    if (0 != strncmp(running, value.c_str() + i, strlen(running))) {
      *result = 0;
      return Status::OK();
    }
  }
  *result = 1;
  return Status::OK();
}

bool MetricsCollector::FilterByAttribute(const JsonReader& r,
                                         const rapidjson::Value* entity) const {
  if (attributes_filter_.empty()) {
    return false;
  }
  const Value* attributes;
  CHECK_OK(r.ExtractObject(entity, "attributes", &attributes));
  for (const auto& name_values : attributes_filter_) {
    string value;
    Status s = r.ExtractString(attributes, name_values.first.c_str(), &value);
    if (s.ok() && ContainsKey(name_values.second, value)) {
      return false;
    }
  }
  return true;
}

Status MetricsCollector::ParseServerMetrics(const JsonReader& /*r*/,
                                            const rapidjson::Value* /*entity*/) {
  return Status::NotSupported("server entity is not supported");
}

Status MetricsCollector::ParseTableMetrics(const JsonReader& r,
                                           const rapidjson::Value* entity,
                                           TablesMetrics* metrics_by_table_name,
                                           Metrics* host_metrics,
                                           TablesHistMetrics* hist_metrics_by_table_name,
                                           HistMetrics* host_hist_metrics) const {
  CHECK(metrics_by_table_name);
  CHECK(host_metrics);
  CHECK(hist_metrics_by_table_name);
  CHECK(host_hist_metrics);

  string table_name;
  CHECK_OK(r.ExtractString(entity, "id", &table_name));
  CHECK(!ContainsKey(*metrics_by_table_name, table_name));
  CHECK(!ContainsKey(*hist_metrics_by_table_name, table_name));

  EmplaceOrDie(metrics_by_table_name, std::make_pair(table_name, Metrics()));
  auto& table_metrics = FindOrDie(*metrics_by_table_name, table_name);

  EmplaceOrDie(hist_metrics_by_table_name, std::make_pair(table_name, HistMetrics()));
  auto& table_hist_metrics = FindOrDie(*hist_metrics_by_table_name, table_name);

  vector<const Value*> metrics;
  CHECK_OK(r.ExtractObjectArray(entity, "metrics", &metrics));
  for (const Value* metric : metrics) {
    string name;
    CHECK_OK(r.ExtractString(metric, "name", &name));
    const auto* tablet_metric_types = FindOrNull(metric_types_by_entity_type_, "tablet");
    CHECK(tablet_metric_types);
    const auto* known_type = FindOrNull(*tablet_metric_types, name);
    if (!known_type) {
      LOG(ERROR) << Substitute("metric $0 has unknown type, ignore it", name);
      continue;
    }

    if (*known_type == "GAUGE" || *known_type ==  "COUNTER") {
      int64_t value = 0;
      const Value* val;
      RETURN_NOT_OK(r.ExtractField(metric, "value", &val));
      rapidjson::Type type = val->GetType();
      switch (type) {
        case rapidjson::Type::kStringType:
          CHECK_OK(GetStringMetricValue(val, name, &value));
          break;
        case rapidjson::Type::kNumberType:
          CHECK_OK(GetNumberMetricValue(val, name, &value));
          break;
        default:
          LOG(FATAL) << "Unknown type, metrics name: " << name;
      }

      EmplaceOrDie(&table_metrics, std::make_pair(name, value));
      if (!EmplaceIfNotPresent(host_metrics, std::make_pair(name, value))) {
        auto& host_metric = FindOrDie(*host_metrics, name);
        host_metric += value;
      }
    } else if (*known_type == "HISTOGRAM") {
      for (const auto& percentile : kRegisterPercentiles) {
        string hist_metric_name(name);
        hist_metric_name += "_" + percentile;
        int64_t total_count;
        CHECK_OK(r.ExtractInt64(metric, "total_count", &total_count));
        int64_t percentile_value;
        CHECK_OK(r.ExtractInt64(metric, percentile.c_str(), &percentile_value));
        vector<SimpleHistogram> tmp({{total_count, percentile_value}});
        EmplaceOrDie(&table_hist_metrics, std::make_pair(hist_metric_name, tmp));
        if (!EmplaceIfNotPresent(host_hist_metrics, std::make_pair(hist_metric_name, tmp))) {
          auto& host_hist_metric = FindOrDie(*host_hist_metrics, hist_metric_name);
          host_hist_metric.emplace_back(tmp[0]);
        }
      }
    } else {
      LOG(FATAL) << "Unknown metric type: " << *known_type;
    }
  }

  return Status::OK();
}

Status MetricsCollector::ParseTabletMetrics(const JsonReader& /*r*/,
                                            const rapidjson::Value* /*entity*/) {
  return Status::NotSupported("tablet entity is not supported");
}

Status MetricsCollector::CollectAndReportHostLevelMetrics(
    const string& url,
    TablesMetrics* metrics_by_table_name,
    TablesHistMetrics* hist_metrics_by_table_name) {
  MonoTime start(MonoTime::Now());
  scoped_refptr<Trace> trace(new Trace);
  ADOPT_TRACE(trace.get());
  TRACE_EVENT1("collector", "MetricsCollector::CollectAndReportHostLevelMetrics",
               "url", url);
  TRACE("init");
  CHECK(metrics_by_table_name);
  CHECK(hist_metrics_by_table_name);

  // Get metrics from server.
  string resp;
  RETURN_NOT_OK(GetMetrics(url, &resp));

  // Merge metrics by table and metric type.
  Metrics host_metrics;
  HistMetrics host_hist_metrics;
  RETURN_NOT_OK(ParseMetrics(resp, metrics_by_table_name, &host_metrics,
                             hist_metrics_by_table_name, &host_hist_metrics));

  string host_name = ExtractHostName(url);
  auto timestamp = static_cast<uint64_t>(WallTime_Now());

  // Host table level.
  if (!FLAGS_collector_ignore_hosttable_level_metrics) {
    RETURN_NOT_OK(ReportHostTableLevelMetrics(host_name, timestamp,
                                              *metrics_by_table_name,
                                              *hist_metrics_by_table_name));
  }

  // Host level.
  RETURN_NOT_OK(ReportHostLevelMetrics(host_name, timestamp,
                                       host_metrics,
                                       host_hist_metrics));

  int64_t elapsed_ms = (MonoTime::Now() - start).ToMilliseconds();
  if (elapsed_ms > FLAGS_collector_warn_threshold_ms) {
    if (Trace::CurrentTrace()) {
      LOG(WARNING) << "Trace:" << std::endl
                   << Trace::CurrentTrace()->DumpToString();
    }
  }
  return Status::OK();
}

Status MetricsCollector::ParseMetrics(const string& data,
                                      TablesMetrics* metrics_by_table_name,
                                      Metrics* host_metrics,
                                      TablesHistMetrics* hist_metrics_by_table_name,
                                      HistMetrics* host_hist_metrics) {
  JsonReader r(data);
  RETURN_NOT_OK(r.Init());
  vector<const Value*> entities;
  RETURN_NOT_OK(r.ExtractObjectArray(r.root(), nullptr, &entities));

  for (const Value* entity : entities) {
    if (FilterByAttribute(r, entity)) {
      continue;
    }
    string entity_type;
    CHECK_OK(r.ExtractString(entity, "type", &entity_type));
    if (entity_type == "server") {
      CHECK(ParseServerMetrics(r, entity).IsNotSupported());
    } else if (entity_type == "table") {
      CHECK_OK(ParseTableMetrics(r, entity,
                                 metrics_by_table_name, host_metrics,
                                 hist_metrics_by_table_name, host_hist_metrics));
    } else if (entity_type == "tablet") {
      CHECK(ParseTabletMetrics(r, entity).IsNotSupported());
    } else {
      LOG(FATAL) << "Unknown entity_type: " << entity_type;
    }
  }
  TRACE(Substitute("Metrics parsed, entity count $0", entities.size()));

  return Status::OK();
}

void MetricsCollector::CollectMetrics(const string& endpoint,
                                      const Metrics& metrics,
                                      const std::string& level,
                                      uint64_t timestamp,
                                      const std::string& extra_tags,
                                      list<scoped_refptr<ItemBase>>* items) {
  for (const auto& metric : metrics) {
    items->emplace_back(
      reporter_->ConstructItem(endpoint,
                               metric.first,
                               level,
                               timestamp,
                               metric.second,
                               FindOrDie(metric_types_by_entity_type_["tablet"], metric.first),
                               extra_tags));
  }
}

void MetricsCollector::CollectMetrics(const string& endpoint,
                      const HistMetrics& metrics,
                      const string& level,
                      uint64_t timestamp,
                      const string& extra_tags,
                      list<scoped_refptr<ItemBase>>* items) {
  for (const auto& metric : metrics) {
    items->emplace_back(
      reporter_->ConstructItem(endpoint,
                               metric.first,
                               level,
                               timestamp,
                               GetHistValue(metric.second),
                               "GAUGE",
                               extra_tags));
  }
}

Status MetricsCollector::ReportHostTableLevelMetrics(
    const string& host_name,
    uint64_t timestamp,
    const TablesMetrics& metrics_by_table_name,
    const TablesHistMetrics& hist_metrics_by_table_name) {
  list<scoped_refptr<ItemBase>> items;
  // GAUGE/COUNTER type metrics.
  int metrics_count = 0;
  for (const auto& table_metrics : metrics_by_table_name) {
    const auto extra_tag = Substitute("table=$0", table_metrics.first);
    metrics_count += table_metrics.second.size();
    CollectMetrics(host_name, table_metrics.second, "host_table", timestamp, extra_tag, &items);
  }
  TRACE(Substitute("Host-table GAUGE/COUNTER type metrics collected, count $0", metrics_count));

  // HISTOGRAM type metrics.
  int hist_metrics_count = 0;
  for (const auto& table_hist_metrics : hist_metrics_by_table_name) {
    const auto extra_tag = Substitute("table=$0", table_hist_metrics.first);
    hist_metrics_count += table_hist_metrics.second.size();
    CollectMetrics(host_name, table_hist_metrics.second,
                   "host_table", timestamp, extra_tag,
                   &items);
  }
  TRACE(Substitute("Host-table HISTOGRAM type metrics collected, count $0", hist_metrics_count));

  reporter_->PushItems(std::move(items));
  TRACE(Substitute("Host-table metrics reported, count $0", metrics_count + hist_metrics_count));

  return Status::OK();
}

Status MetricsCollector::ReportHostLevelMetrics(
    const string& host_name,
    uint64_t timestamp,
    const Metrics& host_metrics,
    const HistMetrics& host_hist_metrics) {
  list<scoped_refptr<ItemBase>> items;
  // GAUGE/COUNTER type metrics.
  CollectMetrics(host_name, host_metrics, "host", timestamp, "", &items);
  TRACE(Substitute("Host GAUGE/COUNTER type metrics collected, count $0", host_metrics.size()));

  // HISTOGRAM type metrics.
  CollectMetrics(host_name, host_hist_metrics, "host", timestamp, "", &items);
  TRACE(Substitute("Host HISTOGRAM type metrics collected, count $0", host_hist_metrics.size()));

  reporter_->PushItems(std::move(items));
  TRACE(Substitute("Host metrics reported, count $0",
                   host_metrics.size() + host_hist_metrics.size()));

  return Status::OK();
}

Status MetricsCollector::ReportTableLevelMetrics(
    uint64_t timestamp,
    const TablesMetrics& metrics_by_table_name,
    const TablesHistMetrics& hist_metrics_by_table_name) {
  list<scoped_refptr<ItemBase>> items;
  // GAUGE/COUNTER type metrics.
  int metrics_count = 0;
  for (const auto& table_metrics : metrics_by_table_name) {
    metrics_count += table_metrics.second.size();
    CollectMetrics(table_metrics.first,
                   table_metrics.second,
                   "table", timestamp, "", &items);
  }
  TRACE(Substitute("Table GAUGE/COUNTER type metrics collected, count $0", metrics_count));

  // HISTOGRAM type metrics.
  int hist_metrics_count = 0;
  for (const auto& table_hist_metrics : hist_metrics_by_table_name) {
    hist_metrics_count += table_hist_metrics.second.size();
    CollectMetrics(table_hist_metrics.first,
                   table_hist_metrics.second,
                   "table", timestamp, "", &items);
  }
  TRACE(Substitute("Table HISTOGRAM type metrics collected, count $0", hist_metrics_count));

  reporter_->PushItems(std::move(items));
  TRACE(Substitute("Table metrics reported, count $0", metrics_count + hist_metrics_count));

  return Status::OK();
}

Status MetricsCollector::ReportClusterLevelMetrics(uint64_t timestamp,
                                                   const Metrics& cluster_metrics) {
  list<scoped_refptr<ItemBase>> items;
  CollectMetrics(FLAGS_collector_cluster_name, cluster_metrics, "cluster", timestamp, "", &items);
  TRACE(Substitute("Cluster metrics collected, count $0", cluster_metrics.size()));

  reporter_->PushItems(std::move(items));
  TRACE(Substitute("Cluster metrics reported, count $0", cluster_metrics.size()));

  return Status::OK();
}

int64_t MetricsCollector::GetHistValue(const vector<SimpleHistogram>& hist_values) {
  int64_t total_count = 0;
  double total_value = 0.0;
  for (const auto& hist_value : hist_values) {
    total_count += hist_value.count;
    total_value += hist_value.count * hist_value.value;
  }
  int64_t value = 0;
  if (total_count != 0) {
    value = std::llround(total_value / total_count);
  }
  return value;
}

Status MetricsCollector::GetMetrics(const string& url, string* resp) {
  CHECK(resp);
  EasyCurl curl;
  faststring dst;
  //curl.set_return_headers(true);
  RETURN_NOT_OK(curl.FetchURL(url, &dst, {"Accept-Encoding: gzip"}));
  std::ostringstream oss;
  string dst_str = dst.ToString();
  if (zlib::Uncompress(Slice(dst_str), &oss).ok()) {
    *resp = oss.str();
  } else {
    *resp = dst_str;
  }
  TRACE("Metrics got from server");

  return Status::OK();
}
} // namespace collector
} // namespace kudu
