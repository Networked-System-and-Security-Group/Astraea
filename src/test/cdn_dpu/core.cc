#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>
#include <doca_types.h>
#include <netinet/in.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>

#include "cdn_dpu.h"
#include "common.h"
#include "ec.h"
#include "memory.h"
#include "rdma.h"

DOCA_LOG_REGISTER(CDN:DPU : CORE);

extern bool gForceQuit;

static void freeTasks(CdnRscs &aRscs, u32 aStageId) {
    TaskPack &pack = aRscs.packs[aStageId];
    doca_task_free(doca_rdma_task_receive_as_task(pack.recvTask));
    doca_task_free(doca_rdma_task_write_imm_as_task(pack.immTask));
    for (doca_ec_task_recover *ecTask : pack.ecTasks) {
        doca_task_free(doca_ec_task_recover_as_task(ecTask));
    }
}

static void recvSuccCb(doca_rdma_task_receive *task, doca_data task_user_data,
                       doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;

    rscs.beginTimes.push_back(std::chrono::high_resolution_clock::now());

    doca_be32_t be_imm = doca_rdma_task_receive_get_result_immediate_data(task);
    u32 imm = ntohl(be_imm);

    size_t rounded = (imm + kMinChunkSize / 2) / kMinChunkSize * kMinChunkSize;
    u32 nb_chunks = std::max<size_t>(rounded / kMaxChunkSize, 1);
    if (!gForceQuit) {
        doca_buf_set_data_len(rscs.packs[stageId].sendBuf, imm);
        for (u32 i = 0; i < nb_chunks; ++i) {
            userData->nbChunks = nb_chunks;
            doca_buf_set_data_len(rscs.packs[stageId].dataBufs[i],
                                  nb_chunks == 1 ? rounded : kMaxChunkSize);
            doca_buf_set_data_len(rscs.packs[stageId].rdncBufs[i], 0);
            CHECK_LOG(doca_task_submit(doca_ec_task_recover_as_task(
                          rscs.packs[stageId].ecTasks[i])),
                      "submit ec taks in recv cb");
        }
    } else {
        freeTasks(rscs, stageId);
        rscs.nbFreedTasks++;
    }
}

static void recvErrCb(doca_rdma_task_receive *task, doca_data task_user_data,
                      doca_data ctx_user_data) {}

static void ecSuccCb(doca_ec_task_recover *task, doca_data task_user_data,
                     doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;
    if (!gForceQuit) {
        if (userData->chunkId == userData->nbChunks - 1) {
            doca_buf_set_data_len(rscs.packs[stageId].clientBuf, 0);
            CHECK_LOG(doca_task_submit(doca_rdma_task_write_imm_as_task(
                          rscs.packs[stageId].immTask)),
                      "submit write task in cb");
        }
    } else {
        if (userData->chunkId == userData->nbChunks - 1) {
            freeTasks(rscs, stageId);
            rscs.nbFreedTasks++;
        }
    }
}
static void ecErrCb(doca_ec_task_recover *task, doca_data task_user_data,
                    doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;
    gForceQuit = true;
    freeTasks(rscs, stageId);
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("EC task failed");
}

// static void WriteSuccCb(doca_rdma_task_write *task, doca_data task_user_data,
//                         doca_data ctx_user_data) {
//     CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
//     CdnRscs &rscs = userData->rscs;
//     u32 stageId = userData->stageId;
//     if (!gForceQuit) {
//         if (userData->chunkId == userData->nbChunks - 1) {
//             userData->chunkId = 0;
//             userData->nbChunks = 1;
//             rscs.sizes.push_back(userData->requestSize);
//             CHECK_LOG(doca_task_submit(doca_rdma_task_receive_as_task(
//                           rscs.packs[stageId].recvTasks)),
//                       "submit receive task in cb");

//             rscs.endTimes.push_back(std::chrono::high_resolution_clock::now());
//         }
//     } else {
//         if (userData->chunkId == userData->nbChunks - 1) {
//             freeTasks(rscs, stageId);
//             rscs.nbFreedTasks++;
//         }
//     }
// }

