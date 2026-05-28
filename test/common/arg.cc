#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>

#include "common.h"

DOCA_LOG_REGISTER(COMMON : ARG)

doca_error_t registerOneParam(const char *aShortName, const char *aDescription,
                              doca_argp_type aType,
                              doca_argp_param_cb_t aCallback) {
    doca_argp_param *param;
    CHECK_RETURN(doca_argp_param_create(&param), "create param");
    doca_argp_param_set_short_name(param, aShortName);
    doca_argp_param_set_description(param, aDescription);
    doca_argp_param_set_type(param, aType);
    doca_argp_param_set_callback(param, aCallback);
    CHECK_RETURN(doca_argp_register_param(param), "register param");

    return DOCA_SUCCESS;
}