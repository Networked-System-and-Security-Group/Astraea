alias cc=./build/src/test/cdn_client/cdn_client
alias rc=./build/src/test/replica_client/replica_client
alias sc="taskset -c 7 ./build/src/scheduler/scheduler"
alias ac="taskset -c 1-3 env LD_PRELOAD=./build/src/broker/libastraea_broker.so SLA=180 ./build/src/test/cdn_dpu/cdn_dpu -r 10000"
alias ar="taskset -c 4-6 env LD_PRELOAD=./build/src/broker/libastraea_broker.so SLA=1860 ./build/src/test/replica_dpu/replica_dpu"
alias dc="taskset -c 1-3 ./build/src/test/cdn_dpu/cdn_dpu -r 10000"
alias dr="taskset -c 4-6 ./build/src/test/replica_dpu/replica_dpu"
alias as="taskset -c 1 env LD_PRELOAD=build/src/broker/libastraea_broker.so SLA=300 ./build/src/microbenchmark/microbenchmark 8192"
alias ab="taskset -c 3 env LD_PRELOAD=build/src/broker/libastraea_broker.so SLA=3000 ./build/src/microbenchmark/microbenchmark 65536"
alias ds="taskset -c 1 ./build/src/microbenchmark/microbenchmark 8192"
alias db="taskset -c 3 ./build/src/microbenchmark/microbenchmark 65536"