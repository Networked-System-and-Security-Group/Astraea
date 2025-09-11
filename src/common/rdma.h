#ifndef RDMA_H_
#define RDMA_H_

#include <cstdint>
#include <vector>

#include <doca_dev.h>
#include <doca_error.h>

#include <doca_ctx.h>
#include <doca_pe.h>

#include <doca_mmap.h>

#include <doca_rdma.h>

#include "common.h"

/* This is unlimited actually */
constexpr uint32_t MAX_NB_RDMA_TASKS = 8192;

doca_error_t initRdma(uint32_t aGidIdx, doca_dev *aDev, doca_pe *aPe,
                      doca_rdma_task_read_completion_cb_t aReadSuccCb,
                      doca_rdma_task_read_completion_cb_t aReadErrCb,
                      doca_rdma_task_write_completion_cb_t aWriteSuccCb,
                      doca_rdma_task_write_completion_cb_t aWriteErrCb,
                      doca_rdma *&aRdma, doca_ctx *&aCtx);

void destroyRdma(doca_rdma *aRdma, doca_ctx *aCtx);
doca_error_t rdmaConnect(doca_dev *aDev, const char *aIpAddr, uint16_t aPort,
                         doca_rdma *aRdma, doca_rdma_connection *&aConn,
                         doca_mmap *&remoteMmap);
#endif