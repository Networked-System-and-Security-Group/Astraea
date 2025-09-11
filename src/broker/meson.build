astraea_rscs = [
    'Buf.cc',
    'Ctx.cc',
    'Ec.cc',
    'Rdma.cc',
    'Pe.cc',
    'Task.cc',
    'initialize.cc',
]
shared_library(
    'astraea_broker',
    astraea_rscs,
    dependencies: [
        doca_common_dep,
        doca_ec_dep,
        doca_rdma_dep,
        dl_dep,
    ],
)