#ifndef RDMA_H_
#define RDMA_H_

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <cstdint>

/* This is unlimited actually */
constexpr uint32_t MAX_NB_RDMA_TASKS = 1024;

doca_error_t initRdma(uint32_t aGidIdx, doca_dev *aDev, doca_pe *aPe,
                      doca_rdma_task_read_completion_cb_t aReadSuccCb,
                      doca_rdma_task_read_completion_cb_t aReadErrCb,
                      doca_rdma_task_write_completion_cb_t aWriteSuccCb,
                      doca_rdma_task_write_completion_cb_t aWriteErrCb,
                      doca_rdma_task_receive_completion_cb_t aRecvSuccCb,
                      doca_rdma_task_receive_completion_cb_t aRecvErrCb,
                      doca_rdma_task_write_imm_completion_cb_t aImmSuccCb,
                      doca_rdma_task_write_imm_completion_cb_t aImmErrCb,
                      doca_rdma *&aRdma, doca_ctx *&aCtx);

void destroyRdma(doca_rdma *aRdma, doca_ctx *aCtx, doca_pe *aPe);

doca_error_t rdmaConnectToServer(doca_dev *aDev, const char *aIpAddr,
                                 uint16_t aPort, doca_rdma *aRdma,
                                 doca_mmap *aMmap, uint16_t aThreadId,
                                 doca_rdma_connection *&aConn);

doca_error_t rdmaConnectToClient(doca_dev *aDev, uint16_t aPort,
                                 doca_rdma *aRdma, uint16_t aThreadId,
                                 doca_rdma_connection *&aConn,
                                 doca_mmap *&remoteMmap);
#endif