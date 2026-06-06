#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <time.h>

#include <chrono>
#include <cstdlib>

#include "common.h"
#include "dma.h"
#include "memory.h"
#include "memscan_dpu.h"

DOCA_LOG_REGISTER(MEMSCAN:DPU : CORE);

extern bool gForceQuit;

static void memcpySuccCb(doca_dma_task_memcpy *task, doca_data task_user_data,
                         doca_data ctx_user_data) {
    PageCtx *ctx = static_cast<PageCtx *>(task_user_data.ptr);
    /* Trivial inspection: touch first byte of the transferred region */
    volatile char *p = static_cast<volatile char *>(ctx->dstAddr);
    (void)p[0];
    /* Reset dst buf so it can be reused next burst */
    CHECK_LOG(doca_buf_set_data_len(ctx->dstBuf, 0), "reset dst buf data len");
    ctx->rscs.nbCompletedInBurst++;
    doca_task_free(doca_dma_task_memcpy_as_task(task));
}

static void memcpyErrCb(doca_dma_task_memcpy *task, doca_data task_user_data,
                        doca_data ctx_user_data) {
    PageCtx *ctx = static_cast<PageCtx *>(task_user_data.ptr);
    CHECK_LOG(doca_buf_set_data_len(ctx->dstBuf, 0), "reset dst buf data len");
    ctx->rscs.nbCompletedInBurst++;
    doca_task_free(doca_dma_task_memcpy_as_task(task));
    DOCA_LOG_ERR("DMA memcpy task %u failed", ctx->pageId);
}

static doca_error_t initBufs(const MemscanCfg &aCfg, MemscanRscs &aRscs) {
    size_t regionBytes = static_cast<size_t>(aCfg.regionSizeKb) * 1024;

    void *hostMemBase;
    size_t hostMemSize;
    CHECK_RETURN(
        doca_mmap_get_memrange(aRscs.hostMmap, &hostMemBase, &hostMemSize),
        "get host mmap range");

    char *hostBase = static_cast<char *>(hostMemBase);
    char *localBase = static_cast<char *>(aRscs.localMemAddr);

    for (u32 i = 0; i < aCfg.nPages; i++) {
        doca_buf *srcBuf, *dstBuf;

        /* src: host memory — use by_data to mark the valid data region */
        CHECK_RETURN(doca_buf_inventory_buf_get_by_data(
                         aRscs.bufInv, aRscs.hostMmap,
                         hostBase + i * regionBytes, regionBytes, &srcBuf),
                     "get src buf from host mmap");

        /* dst: local DPU memory — use by_addr; DMA fills it */
        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.localMmap,
                         localBase + i * regionBytes, regionBytes, &dstBuf),
                     "get dst buf from local mmap");

        aRscs.srcBufs.push_back(srcBuf);
        aRscs.dstBufs.push_back(dstBuf);
        aRscs.pageCtxs.push_back({.rscs = aRscs,
                                  .pageId = i,
                                  .dstAddr = localBase + i * regionBytes,
                                  .dstBuf = dstBuf});
    }
    return DOCA_SUCCESS;
}

static void destroyBufs(MemscanRscs &aRscs) {
    for (doca_buf *buf : aRscs.srcBufs) {
        CHECK_LOG(doca_buf_dec_refcount(buf, nullptr), "dec src buf refcount");
    }
    for (doca_buf *buf : aRscs.dstBufs) {
        CHECK_LOG(doca_buf_dec_refcount(buf, nullptr), "dec dst buf refcount");
    }
}

doca_error_t init(const MemscanCfg &aCfg, MemscanRscs &aRscs) {
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");
    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    /* Receive mmap descriptor from host and import it */
    CHECK_RETURN(dmaImportFromClient(aCfg.port, aRscs.dev, aRscs.hostMmap),
                 "import host mmap");

    /* Local DPU buffer: one receive region per page */
    size_t regionBytes = static_cast<size_t>(aCfg.regionSizeKb) * 1024;
    size_t localMemSize = aCfg.nPages * regionBytes;
    CHECK_RETURN(
        initMemory(aCfg.nPages * 2 + 16 + 65536, aRscs.dev, localMemSize,
                   aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv),
        "init local memory");

    CHECK_RETURN(initDma(aRscs.dev, aRscs.pe, memcpySuccCb, memcpyErrCb,
                         aRscs.dma, aRscs.dmaCtx),
                 "init dma");

    CHECK_RETURN(initBufs(aCfg, aRscs), "init bufs");

    return DOCA_SUCCESS;
}

void runTasks(const MemscanCfg &aCfg, MemscanRscs &aRscs) {
    using Clock = std::chrono::high_resolution_clock;
    auto scanPeriod = std::chrono::milliseconds(aCfg.tScanMs);
    size_t regionBytes = static_cast<size_t>(aCfg.regionSizeKb) * 1024;

    u32 nbBursts = 0;
    while (aCfg.nbBursts == 0 || nbBursts < aCfg.nbBursts) {
        auto burstStart = Clock::now();
        aRscs.nbCompletedInBurst = 0;

        /* Submit N_pages DMA read tasks (host memory → DPU local memory) */
        for (u32 i = 0; i < aCfg.nPages; i++) {
            doca_dma_task_memcpy *task;
            CHECK_LOG(doca_dma_task_memcpy_alloc_init(
                          aRscs.dma, aRscs.srcBufs[i], aRscs.dstBufs[i],
                          {.ptr = &aRscs.pageCtxs[i]}, &task),
                      "alloc dma memcpy task");
            CHECK_LOG(doca_task_submit(doca_dma_task_memcpy_as_task(task)),
                      "submit dma memcpy task");
        }

        /* Wait for all tasks in this burst to complete */
        while (aRscs.nbCompletedInBurst < aCfg.nPages) {
            doca_pe_progress(aRscs.pe);
        }

        /* Record JCT: time from first submit to last completion (μs) */
        double jctUs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           Clock::now() - burstStart)
                           .count() /
                       1000.0;
        aRscs.burstJcts.push_back(jctUs);
        nbBursts++;

        if (gForceQuit) break;

        /* Sleep for the remainder of the scan period */
        auto elapsed = Clock::now() - burstStart;
        if (elapsed < scanPeriod) {
            auto remaining = scanPeriod - elapsed;
            long ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)
                    .count();
            struct timespec ts = {ns / 1000000000L, ns % 1000000000L};
            nanosleep(&ts, nullptr);
        }
    }
    DOCA_LOG_INFO(
        "Completed %u bursts (%.1f MB each)", nbBursts,
        static_cast<double>(aCfg.nPages * regionBytes) / (1024.0 * 1024.0));
}

void destroy(MemscanRscs &aRscs) {
    destroyDma(aRscs.dma, aRscs.dmaCtx);
    destroyBufs(aRscs);
    CHECK_LOG(doca_mmap_destroy(aRscs.hostMmap), "destroy host mmap");
    destroyMemory(aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close device");
}
