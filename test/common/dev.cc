#include <cstdint>
#include <cstring>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"

DOCA_LOG_REGISTER(COMMON : DEV)

doca_error_t openDev(const char *aIbdevName, doca_dev *&oDev) {
    doca_devinfo **devinfoList;
    uint32_t nbDevs;
    CHECK_RETURN(doca_devinfo_create_list(&devinfoList, &nbDevs),
                 "create devinfo list");
    for (uint32_t i = 0; i < nbDevs; i++) {
        char ibdevName[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
        CHECK_RETURN(doca_devinfo_get_ibdev_name(devinfoList[i], ibdevName,
                                                 DOCA_DEVINFO_IBDEV_NAME_SIZE),
                     "get ibdev name");

        if (!strcmp(ibdevName, aIbdevName)) {
            CHECK_RETURN(doca_dev_open(devinfoList[i], &oDev), "open dev");
            CHECK_RETURN(doca_devinfo_destroy_list(devinfoList),
                         "destroy devinfo list");
            return DOCA_SUCCESS;
        }
    }

    CHECK_RETURN(doca_devinfo_destroy_list(devinfoList),
                 "destroy devinfo list");

    return DOCA_ERROR_NOT_FOUND;
}