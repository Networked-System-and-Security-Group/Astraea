#ifndef EC_H_
#define EC_H_

#include <cstdint>
#include <doca_error.h>
#include <doca_log.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <doca_erasure_coding.h>

doca_error_t initEc(doca_dev *aDev, doca_pe *aPe, uint32_t aNbDataBlks,
                    uint32_t aNbRdncBlks, uint32_t *aMissingIdxs,
                    uint32_t aNbMissingIdxs,
                    doca_ec_task_create_completion_cb_t aEncSuccCb,
                    doca_ec_task_create_completion_cb_t aEncErrCb,
                    doca_ec_task_recover_completion_cb_t aDecSuccCb,
                    doca_ec_task_recover_completion_cb_t aDecErrCb,
                    doca_ec *&aEc, doca_ec_matrix *&aEncMat,
                    doca_ec_matrix *&aDecMat, doca_ctx *&aCtx);

void destroyEc(doca_ec_matrix *aEncMat, doca_ec_matrix *aDecMat, doca_ec *aEc,
               doca_ctx *aCtx);
#endif