#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "common.h"
#include "memory.h"

DOCA_LOG_REGISTER(PROFILING : DMA);

using TimePoint = std::chrono::high_resolution_clock::time_point;
auto now = std::chrono::high_resolution_clock::now;

constexpr u32 kMaxNbTasks = 64;
constexpr std::array<size_t, 16> blkSizeParams{
    64,    128,   256,   512,    1024,   2048,   4096,    8192,
    16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152};

u32 gNbFinishedTasks = 0;

std::array<TimePoint, kMaxNbTasks> gBeginTimes, gEndTimes;
using Result = std::tuple<size_t, double>;

static std::string printResults(std::vector<Result> &results) {
    std::string str;
    str += "[";
    for (const auto &result : results) {
        const auto &[blkSize, timeCostInUs] = result;
        str = str + "[" + std::to_string(blkSize) + ", " +
              std::to_string(timeCostInUs) + "], ";
    }
    str += "]";
    return str;
}

static double calAvgTimeCostInUs() {
    double timeCostSumInUs = 0;
    for (u32 i = 0; i < kMaxNbTasks; i++) {
        timeCostSumInUs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                               gEndTimes[i] - gBeginTimes[i])
                               .count() /
                           1000.0;
    }
    return timeCostSumInUs / kMaxNbTasks;
}

static void memcpySuccCb(doca_dma_task_memcpy *task, doca_data task_user_data,
                         doca_data ctx_user_data) {
    gEndTimes[gNbFinishedTasks] = now();
    gNbFinishedTasks++;
    if (gNbFinishedTasks < kMaxNbTasks) {
        auto userData =
            static_cast<std::array<doca_dma_task_memcpy *, kMaxNbTasks> *>(
                task_user_data.ptr);
        gBeginTimes[gNbFinishedTasks] = now();
        CHECK_LOG(doca_task_submit(doca_dma_task_memcpy_as_task(
                      (*userData)[gNbFinishedTasks])),
                  "submit dma memcpy task");
    }
}

static void memcpyErrCb(doca_dma_task_memcpy *task, doca_data task_user_data,
                        doca_data ctx_user_data) {
    gNbFinishedTasks++;
    DOCA_LOG_ERR("Failed to do dma memcpy task");
}

static void memcpyPipelineSuccCb(doca_dma_task_memcpy *task,
                                 doca_data task_user_data,
                                 doca_data ctx_user_data) {
    gEndTimes[gNbFinishedTasks] = now();
    if (gNbFinishedTasks != 0) {
        gBeginTimes[gNbFinishedTasks] = gEndTimes[gNbFinishedTasks - 1];
    }
    gNbFinishedTasks++;
}

static void memcpyPipelineErrCb(doca_dma_task_memcpy *task,
                                doca_data task_user_data,
                                doca_data ctx_user_data) {
    gNbFinishedTasks++;
    DOCA_LOG_ERR("Failed to do dma memcpy task");
}

static void profileMemcpy(doca_pe *aPe, doca_dma *aDma, doca_ctx *aCtx,
                          doca_buf *aSrcBuf,
                          std::array<doca_buf *, kMaxNbTasks> &aDstBufs) {
    CHECK_LOG(doca_dma_task_memcpy_set_conf(aDma, memcpySuccCb, memcpyErrCb,
                                            kMaxNbTasks),
              "set memcpy task conf");
    CHECK_LOG(doca_ctx_start(aCtx), "start ctx");

    std::vector<Result> results;
    for (const auto &blkSize : blkSizeParams) {
        gNbFinishedTasks = 0;
        CHECK_LOG(doca_buf_set_data_len(aSrcBuf, blkSize), "set src buf size");

        std::array<doca_dma_task_memcpy *, kMaxNbTasks> tasks;
        for (u32 i = 0; i < kMaxNbTasks; i++) {
            CHECK_LOG(
                doca_dma_task_memcpy_alloc_init(aDma, aSrcBuf, aDstBufs[i],
                                                {.ptr = &tasks}, &tasks[i]),
                "alloc dma memcpy task");
        }

        gBeginTimes[0] = now();
        CHECK_LOG(doca_task_submit(doca_dma_task_memcpy_as_task(tasks[0])),
                  "submit the first dma create task");

        while (gNbFinishedTasks < kMaxNbTasks) {
            doca_pe_progress(aPe);
        }

        for (auto &task : tasks) {
            doca_task_free(doca_dma_task_memcpy_as_task(task));
        }

        for (auto &dstBuf : aDstBufs) {
            CHECK_LOG(doca_buf_set_data_len(dstBuf, 0), "set dst buf len to 0");
        }
        results.push_back(std::make_tuple(blkSize, calAvgTimeCostInUs()));
    }

    std::ofstream logFile{"logs/dma_prof_wo_pipeline.log"};
    if (logFile.is_open()) {
        logFile << printResults(results) << std::endl;
        logFile.close();
    }

    CHECK_LOG(doca_ctx_stop(aCtx), "stop ctx");
}