// static void WriteErrCb(doca_rdma_task_write *task, doca_data task_user_data,
//                        doca_data ctx_user_data) {
//     CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
//     CdnRscs &rscs = userData->rscs;
//     u32 stageId = userData->stageId;
//     gForceQuit = true;
//     freeTasks(rscs, stageId);
//     rscs.nbFreedTasks++;
//     DOCA_LOG_ERR("Write task failed");
// }

u32 fin = 0;
static void immSuccCb(doca_rdma_task_write_imm *task, doca_data task_user_data,
                      doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;
    if (!gForceQuit) {
        rscs.sizes.push_back(userData->requestSize);
        ++fin;
        DOCA_LOG_INFO("Nb finished task is %u", fin);
        CHECK_LOG(doca_task_submit(doca_rdma_task_receive_as_task(
                      rscs.packs[stageId].recvTask)),
                  "submit receive task in cb");

        rscs.endTimes.push_back(std::chrono::high_resolution_clock::now());
    } else {
        freeTasks(rscs, stageId);
        rscs.nbFreedTasks++;
    }
}

static void immErrCb(doca_rdma_task_write_imm *task, doca_data task_user_data,
                     doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;
    gForceQuit = true;
    freeTasks(rscs, stageId);
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("Write task failed");
}

static doca_error_t initBufs(const CdnCfg &aCfg, CdnRscs &aRscs) {
    char *clientMemAddr;
    size_t clientMemAddrSize;
    CHECK_RETURN(
        doca_mmap_get_memrange(aRscs.clientMmap,
                               reinterpret_cast<void **>(&clientMemAddr),
                               &clientMemAddrSize),
        "get client memory addr");

    char *localMemAddrInChar = static_cast<char *>(aRscs.localMemAddr);
    constexpr size_t dataSize = kMaxChunkSize;

    for (u32 i = 0; i < aCfg.nbPipelineStages; ++i) {
        TaskPack &pack = aRscs.packs[i];
        // Size of send buf and client buf is up to 1GB
        CHECK_RETURN(doca_buf_inventory_buf_get_by_data(
                         aRscs.bufInv, aRscs.localMmap, localMemAddrInChar,
                         dataSize * kMaxNbChunks, &pack.sendBuf),
                     "get send buf by data");
        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.clientMmap, clientMemAddr,
                         dataSize * kMaxNbChunks, &pack.clientBuf),
                     "get client buf by addr");

        for (u32 j = 0; j < kMaxNbChunks; ++j) {
            doca_buf *dataBuf, *rdncBuf;
            CHECK_RETURN(
                doca_buf_inventory_buf_get_by_data(
                    aRscs.bufInv, aRscs.localMmap,
                    localMemAddrInChar + i * 2 * dataSize, dataSize, &dataBuf),
                "get data buf by data");
            CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                             aRscs.bufInv, aRscs.localMmap,
                             localMemAddrInChar + (i * 2 + 1) * dataSize,
                             dataSize, &rdncBuf),
                         "get rdnc buf by addr");
            pack.dataBufs.push_back(dataBuf);
            pack.rdncBufs.push_back(rdncBuf);
        }
    }

    return DOCA_SUCCESS;
}

static void destroyBufs(CdnRscs &aRscs) {
    for (auto &pack : aRscs.packs) {
        CHECK_LOG(doca_buf_dec_refcount(pack.clientBuf, nullptr),
                  "dec client buf ref count");
        CHECK_LOG(doca_buf_dec_refcount(pack.sendBuf, nullptr),
                  "dec send buf ref count");

        for (auto &rdncBuf : pack.rdncBufs) {
            CHECK_LOG(doca_buf_dec_refcount(rdncBuf, nullptr),
                      "dec rdnc buf ref count");
        }

        for (auto &dataBuf : pack.dataBufs) {
            CHECK_LOG(doca_buf_dec_refcount(dataBuf, nullptr),
                      "dec data buf ref count");
        }
    }
}

