#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <doca_error.h>
#include <doca_log.h>

#include "ag_encrypt.h"

DOCA_LOG_REGISTER(EC_CREATE : MAIN);

constexpr uint32_t plaintext_size_arr[] = { 16,
                                            32,
                                            64,
                                            128,
                                            256,
                                            512,
                                            1024,
                                            2048,
                                            4096,
                                            8192,
                                            16384,
                                            32768,
                                            65536,
                                            131072,
                                            262144,
                                            524288,
                                            1048576 -
                                              DEFAULT_TAG_SIZE_IN_BYTES };
constexpr uint32_t aad_size_arr[] = { 16,
                                      32,
                                      64,
                                      128,
                                      256,
                                      512,
                                      1024,
                                      2048,
                                      4096,
                                      8192,
                                      16384,
                                      32768,
                                      65536,
                                      131072,
                                      262144,
                                      524288,
                                      1048576 - DEFAULT_TAG_SIZE_IN_BYTES };
constexpr uint32_t nb_tasks_arr[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };

static doca_error_t
profile()
{
    doca_error_t status;
    for (uint32_t i = 0; i < 1;
         i++) {
        for (uint32_t j = 0; j < 1; j++) {
            uint32_t nb_data_blocks = 1048576 - 12;
            uint32_t nb_rdnc_blocks = 1048576 - 12;
            lz4_decomp_config cfg = { .plaintext_size = nb_data_blocks,
                                      .aad_size = nb_rdnc_blocks,
                                      .nb_tasks = 32 };
            status = lz4_decomp(cfg);
            if (status != DOCA_SUCCESS) {
                DOCA_LOG_ERR("AG encrypt failed when ");
                return status;
            }
        }
    }

    return DOCA_SUCCESS;
}

static doca_error_t
profile_pipeline()
{
    doca_error_t status;
    for (uint32_t i = 0; i < sizeof(nb_tasks_arr) / sizeof(uint32_t); i++) {
        lz4_decomp_config cfg = { .plaintext_size = 1024 * 1024,
                                  .aad_size = 0,
                                  .nb_tasks = nb_tasks_arr[i] };
        status = lz4_decomp(cfg);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("AG encrypt failed when ");
            return status;
        }
    }

    return DOCA_SUCCESS;
}

int
main(int argc, char** argv)
{
    doca_error_t status;

    /* Setup SDK logger */
    doca_log_backend* sdk_log;
    status = doca_log_backend_create_standard();
    if (status != DOCA_SUCCESS) {
        printf("Failed to create log standard backend: %s\n",
               doca_error_get_descr(status));
        return EXIT_FAILURE;
    }

    status = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    if (status != DOCA_SUCCESS) {
        printf("Failed to create log backend with file sdk: %s\n",
               doca_error_get_descr(status));
        return EXIT_FAILURE;
    }

    status = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
    if (status != DOCA_SUCCESS) {
        printf("Failed to set log backend level: %s",
               doca_error_get_descr(status));
        return EXIT_FAILURE;
    }

    status = profile();
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Profiling failed");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
