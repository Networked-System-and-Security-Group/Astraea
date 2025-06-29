ag_encrypt_sources = ['lz4_decomp_main.cc', 'lz4_decomp_core.cc', 'lz4_decomp_resources.cc']
executable(
    'lz4_decomp_doca',
    ag_encrypt_sources,
    dependencies: [doca_common_dep, doca_argp_dep, doca_comp_dep],
)