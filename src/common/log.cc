#include <cstdint>
#include <cstdio>

#include <doca_error.h>
#include <doca_log.h>

#include "common.h"

doca_error_t registerLogger(uint32_t aSdkLogLevel) {
    doca_error_t status;

    status = doca_log_backend_create_standard();
    if (status != DOCA_SUCCESS) {
        printf("Failed to create log backend for app: %s",
               doca_error_get_descr(status));
        return DOCA_ERROR_IO_FAILED;
    }

    doca_log_backend *sdkLog;
    status = doca_log_backend_create_with_file_sdk(stderr, &sdkLog);
    if (status != DOCA_SUCCESS) {
        printf("Failed to create log backedn for SDK: %s",
               doca_error_get_descr(status));
        return DOCA_ERROR_IO_FAILED;
    }

    status = doca_log_backend_set_sdk_level(sdkLog, aSdkLogLevel);
    if (status != DOCA_SUCCESS) {
        printf("Failed to set sdk log backend level: %s",
               doca_error_get_descr(status));
        return DOCA_ERROR_IO_FAILED;
    }

    return status;
}