#pragma once

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <cstdint>
#include <vector>

using u32 = uint32_t;

struct alignas(64) MemscanRscs;

struct PageCtx {
    MemscanRscs &rscs;
    u32 pageId;
    void *dstAddr;   /* base of this page in local DPU memory, for inspection */
    doca_buf *dstBuf; /* to reset data_len after each use */
};

struct alignas(64) MemscanRscs {
    doca_pe *pe;
    doca_dev *dev;
    doca_dma *dma;
    doca_ctx *dmaCtx;

    doca_buf_inventory *bufInv;
    doca_mmap *localMmap;
    void *localMemAddr;
    doca_mmap *hostMmap;

    std::vector<doca_buf *> srcBufs;  /* from hostMmap, one per page */
    std::vector<doca_buf *> dstBufs;  /* from localMmap, one per page */
    std::vector<PageCtx> pageCtxs;

    u32 nbCompletedInBurst;
    uint16_t port;

    std::vector<double> burstJcts; /* per-burst JCT in microseconds */
};

struct alignas(64) MemscanCfg {
    char ibdevName[1024];
    u32 tScanMs;      /* burst period in milliseconds */
    u32 nPages;       /* DMA read tasks per burst */
    u32 regionSizeKb; /* bytes per task = regionSizeKb * 1024 */
    u32 nbBursts;     /* total bursts to run; 0 = unlimited */
    uint16_t port;    /* port to listen on for host mmap descriptor */
};

doca_error_t init(const MemscanCfg &aCfg, MemscanRscs &aRscs);
void destroy(MemscanRscs &aRscs);
void runTasks(const MemscanCfg &aCfg, MemscanRscs &aRscs);
