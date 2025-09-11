common_lib = library(
    'common_lib',
    [
        'arg.cc',
        'dev.cc',
        'log.cc',
        'rdma.cc',
        'memory.cc',
        'socket.cc',
        'ec.cc',
    ],
    dependencies: [
        doca_common_dep,
        doca_argp_dep,
        doca_rdma_dep,
        doca_ec_dep,
    ],
)

common_lib_dep = declare_dependency(include_directories: '.', link_with: common_lib)