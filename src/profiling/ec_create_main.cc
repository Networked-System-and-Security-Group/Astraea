#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <doca_error.h>
#include <doca_log.h>

#include "ec_create.h"

DOCA_LOG_REGISTER(EC_CREATE : MAIN);

constexpr uint32_t nb_data_blocks_arr[] = {1, 2, 4, 8, 16, 32, 64, 128};
constexpr uint32_t nb_rdnc_blocks_arr[] = {1, 2, 4, 8, 16, 32};
constexpr size_t block_size_arr[] = {1024,   2048,   4096,   8192,
                                     16384,  32768,  65536,  131072,
                                     262144, 524288, 1048576};

static doca_error_t profile() {
    doca_error_t status;
    for (uint32_t i = 0; i < sizeof(nb_data_blocks_arr) / sizeof(uint32_t);
         i++) {
        for (uint32_t j = 0; j < sizeof(nb_rdnc_blocks_arr) / sizeof(uint32_t);
             j++) {
            for (uint32_t k = 0; k < sizeof(block_size_arr) / sizeof(size_t);
                 k++) {
                uint32_t nb_data_blocks = nb_data_blocks_arr[i];
                uint32_t nb_rdnc_blocks = nb_rdnc_blocks_arr[j];
                size_t block_size = block_size_arr[k];
                ec_create_config cfg = {.nb_data_blocks = nb_data_blocks,
                                        .nb_rdnc_blocks = nb_rdnc_blocks,
                                        .block_size = block_size,
                                        .nb_tasks = 32};
                status = ec_create(cfg);
                if (status != DOCA_SUCCESS) {
                    DOCA_LOG_ERR("EC create failed when ");
                    return status;
                }
            }
        }
    }
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    doca_error_t status;

    /* Setup SDK logger */
    doca_log_backend *sdk_log;
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