static void profileMemcpyPipeline(
    doca_pe *aPe, doca_dma *aDma, doca_ctx *aCtx, doca_buf *aSrcBuf,
    std::array<doca_buf *, kMaxNbTasks> &aDstBufs) {
    CHECK_LOG(doca_dma_task_memcpy_set_conf(aDma, memcpyPipelineSuccCb,
                                            memcpyPipelineErrCb, kMaxNbTasks),
              "set memcpy task conf");
    CHECK_LOG(doca_ctx_start(aCtx), "start ctx");

    std::vector<Result> results;
    for (const auto &blkSize : blkSizeParams) {
        gNbFinishedTasks = 0;

        CHECK_LOG(doca_buf_set_data_len(aSrcBuf, blkSize), "set src buf size");

        std::array<doca_dma_task_memcpy *, kMaxNbTasks> tasks;
        for (u32 i = 0; i < kMaxNbTasks; i++) {
            CHECK_LOG(
                doca_dma_task_memcpy_alloc_init(aDma, aSrcBuf, aDstBufs[i],
                                                {.ptr = &tasks}, &tasks[i]),
                "alloc dma memcpy task");
        }

        for (u32 i = 0; i < kMaxNbTasks; i++) {
            gBeginTimes[i] = now();
            CHECK_LOG(doca_task_submit(doca_dma_task_memcpy_as_task(tasks[i])),
                      "submit the first dma memcpy task");
        }

        while (gNbFinishedTasks < kMaxNbTasks) {
            doca_pe_progress(aPe);
        }

        for (auto &task : tasks) {
            doca_task_free(doca_dma_task_memcpy_as_task(task));
        }

        for (auto &dstBuf : aDstBufs) {
            CHECK_LOG(doca_buf_set_data_len(dstBuf, 0), "set dst buf len to 0");
        }
        results.push_back(std::make_tuple(blkSize, calAvgTimeCostInUs()));
    }

    std::ofstream logFile{"logs/dma_prof_w_pipeline.log"};
    if (logFile.is_open()) {
        logFile << printResults(results) << std::endl;
        logFile.close();
    }

    CHECK_LOG(doca_ctx_stop(aCtx), "stop ctx");
}

int main() {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    doca_dev *dev;
    doca_pe *pe;

    doca_dma *dma;
    doca_ctx *ctx;

    doca_buf_inventory *inv;
    doca_mmap *mmap;
    void *memAddr;
    doca_buf *srcBuf;
    std::array<doca_buf *, kMaxNbTasks> dstBufs;

    CHECK_RETURN(openDev("mlx5_0", dev), "open device");

    CHECK_RETURN(
        initMemory(8192, dev, 1024 * 1024 * 128 * 2, memAddr, mmap, inv),
        "init memory");
    CHECK_RETURN(doca_pe_create(&pe), "create pe");

    CHECK_LOG(doca_buf_inventory_buf_get_by_data(inv, mmap, memAddr,
                                                 1024 * 1024 * 128, &srcBuf),
              "create src buf");
    for (auto &dstBuf : dstBufs) {
        CHECK_LOG(doca_buf_inventory_buf_get_by_addr(
                      inv, mmap, static_cast<u8 *>(memAddr) + 1024 * 1024 * 128,
                      1024 * 1024 * 128, &dstBuf),
                  "create dst buf");
    }

    CHECK_LOG(doca_dma_create(dev, &dma), "create dma");
    ctx = doca_dma_as_ctx(dma);
    CHECK_LOG(doca_pe_connect_ctx(pe, ctx), "connect pe to ctx");

    /********************************
     * Start Profiling
     ********************************/
    profileMemcpy(pe, dma, ctx, srcBuf, dstBufs);
    ////////////////////////////////////
    CHECK_LOG(doca_dma_destroy(dma), "destroy dma");

    CHECK_LOG(doca_buf_dec_refcount(srcBuf, nullptr), "destroy src buf");
    for (u32 i = 0; i < kMaxNbTasks; i++) {
        CHECK_LOG(doca_buf_dec_refcount(dstBufs[i], nullptr),
                  "destroy dst buf");
    }

    CHECK_LOG(doca_pe_destroy(pe), "destroy pe");
    destroyMemory(memAddr, mmap, inv);
    CHECK_LOG(doca_dev_close(dev), "close device");

    return EXIT_SUCCESS;
}