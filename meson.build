project(
    'Astraea',
    'CPP',
    default_options: ['cpp_std=c++20'],
    version: '0.9.0',
)

add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: 'CPP')

doca_common_dep = dependency('doca-common')
doca_argp_dep = dependency('doca-argp')
doca_rdma_dep = dependency('doca-rdma')
doca_ec_dep = dependency('doca-erasure-coding')
dl_dep = dependency('dl')

# Reuseable codes for profiling, microbenchmark and test apps (not used by Astraea)
subdir('src/common')

# Astraea Broker & Scheduler
subdir('src/broker')
subdir('src/scheduler')

# Profiling
subdir('src/profiling')

# Microbenchmark
subdir('src/microbenchmark')

# Test applications
subdir('src/test')