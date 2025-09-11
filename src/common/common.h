#ifndef COMMON_H_
#define COMMON_H_

#include <cstdint>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

// Pass -l 60 in cmd to enable DBG log
#define CHECK_RETURN(statement, error_msg)                                     \
    do {                                                                       \
        doca_error_t status = (statement);                                     \
        if (status != DOCA_SUCCESS) {                                          \
            DOCA_LOG_ERR("Failed to %s: %s", error_msg,                        \
                         doca_error_get_descr(status));                        \
            return status;                                                     \
        } else {                                                               \
            DOCA_LOG_DBG("Successfully %s", error_msg);                        \
        }                                                                      \
    } while (0)

#define CHECK_LOG(statement, error_msg)                                        \
    do {                                                                       \
        doca_error_t status = (statement);                                     \
        if (status != DOCA_SUCCESS) {                                          \
            DOCA_LOG_ERR("Failed to %s: %s", error_msg,                        \
                         doca_error_get_descr(status));                        \
        }                                                                      \
    } while (0)

// Log
doca_error_t registerLogger(uint32_t aSdkLogLevel);

// Param
doca_error_t registerOneParam(const char *aShortName, const char *aDescription,
                              doca_argp_type aType,
                              doca_argp_param_cb_t aCallback);

// Device
doca_error_t openDev(const char *aIbdevName, doca_dev *&oDev);

#endif