#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>

#include "ec_create.h"

DOCA_LOG_REGISTER(EC_CREATE : MAIN);

static doca_error_t register_param(const char *short_name,
                                   const char *long_name,
                                   const char *description,
                                   doca_argp_param_cb_t callback,
                                   doca_argp_type type) {
    doca_error_t result;
    doca_argp_param *param;
    result = doca_argp_param_create(&param);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create argp param: %s",
                     doca_error_get_descr(result));
        return result;
    }
    doca_argp_param_set_short_name(param, short_name);
    doca_argp_param_set_long_name(param, long_name);
    doca_argp_param_set_description(param, description);
    doca_argp_param_set_callback(param, callback);
    doca_argp_param_set_type(param, type);
    result = doca_argp_register_param(param);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to register argp param: %s",
                     doca_error_get_descr(result));
    }

    return result;
}

doca_error_t register_ec_create_params() {
    doca_error_t status;
    status = register_param(
        "nd", "nb_data_blocks", "number of data blocks",
        [](void *param, void *config) -> doca_error_t {
            ec_create_config *cfg = (ec_create_config *)config;
            uint32_t nb_data_blocks = *(uint32_t *)param;
            cfg->nb_data_blocks = nb_data_blocks;
            return DOCA_SUCCESS;
        },
        DOCA_ARGP_TYPE_INT);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to register nd param: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = register_param(
        "nr", "nb_rdnc_blocks", "number of rdnc blocks",
        [](void *param, void *config) -> doca_error_t {
            ec_create_config *cfg = (ec_create_config *)config;
            uint32_t nb_rdnc_blocks = *(uint32_t *)param;
            cfg->nb_rdnc_blocks = nb_rdnc_blocks;
            return DOCA_SUCCESS;
        },
        DOCA_ARGP_TYPE_INT);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to register nr param: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = register_param(
        "s", "block_size", "block size",
        [](void *param, void *config) -> doca_error_t {
            ec_create_config *cfg = (ec_create_config *)config;
            uint32_t block_size = *(uint32_t *)param;
            cfg->block_size = block_size;
            return DOCA_SUCCESS;
        },
        DOCA_ARGP_TYPE_INT);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to register size param: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = register_param(
        "nt", "nb_tasks", "number of tasks",
        [](void *param, void *config) -> doca_error_t {
            ec_create_config *cfg = static_cast<ec_create_config *>(config);
            uint32_t nb_tasks = *static_cast<uint32_t *>(param);
            cfg->nb_tasks = nb_tasks;
            return DOCA_SUCCESS;
        },
        DOCA_ARGP_TYPE_INT);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to register nt param: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = register_param(
        "lat", "latency", "latency sla",
        [](void *param, void *config) -> doca_error_t { return DOCA_SUCCESS; },
        DOCA_ARGP_TYPE_INT);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to register latency param: %s",
                     doca_error_get_descr(status));
        return status;
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

    /* Setup argp */
    ec_create_config cfg = {.nb_data_blocks = 128,
                            .nb_rdnc_blocks = 32,
                            .block_size = 1024,
                            .nb_tasks = 1};

    status = doca_argp_init("ec_create", &cfg);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init argp: %s", doca_error_get_descr(status));
        return EXIT_FAILURE;
    }

    status = register_ec_create_params();
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to register ec create params");
        doca_argp_destroy();
        return EXIT_FAILURE;
    }

    status = doca_argp_start(argc, argv);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to parse parameters: %s",
                     doca_error_get_descr(status));
        doca_argp_destroy();
        return EXIT_FAILURE;
    }

    status = ec_create(cfg);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("EC create failed");
        doca_argp_destroy();
        return EXIT_FAILURE;
    }

    doca_argp_destroy();
    return EXIT_SUCCESS;
}