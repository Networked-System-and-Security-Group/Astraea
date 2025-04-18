#include <cstdio>
#include <cstdlib>

#include <doca_error.h>
#include <doca_log.h>

#include "astraea_scheduler.h"

DOCA_LOG_REGISTER(ASTRAEA:SCHEDULER : MAIN);

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

    astraea_scheduler scheduler{&status};
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to init scheduler");
        return EXIT_FAILURE;
    }

    DOCA_LOG_INFO("Astraea scheduler started");
    scheduler.run();

    return EXIT_SUCCESS;
}