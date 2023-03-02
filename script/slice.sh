#!/bin/bash

##################### Load config
if [ ! -e ../config/env.config ]; then
    echo "env.config file not exists! Please make a copy from env.config.bak"
    exit 1
else
    . ../config/env.config
fi


##################### PMU
benchmark="bc"
dist="1"
count="$1"

pid=$(ps -ef | grep "./"$benchmark | grep -v 'grep' | awk '{print $2}')
taskset -pc 0 $pid
(time $pmu_path/toplev -l3 --no-desc --user --core C0 sleep 20 -o $results_path/pmu_analysis/pmu_analysis_$count.txt) 2> time_$count.out
# ($pmu_path/toplev -l3 --no-desc --user --core C0) 2> $results_path/pmu_analysis/pmu_analysis_$count.txt
exit

