astraea_sources = ['astraea_pe.cc', 'astraea_ec.cc', 'astraea_ctx.cc', 'resource_mgmt.cc']

astraea_library = library(
    'astraea',
    astraea_sources,
    include_directories: '.',
    dependencies: [doca_argp_dep, doca_common_dep, doca_ec_dep, thread_dep],
)

astraea_dep = declare_dependency(include_directories: '.', link_with: astraea_library)