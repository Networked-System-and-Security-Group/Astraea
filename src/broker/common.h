#pragma once

#include <doca_dev.h>
#include <doca_error.h>

#include <cstdint>

#include "shm.h"

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

extern u32 gSla;
extern u32 gShmFd;
extern SharedData *gSharedData;
extern u32 gAppId;

// Pass -l 60 in cmd to enable DBG log
#define CHECK_RETURN(statement, error_msg)              \
    do {                                                \
        doca_error_t status = (statement);              \
        if (status != DOCA_SUCCESS) {                   \
            DOCA_LOG_ERR("Failed to %s: %s", error_msg, \
                         doca_error_get_descr(status)); \
            return status;                              \
        } else {                                        \
            DOCA_LOG_DBG("Successfully %s", error_msg); \
        }                                               \
    } while (0)

#define CHECK_LOG(statement, error_msg)                 \
    do {                                                \
        doca_error_t status = (statement);              \
        if (status != DOCA_SUCCESS) {                   \
            DOCA_LOG_ERR("Failed to %s: %s", error_msg, \
                         doca_error_get_descr(status)); \
        }                                               \
    } while (0)
