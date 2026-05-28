#pragma once

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <cstdint>

constexpr uint32_t MAX_NB_DMA_TASKS = 1024;

doca_error_t initDma(doca_dev *dev, doca_pe *pe,
                     doca_dma_task_memcpy_completion_cb_t succCB,
                     doca_dma_task_memcpy_completion_cb_t errCb, doca_dma *&dma,
                     doca_ctx *&ctx);

void destroyDma(doca_dma *dma, doca_ctx *ctx);

/* Host calls this to export its mmap descriptor and send it to the DPU server */
doca_error_t dmaExportToServer(doca_dev *aDev, const char *aServerIp,
                                uint16_t aPort, doca_mmap *aMmap);

/* DPU calls this to listen for the host and import the received mmap descriptor */
doca_error_t dmaImportFromClient(uint16_t aPort, doca_dev *aDev,
                                  doca_mmap *&oHostMmap);
