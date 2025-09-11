#include <cstdint>
#include <doca_error.h>
#include <doca_log.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <doca_erasure_coding.h>

#include "common.h"
#include "ec.h"

DOCA_LOG_REGISTER(COMMON : EC)

doca_error_t initEc(doca_dev *aDev, doca_pe *aPe, uint32_t aNbDataBlks,
                    uint32_t aNbRdncBlks, uint32_t *aMissingIdxs,
                    uint32_t aNbMissingIdxs,
                    doca_ec_task_create_completion_cb_t aEncSuccCb,
                    doca_ec_task_create_completion_cb_t aEncErrCb,
                    doca_ec_task_recover_completion_cb_t aDecSuccCb,
                    doca_ec_task_recover_completion_cb_t aDecErrCb,
                    doca_ec *&aEc, doca_ec_matrix *&aEncMat,
                    doca_ec_matrix *&aDecMat, doca_ctx *&aCtx) {
    CHECK_RETURN(doca_ec_create(aDev, &aEc), "create ec");
    CHECK_RETURN(doca_ec_matrix_create(aEc, DOCA_EC_MATRIX_TYPE_CAUCHY,
                                       aNbDataBlks, aNbRdncBlks, &aEncMat),
                 "create encoding matrix");
    if (aEncSuccCb && aEncErrCb) {
        CHECK_RETURN(
            doca_ec_task_create_set_conf(aEc, aEncSuccCb, aEncErrCb, 8192),
            "set ec create conf");
    }

    if (aDecSuccCb && aDecErrCb) {
        CHECK_RETURN(doca_ec_matrix_create_recover(aEc, aEncMat, aMissingIdxs,
                                                   aNbMissingIdxs, &aDecMat),
                     "create decoding matrix");
        CHECK_RETURN(
            doca_ec_task_recover_set_conf(aEc, aDecSuccCb, aDecErrCb, 8192),
            "set ec recover conf");
    }

    aCtx = doca_ec_as_ctx(aEc);

    CHECK_RETURN(doca_pe_connect_ctx(aPe, aCtx), "connect pe to ec ctx");

    CHECK_RETURN(doca_ctx_start(aCtx), "start ec ctx");
    return DOCA_SUCCESS;
}

void destroyEc(doca_ec_matrix *aEncMat, doca_ec_matrix *aDecMat, doca_ec *aEc,
               doca_ctx *aCtx) {
    CHECK_LOG(doca_ec_matrix_destroy(aEncMat), "destroy encoding matrix");

    if (aDecMat) {
        CHECK_LOG(doca_ec_matrix_destroy(aDecMat), "destroy decoding matrix");
    }

    CHECK_LOG(doca_ctx_stop(aCtx), "stop ec ctx");

    CHECK_LOG(doca_ec_destroy(aEc), "destroy ec");
}
