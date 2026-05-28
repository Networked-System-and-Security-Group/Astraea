alias cc=./build/cdn_client
alias rc=./build/replica_client
alias sc="taskset -c 7 ./build/scheduler"
alias ac="taskset -c 1-3 env LD_PRELOAD=./build/libastraea_broker.so SLA=269 ./build/cdn_dpu -r 50000"
alias ar="taskset -c 4-6 env LD_PRELOAD=./build/libastraea_broker.so SLA=1250 ./build/replica_dpu"
alias dc="taskset -c 1-3 ./build/cdn_dpu -r 50000"
alias dr="taskset -c 4-6 ./build/replica_dpu"
alias as="taskset -c 1 env LD_PRELOAD=build/libastraea_broker.so SLA=300 ./build/microbenchmark 8192"
alias ab="taskset -c 3 env LD_PRELOAD=build/libastraea_broker.so SLA=3000 ./build/microbenchmark 65536"
alias ds="taskset -c 1 ./build/microbenchmark 8192"
alias db="taskset -c 3 ./build/microbenchmark 65536"
