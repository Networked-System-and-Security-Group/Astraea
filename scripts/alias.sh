alias sc="taskset -c 15 ./build/scheduler"

alias as="taskset -c 1 env LD_PRELOAD=build/libastraea_broker.so SLA=300 ./build/microbenchmark 8192"
alias ab="taskset -c 3 env LD_PRELOAD=build/libastraea_broker.so SLA=3000 ./build/microbenchmark 65536"
alias ds="taskset -c 1 ./build/microbenchmark 8192"
alias db="taskset -c 3 ./build/microbenchmark 65536"

alias cc=./build/cdn_client
alias rc=./build/replica_client
alias mh=./build/memscan_host
alias lh=./build/localec_host


alias ac="taskset -c 1-3 env LD_PRELOAD=./build/libastraea_broker.so SLA=150 ./build/cdn_dpu -r 50000"
alias ar="taskset -c 4-6 env LD_PRELOAD=./build/libastraea_broker.so SLA=8000 ./build/replica_dpu"
alias al="taskset -c 7-9 env LD_PRELOAD=./build/libastraea_broker.so SLA=50 ./build/localec_dpu"
alias am="taskset -c 10-12 env LD_PRELOAD=./build/libastraea_broker.so SLA=8000 ./build/memscan_dpu"


alias dc="taskset -c 1-3 ./build/cdn_dpu -r 50000"
alias dr="taskset -c 4-6 ./build/replica_dpu"
alias dl="taskset -c 7-9 ./build/localec_dpu"
alias dm="taskset -c 10-12 ./build/memscan_dpu"