static doca_error_t initTasks(const CdnCfg &aCfg, CdnRscs &aRscs) {
    for (auto &pack : aRscs.packs) {
        for (u32 i = 0; i < kMaxNbChunks; ++i) {
            doca_ec_task_recover *ecTask;
            CHECK_RETURN(
                doca_ec_task_recover_allocate_init(
                    aRscs.ec, aRscs.decMat, pack.dataBufs[i], pack.rdncBufs[i],
                    {.ptr = &pack.userDatas[i]}, &ecTask),
                "alloc and init ec task");
            pack.ecTasks.push_back(ecTask);
        }

        CHECK_RETURN(doca_rdma_task_receive_allocate_init(
                         aRscs.rdma, nullptr, {.ptr = &pack.userDatas[0]},
                         &pack.recvTask),
                     "alloc and init receive task");
        CHECK_RETURN(
            doca_rdma_task_write_imm_allocate_init(
                aRscs.rdma, aRscs.clientConn, pack.sendBuf, pack.clientBuf, 0,
                {.ptr = &pack.userDatas[0]}, &pack.immTask),
            "alloc and init write imm task");
    }

    return DOCA_SUCCESS;
}

doca_error_t init(const CdnCfg &aCfg, CdnRscs &aRscs) {
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");

    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    size_t mmapSize = kMaxChunkSize * kMaxNbChunks;
    CHECK_RETURN(initMemory(8192, aRscs.dev, mmapSize, aRscs.localMemAddr,
                            aRscs.localMmap, aRscs.bufInv),
                 "init memory");

    u32 missingIndices[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                            11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                            22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    CHECK_RETURN(initEc(aRscs.dev, aRscs.pe, kNbDataBlks, kNbRdncBlks,
                        missingIndices, 32, nullptr, nullptr, ecSuccCb, ecErrCb,
                        aRscs.ec, aRscs.encMat, aRscs.decMat, aRscs.ecCtx),
                 "init ec");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          nullptr, nullptr, recvSuccCb, recvErrCb, immSuccCb,
                          immErrCb, aRscs.rdma, aRscs.rdmaCtx),
                 "init rdma");

    CHECK_RETURN(
        rdmaConnectToClient(aRscs.dev, aRscs.port, aRscs.rdma, aRscs.threadId,
                            aRscs.clientConn, aRscs.clientMmap),
        "connect to client");

    // Init user data
    for (u32 i = 0; i < aCfg.nbPipelineStages; ++i) {
        aRscs.packs.push_back({});
        for (u32 j = 0; j < kMaxNbChunks; ++j) {
            aRscs.packs[i].userDatas.push_back({.rscs = aRscs,
                                                .cfg = aCfg,
                                                .stageId = i,
                                                .chunkId = j,
                                                .nbChunks = 1});
        }
    }

    CHECK_RETURN(initBufs(aCfg, aRscs), "init bufs");

    CHECK_RETURN(initTasks(aCfg, aRscs), "init tasks");

    return DOCA_SUCCESS;
}

void runTasks(const CdnCfg &aCfg, CdnRscs &aRscs) {
    DOCA_LOG_INFO("Before submit");
    for (u32 i = 0; i < aCfg.nbPipelineStages; i++) {
        CHECK_LOG(doca_task_submit(
                      doca_rdma_task_receive_as_task(aRscs.packs[i].recvTask)),
                  "submit recv task");
    }
    DOCA_LOG_INFO("Submitted recv tasks");

    while (!gForceQuit) {
        doca_pe_progress(aRscs.pe);
    }

    while (aRscs.nbFreedTasks < aCfg.nbPipelineStages) {
        doca_pe_progress(aRscs.pe);
    }

    for (u32 i = 0; i < aRscs.sizes.size(); i++) {
        double timeCost = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              aRscs.endTimes[i] - aRscs.beginTimes[i])
                              .count() /
                          1000.0;
        aRscs.timeCosts.push_back(timeCost);
    }
}

void destroy(CdnRscs &aRscs) {
    destroyEc(aRscs.encMat, nullptr, aRscs.ec, aRscs.ecCtx);
    destroyRdma(aRscs.rdma, aRscs.rdmaCtx);
    destroyBufs(aRscs);
    CHECK_LOG(doca_mmap_destroy(aRscs.clientMmap), "destroy client mmap");
    destroyMemory(aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}