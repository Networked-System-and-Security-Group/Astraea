project('Astraea', 'cpp', default_options: ['cpp_std=c++20'])
doca_common_dep = dependency('doca-common')
doca_argp_dep = dependency('doca-argp')
doca_ec_dep = dependency('doca-erasure-coding')
thread_dep = dependency('threads')

add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: 'cpp')

# lib should be built before building sample to avoid undefined dependency error
subdir('src/lib')

subdir('src/profiling')
subdir('src/scheduler')
subdir('src/example')