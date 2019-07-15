#!/bin/bash

PID=$$
BASE_DIR="$( cd "$( dirname "$0" )" && pwd )"
BIN_PATH=${KUDU_HOME}/kudu
if [[ ! -f ${BIN_PATH} ]]; then
  echo "ERROR: kudu not found in ${BIN_PATH}"
  exit 1
fi

if [[ $# -ne 2 ]]
then
  echo "This tool is for update falcon screen for specified kudu cluster."
  echo "USAGE: $0 <cluster_name> <table_count>"
  exit 1
fi

CLUSTER=$1
TABLE_COUNT=$2

echo "UID: ${UID}"
echo "PID: ${PID}"
echo "cluster: ${CLUSTER}"
echo "top n table: ${TABLE_COUNT}"
echo "Start time: `date`"
ALL_START_TIME=$((`date +%s`))
echo

# get master list
${BIN_PATH} master list @${CLUSTER} -format=space | awk -F' |:' '{print $2}' | sort -n &>/tmp/${UID}.${PID}.kudu.master.list
if [[ $? -ne 0 ]]; then
    echo "`kudu master list @${CLUSTER} -format=space` failed"
    exit $?
fi

MASTER_COUNT=`cat /tmp/${UID}.${PID}.kudu.master.list | wc -l`
if [[ ${MASTER_COUNT} -eq 0 ]]; then
    echo "ERROR: master list is empty, please check the cluster ${CLUSTER}"
    exit -1
fi

# get tserver list
${BIN_PATH} tserver list @${CLUSTER} -format=space | awk -F' |:' '{print $2}' | sort -n &>/tmp/${UID}.${PID}.kudu.tserver.list
if [[ $? -ne 0 ]]; then
    echo "`kudu tserver list @${CLUSTER} -format=space` failed"
    exit $?
fi

TSERVER_COUNT=`cat /tmp/${UID}.${PID}.kudu.tserver.list | wc -l`
if [[ ${TSERVER_COUNT} -eq 0 ]]; then
    echo "ERROR: tserver list is empty, please check the cluster ${CLUSTER}"
    exit 1
fi

# get table list
python ${BASE_DIR}/kudu_metrics_collector_for_falcon.py --cluster_name=${CLUSTER} --falcon_url= --metrics=bytes_flushed,on_disk_data_size,scanner_rows_returned --local_stat > /tmp/${UID}.${PID}.kudu.metric_table_value
if [[ $? -ne 0 ]]; then
    echo "ERROR: kudu_metrics_collector_for_falcon.py execute failed"
    exit 1
fi

cat /tmp/${UID}.${PID}.kudu.metric_table_value | egrep "^bytes_flushed" | sort -rnk3 | head -n ${TABLE_COUNT} | awk '{print $2}' > /tmp/${UID}.${PID}.kudu.top.bytes_flushed
cat /tmp/${UID}.${PID}.kudu.metric_table_value | egrep "^on_disk_data_size" | sort -rnk3 | head -n ${TABLE_COUNT} | awk '{print $2}' > /tmp/${UID}.${PID}.kudu.top.on_disk_data_size
cat /tmp/${UID}.${PID}.kudu.metric_table_value | egrep "^scanner_rows_returned" | sort -rnk3 | head -n ${TABLE_COUNT} | awk '{print $2}' > /tmp/${UID}.${PID}.kudu.top.scanner_rows_returned
cat /tmp/${UID}.${PID}.kudu.top.* | sort -n | uniq > /tmp/${UID}.${PID}.kudu.table.list
echo "total `wc -l /tmp/${UID}.${PID}.kudu.table.list | awk '{print $1}'` tables to monitor"
echo -e "\033[32m Please set the following one line to the kudu collector's \`tables\` argument manually\033[0m"
awk BEGIN{RS=EOF}'{gsub(/\n/,",");print}' /tmp/${UID}.${PID}.kudu.table.list
echo ""

python ${BASE_DIR}/falcon_screen.py ${CLUSTER} ${BASE_DIR}/falcon_screen.json /tmp/${UID}.${PID}.kudu.master.list /tmp/${UID}.${PID}.kudu.tserver.list /tmp/${UID}.${PID}.kudu.table.list
if [[ $? -ne 0 ]]; then
    echo "ERROR: falcon screen operate failed"
    exit 1
fi

echo
echo "Finish time: `date`"
ALL_FINISH_TIME=$((`date +%s`))
echo "Falcon screen operate done, elapsed time is $((ALL_FINISH_TIME - ALL_START_TIME)) seconds."

rm -f /tmp/${UID}.${PID}.kudu.* &>/dev/null
