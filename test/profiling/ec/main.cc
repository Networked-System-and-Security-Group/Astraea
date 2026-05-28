#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
// #include <format>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include <doca_erasure_coding.h>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "common.h"
#include "memory.h"

DOCA_LOG_REGISTER(PROFILING : EC);

using TimePoint = std::chrono::high_resolution_clock::time_point;
auto now = std::chrono::high_resolution_clock::now;

constexpr u32 kMaxNbTasks = 64;
constexpr std::array<u32, 8> nbDataBlksParams{1, 2, 4, 8, 16, 32, 64, 128};
constexpr std::array<u32, 6> nbRdncBlksParams{1, 2, 4, 8, 16, 32};
constexpr std::array<size_t, 15> blkSizeParams{
    64,    128,   256,   512,    1024,   2048,   4096,   8192,
    16384, 32768, 65536, 131072, 262144, 524288, 1048576};

u32 gNbFinishedTasks = 0;

std::array<TimePoint, kMaxNbTasks> gBeginTimes, gEndTimes;
using Result = std::tuple<u32, u32, size_t, double>;

static std::string printResults(std::vector<Result> &results) {
    std::string str;
    str += "[";
    for (const auto &result : results) {
        const auto &[nbDataBlks, nbRdncBlks, blkSize, timeCostInUs] = result;
        str = str + "[" + std::to_string(nbDataBlks) + ", " +
              std::to_string(nbRdncBlks) + ", " + std::to_string(blkSize) +
              ", " + std::to_string(timeCostInUs) + "], ";
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

static void createSuccCb(doca_ec_task_create *task, doca_data task_user_data,
                         doca_data ctx_user_data) {
    gEndTimes[gNbFinishedTasks] = now();
    gNbFinishedTasks++;
    if (gNbFinishedTasks < kMaxNbTasks) {
        auto userData =
            static_cast<std::array<doca_ec_task_create *, kMaxNbTasks> *>(
                task_user_data.ptr);
        gBeginTimes[gNbFinishedTasks] = now();
        CHECK_LOG(doca_task_submit(doca_ec_task_create_as_task(
                      (*userData)[gNbFinishedTasks])),
                  "submit ec create task");
    }
}

static void createErrCb(doca_ec_task_create *task, doca_data task_user_data,
                        doca_data ctx_user_data) {
    gNbFinishedTasks++;
    DOCA_LOG_ERR("Failed to do ec create task");
}

static void createSuccCbPipeline(doca_ec_task_create *task,
                                 doca_data task_user_data,
                                 doca_data ctx_user_data) {
    gEndTimes[gNbFinishedTasks] = now();
    if (gNbFinishedTasks != 0) {
        gBeginTimes[gNbFinishedTasks] = gEndTimes[gNbFinishedTasks - 1];
    }
    gNbFinishedTasks++;
}

static void createErrCbPipeline(doca_ec_task_create *task,
                                doca_data task_user_data,
                                doca_data ctx_user_data) {
    gNbFinishedTasks++;
    DOCA_LOG_ERR("Failed to do ec create task");
}

static void recoverSuccCb(doca_ec_task_recover *task, doca_data task_user_data,
                          doca_data ctx_user_data) {
    gEndTimes[gNbFinishedTasks] = now();
    gNbFinishedTasks++;
    if (gNbFinishedTasks < kMaxNbTasks) {
        auto userData =
            static_cast<std::array<doca_ec_task_recover *, kMaxNbTasks> *>(
                task_user_data.ptr);
        gBeginTimes[gNbFinishedTasks] = now();
        CHECK_LOG(doca_task_submit(doca_ec_task_recover_as_task(
                      (*userData)[gNbFinishedTasks])),
                  "submit ec recover task");
    }
}

static void recoverErrCb(doca_ec_task_recover *task, doca_data task_user_data,
                         doca_data ctx_user_data) {

    gNbFinishedTasks++;
    DOCA_LOG_ERR("Failed to do ec recover task");
}
static void recoverSuccCbPipeline(doca_ec_task_recover *task,
                                  doca_data task_user_data,
                                  doca_data ctx_user_data) {
    gEndTimes[gNbFinishedTasks] = now();
    if (gNbFinishedTasks != 0) {
        gBeginTimes[gNbFinishedTasks] = gEndTimes[gNbFinishedTasks - 1];
    }
    gNbFinishedTasks++;
}

static void recoverErrCbPipeline(doca_ec_task_recover *task,
                                 doca_data task_user_data,
                                 doca_data ctx_user_data) {

    gNbFinishedTasks++;
    DOCA_LOG_ERR("Failed to do ec recover task");
}

static void profileCreate(doca_pe *aPe, doca_ec *aEc, doca_ctx *aCtx,
                          doca_buf *aSrcBuf,
                          std::array<doca_buf *, kMaxNbTasks> &aDstBufs) {
    CHECK_LOG(doca_ec_task_create_set_conf(aEc, createSuccCb, createErrCb,
                                           kMaxNbTasks),
              "set create task conf");
    CHECK_LOG(doca_ctx_start(aCtx), "start ctx");

    std::vector<Result> results;
    for (const auto &nbDataBlks : nbDataBlksParams) {
        for (const auto &nbRdncBlks : nbRdncBlksParams) {
            for (const auto &blkSize : blkSizeParams) {
                gNbFinishedTasks = 0;
                doca_ec_matrix *mat;
                CHECK_LOG(doca_ec_matrix_create(aEc, DOCA_EC_MATRIX_TYPE_CAUCHY,
                                                nbDataBlks, nbRdncBlks, &mat),
                          "create ec matrix");
                CHECK_LOG(doca_buf_set_data_len(aSrcBuf, nbDataBlks * blkSize),
                          "set src buf size");

                std::array<doca_ec_task_create *, kMaxNbTasks> tasks;
                for (u32 i = 0; i < kMaxNbTasks; i++) {
                    CHECK_LOG(doca_ec_task_create_allocate_init(
                                  aEc, mat, aSrcBuf, aDstBufs[i],
                                  {.ptr = &tasks}, &tasks[i]),
                              "alloc ec create task");
                }

                gBeginTimes[0] = now();
                CHECK_LOG(
                    doca_task_submit(doca_ec_task_create_as_task(tasks[0])),
                    "submit the first ec create task");

                while (gNbFinishedTasks < kMaxNbTasks) {
                    doca_pe_progress(aPe);
                }

                for (auto &task : tasks) {
                    doca_task_free(doca_ec_task_create_as_task(task));
                }

                CHECK_LOG(doca_ec_matrix_destroy(mat), "destroy matrix");
                for (auto &dstBuf : aDstBufs) {
                    CHECK_LOG(doca_buf_set_data_len(dstBuf, 0),
                              "set dst buf len to 0");
                }
                results.push_back(std::make_tuple(
                    nbDataBlks, nbRdncBlks, blkSize, calAvgTimeCostInUs()));
            }
        }
    }

    std::ofstream logFile{"ec_prof_wo_pipeline.txt"};
    if (logFile.is_open()) {
        logFile << printResults(results) << std::endl;
        logFile.close();
    }

    CHECK_LOG(doca_ctx_stop(aCtx), "stop ctx");
}

static void
profileCreatePipeline(doca_pe *aPe, doca_ec *aEc, doca_ctx *aCtx,
                      doca_buf *aSrcBuf,
                      std::array<doca_buf *, kMaxNbTasks> &aDstBufs) {
    CHECK_LOG(doca_ec_task_create_set_conf(aEc, createSuccCbPipeline,
                                           createErrCbPipeline, kMaxNbTasks),
              "set create task conf");
    CHECK_LOG(doca_ctx_start(aCtx), "start ctx");

    std::vector<Result> results;
    for (const auto &nbDataBlks : nbDataBlksParams) {
        for (const auto &nbRdncBlks : nbRdncBlksParams) {
            for (const auto &blkSize : blkSizeParams) {
                gNbFinishedTasks = 0;
                doca_ec_matrix *mat;
                CHECK_LOG(doca_ec_matrix_create(aEc, DOCA_EC_MATRIX_TYPE_CAUCHY,
                                                nbDataBlks, nbRdncBlks, &mat),
                          "create ec matrix");
                CHECK_LOG(doca_buf_set_data_len(aSrcBuf, nbDataBlks * blkSize),
                          "set src buf size");

                std::array<doca_ec_task_create *, kMaxNbTasks> tasks;
                for (u32 i = 0; i < kMaxNbTasks; i++) {
                    CHECK_LOG(doca_ec_task_create_allocate_init(
                                  aEc, mat, aSrcBuf, aDstBufs[i],
                                  {.ptr = &tasks}, &tasks[i]),
                              "alloc ec create task");
                }

                for (u32 i = 0; i < kMaxNbTasks; i++) {
                    gBeginTimes[i] = now();
                    CHECK_LOG(
                        doca_task_submit(doca_ec_task_create_as_task(tasks[i])),
                        "submit the first ec create task");
                }

                while (gNbFinishedTasks < kMaxNbTasks) {
                    doca_pe_progress(aPe);
                }

                for (auto &task : tasks) {
                    doca_task_free(doca_ec_task_create_as_task(task));
                }

                CHECK_LOG(doca_ec_matrix_destroy(mat), "destroy matrix");
                for (auto &dstBuf : aDstBufs) {
                    CHECK_LOG(doca_buf_set_data_len(dstBuf, 0),
                              "set dst buf len to 0");
                }
                results.push_back(std::make_tuple(
                    nbDataBlks, nbRdncBlks, blkSize, calAvgTimeCostInUs()));
            }
        }
    }

    std::ofstream logFile{"ec_prof_w_pipeline.txt"};
    if (logFile.is_open()) {
        logFile << printResults(results) << std::endl;
        logFile.close();
    }

    CHECK_LOG(doca_ctx_stop(aCtx), "stop ctx");
}

static void profileRecover(doca_pe *aPe, doca_ec *aEc, doca_ctx *aCtx,
                           doca_buf *aSrcBuf,
                           std::array<doca_buf *, kMaxNbTasks> &aDstBufs) {
    CHECK_LOG(doca_ec_task_recover_set_conf(aEc, recoverSuccCb, recoverErrCb,
                                            kMaxNbTasks),
              "set recover task conf");
    CHECK_LOG(doca_ctx_start(aCtx), "start ctx");

    u32 missingIndices[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                            11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                            22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

    std::vector<Result> results;
    for (const auto &nbDataBlks : nbDataBlksParams) {
        for (const auto &nbRdncBlks : nbRdncBlksParams) {
            for (const auto &blkSize : blkSizeParams) {
                gNbFinishedTasks = 0;
                doca_ec_matrix *mat, *decMat;
                CHECK_LOG(doca_ec_matrix_create(aEc, DOCA_EC_MATRIX_TYPE_CAUCHY,
                                                nbDataBlks, nbRdncBlks, &mat),
                          "create ec matrix");
                u32 nbMissingIndices = std::min(nbDataBlks, nbRdncBlks);
                CHECK_LOG(
                    doca_ec_matrix_create_recover(aEc, mat, missingIndices,
                                                  nbMissingIndices, &decMat),
                    "create ec recover matrix");
                CHECK_LOG(doca_buf_set_data_len(aSrcBuf, nbDataBlks * blkSize),
                          "set src buf size");

                std::array<doca_ec_task_recover *, kMaxNbTasks> tasks;
                for (u32 i = 0; i < kMaxNbTasks; i++) {
                    CHECK_LOG(doca_ec_task_recover_allocate_init(
                                  aEc, decMat, aSrcBuf, aDstBufs[i],
                                  {.ptr = &tasks}, &tasks[i]),
                              "alloc ec recover task");
                }

                gBeginTimes[0] = now();
                CHECK_LOG(
                    doca_task_submit(doca_ec_task_recover_as_task(tasks[0])),
                    "submit the first ec recover task");

                while (gNbFinishedTasks < kMaxNbTasks) {
                    doca_pe_progress(aPe);
                }

                for (auto &task : tasks) {
                    doca_task_free(doca_ec_task_recover_as_task(task));
                }

                CHECK_LOG(doca_ec_matrix_destroy(mat), "destroy matrix");
                for (auto &dstBuf : aDstBufs) {
                    CHECK_LOG(doca_buf_set_data_len(dstBuf, 0),
                              "set dst buf len to 0");
                }
                results.push_back(std::make_tuple(
                    nbDataBlks, nbRdncBlks, blkSize, calAvgTimeCostInUs()));
            }
        }
    }

    std::ofstream logFile{"ec_recover_prof_wo_pipeline.txt"};
    if (logFile.is_open()) {
        logFile << printResults(results) << std::endl;
        logFile.close();
    }

    CHECK_LOG(doca_ctx_stop(aCtx), "stop ctx");
}

static void
profileRecoverPipeline(doca_pe *aPe, doca_ec *aEc, doca_ctx *aCtx,
                       doca_buf *aSrcBuf,
                       std::array<doca_buf *, kMaxNbTasks> &aDstBufs) {
    CHECK_LOG(doca_ec_task_recover_set_conf(aEc, recoverSuccCbPipeline,
                                            recoverErrCbPipeline, kMaxNbTasks),
              "set create task conf");
    CHECK_LOG(doca_ctx_start(aCtx), "start ctx");

    u32 missingIndices[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                            11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                            22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    std::vector<Result> results;
    for (const auto &nbDataBlks : nbDataBlksParams) {
        for (const auto &nbRdncBlks : nbRdncBlksParams) {
            for (const auto &blkSize : blkSizeParams) {
                gNbFinishedTasks = 0;
                doca_ec_matrix *mat, *decMat;
                u32 nbMissingIndices = std::min(nbDataBlks, nbRdncBlks);
                CHECK_LOG(doca_ec_matrix_create(aEc, DOCA_EC_MATRIX_TYPE_CAUCHY,
                                                nbDataBlks, nbRdncBlks, &mat),
                          "create ec matrix");
                CHECK_LOG(
                    doca_ec_matrix_create_recover(aEc, mat, missingIndices,
                                                  nbMissingIndices, &decMat),
                    "create ec recover matrix");
                CHECK_LOG(doca_buf_set_data_len(aSrcBuf, nbDataBlks * blkSize),
                          "set src buf size");

                std::array<doca_ec_task_recover *, kMaxNbTasks> tasks;
                for (u32 i = 0; i < kMaxNbTasks; i++) {
                    CHECK_LOG(doca_ec_task_recover_allocate_init(
                                  aEc, mat, aSrcBuf, aDstBufs[i],
                                  {.ptr = &tasks}, &tasks[i]),
                              "alloc ec recover task");
                }

                for (u32 i = 0; i < kMaxNbTasks; i++) {
                    gBeginTimes[i] = now();
                    CHECK_LOG(doca_task_submit(
                                  doca_ec_task_recover_as_task(tasks[i])),
                              "submit the first ec recover task");
                }

                while (gNbFinishedTasks < kMaxNbTasks) {
                    doca_pe_progress(aPe);
                }

                for (auto &task : tasks) {
                    doca_task_free(doca_ec_task_recover_as_task(task));
                }

                CHECK_LOG(doca_ec_matrix_destroy(mat), "destroy matrix");
                for (auto &dstBuf : aDstBufs) {
                    CHECK_LOG(doca_buf_set_data_len(dstBuf, 0),
                              "set dst buf len to 0");
                }
                results.push_back(std::make_tuple(
                    nbDataBlks, nbRdncBlks, blkSize, calAvgTimeCostInUs()));
            }
        }
    }

    std::ofstream logFile{"ec_recover_prof_w_pipeline.txt"};
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

    doca_ec *ec;
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

    CHECK_LOG(doca_ec_create(dev, &ec), "create ec");
    ctx = doca_ec_as_ctx(ec);
    CHECK_LOG(doca_pe_connect_ctx(pe, ctx), "connect pe to ctx");

    /********************************
     * Start Profiling
     ********************************/
    profileRecoverPipeline(pe, ec, ctx, srcBuf, dstBufs);
    ////////////////////////////////////
    CHECK_LOG(doca_ec_destroy(ec), "destroy ec");

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