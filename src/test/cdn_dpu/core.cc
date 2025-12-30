#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>
#include <doca_types.h>
#include <netinet/in.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "cdn_dpu.h"
#include "common.h"
#include "ec.h"
#include "memory.h"
#include "rdma.h"

DOCA_LOG_REGISTER(CDN:DPU : CORE);

extern bool gForceQuit;

void recvSuccCb(doca_rdma_task_receive *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    doca_be32_t be_imm = doca_rdma_task_receive_get_result_immediate_data(task);
    u32 imm = ntohl(be_imm);

    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;

    size_t chunkSize = userData->cfg.blkSize * 128;

    u32 nb_chunks = imm / chunkSize;
    if (!gForceQuit) {
        for (u32 i = 0; i < nb_chunks; ++i) {
            userData->chunk_id = i;
            userData->nb_chunks = nb_chunks;
            doca_task_submit(
                doca_ec_task_recover_as_task(userData->rscs.ecTasks[taskId]));
        }
    } else {
        doca_task_free(doca_rdma_task_receive_as_task(rscs.recvTasks[taskId]));
        doca_task_free(doca_ec_task_recover_as_task(rscs.ecTasks[taskId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        rscs.nbFreedTasks++;
    }
}

void recvErrCb(doca_rdma_task_receive *task, doca_data task_user_data,
               doca_data ctx_user_data) {}

void ecSuccCb(doca_ec_task_recover *task, doca_data task_user_data,
              doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    if (!gForceQuit) {
        CHECK_LOG(doca_task_submit(
                      doca_rdma_task_write_as_task(rscs.writeTasks[taskId])),
                  "submit write task in cb");
    } else {
        doca_task_free(doca_rdma_task_receive_as_task(rscs.recvTasks[taskId]));
        doca_task_free(doca_ec_task_recover_as_task(rscs.ecTasks[taskId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        rscs.nbFreedTasks++;
    }
}
void ecErrCb(doca_ec_task_recover *task, doca_data task_user_data,
             doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    gForceQuit = true;
    doca_task_free(doca_rdma_task_receive_as_task(rscs.recvTasks[taskId]));
    doca_task_free(doca_ec_task_recover_as_task(rscs.ecTasks[taskId]));
    doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("EC task failed");
}

void WriteSuccCb(doca_rdma_task_write *task, doca_data task_user_data,
                 doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    rscs.endTimes.push_back(std::chrono::high_resolution_clock::now());
    if (!gForceQuit) {
        if (userData->chunk_id == userData->nb_chunks) {
            userData->chunk_id = 0;
            userData->nb_chunks = 1;
            rscs.nbFinishedTasks++;
        }
        CHECK_LOG(doca_buf_set_data_len(rscs.sendBufs[taskId], 0),
                  "set send buf len to 0");
        CHECK_LOG(doca_buf_set_data_len(rscs.clientBufs[taskId], 0),
                  "set client buf len to 0");
        rscs.beginTimes.push_back(std::chrono::high_resolution_clock::now());
        CHECK_LOG(doca_task_submit(
                      doca_ec_task_recover_as_task(rscs.ecTasks[taskId])),
                  "submit ec task in cb");
    } else {
        doca_task_free(doca_rdma_task_receive_as_task(rscs.recvTasks[taskId]));
        doca_task_free(doca_ec_task_recover_as_task(rscs.ecTasks[taskId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        rscs.nbFreedTasks++;
    }
}

void WriteErrCb(doca_rdma_task_write *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    CdnUserData *userData = static_cast<CdnUserData *>(task_user_data.ptr);
    CdnRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    gForceQuit = true;
    doca_task_free(doca_rdma_task_receive_as_task(rscs.recvTasks[taskId]));
    doca_task_free(doca_ec_task_recover_as_task(rscs.ecTasks[taskId]));
    doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("Write task failed");
}

static doca_error_t initBufs(const CdnCfg &aCfg, CdnRscs &aRscs) {
    // char *hostMemAddr;
    // size_t hostMemAddrSize;
    // CHECK_RETURN(doca_mmap_get_memrange(aRscs.hostMmap,
    //                                     reinterpret_cast<void
    //                                     **>(&hostMemAddr), &hostMemAddrSize),
    //              "get host memory addr");

    char *clientMemAddr;
    size_t clientMemAddrSize;
    CHECK_RETURN(
        doca_mmap_get_memrange(aRscs.clientMmap,
                               reinterpret_cast<void **>(&clientMemAddr),
                               &clientMemAddrSize),
        "get client memory addr");

    size_t dataSize = aCfg.blkSize * kNbDataBlks;

    char *localMemAddrInChar = static_cast<char *>(aRscs.localMemAddr);
    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        // doca_buf *hostBuf, *recvBuf, *sendBuf, *clientBuf;
        // CHECK_RETURN(doca_buf_inventory_buf_get_by_data(
        //                  aRscs.bufInv, aRscs.hostMmap,
        //                  hostMemAddr + i * dataSize, dataSize, &hostBuf),
        //              "get host buf by data");
        // aRscs.hostBufs.push_back(hostBuf);

        // CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
        //                  aRscs.bufInv, aRscs.localMmap,
        //                  localMemAddrInChar + i * 2 * dataSize, dataSize,
        //                  &recvBuf),
        //              "get recv buf by addr");
        doca_buf *recvBuf, *sendBuf, *clientBuf;
        CHECK_RETURN(
            doca_buf_inventory_buf_get_by_data(
                aRscs.bufInv, aRscs.localMmap,
                localMemAddrInChar + i * 2 * dataSize, dataSize, &recvBuf),
            "get recv buf by addr");
        aRscs.recvBufs.push_back(recvBuf);

        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.localMmap,
                         localMemAddrInChar + i * 2 * dataSize + dataSize,
                         dataSize, &sendBuf),
                     "get send buf by data");
        aRscs.sendBufs.push_back(sendBuf);

        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.clientMmap,
                         clientMemAddr + i * dataSize, dataSize, &clientBuf),
                     "get client buf by addr");
        aRscs.clientBufs.push_back(clientBuf);
    }
    return DOCA_SUCCESS;
}

static void destroyBufs(CdnRscs &aRscs) {
    // for (doca_buf *&hostBuf : aRscs.hostBufs) {
    //     CHECK_LOG(doca_buf_dec_refcount(hostBuf, nullptr),
    //               "dec host buf ref cnt");
    // }

    for (doca_buf *&clientBuf : aRscs.clientBufs) {
        CHECK_LOG(doca_buf_dec_refcount(clientBuf, nullptr),
                  "dec client buf ref cnt");
    }

    for (doca_buf *&recvBuf : aRscs.recvBufs) {
        CHECK_LOG(doca_buf_dec_refcount(recvBuf, nullptr),
                  "dec recv buf ref cnt");
    }

    for (doca_buf *&sendBuf : aRscs.sendBufs) {
        CHECK_LOG(doca_buf_dec_refcount(sendBuf, nullptr),
                  "dec send buf ref cnt");
    }
}

static doca_error_t initTasks(const CdnCfg &aCfg, CdnRscs &aRscs) {
    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        CdnUserData userData = {.rscs = aRscs,
                                .cfg = aCfg,
                                .taskId = i,
                                .chunk_id = 0,
                                .nb_chunks = 1};
        aRscs.userDatas.push_back(userData);
    }

    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        doca_rdma_task_receive *recvTask;
        CHECK_RETURN(
            doca_rdma_task_receive_allocate_init(
                aRscs.rdma, nullptr, {.ptr = &aRscs.userDatas[i]}, &recvTask),
            "alloc and init recv task");
        aRscs.recvTasks.push_back(recvTask);

        doca_ec_task_recover *ecTask;
        CHECK_RETURN(
            doca_ec_task_recover_allocate_init(
                aRscs.ec, aRscs.encMat, aRscs.recvBufs[i], aRscs.sendBufs[i],
                {.ptr = &aRscs.userDatas[i]}, &ecTask),
            "alloc and init ec task");
        aRscs.ecTasks.push_back(ecTask);

        doca_rdma_task_write *writeTask;
        CHECK_RETURN(
            doca_rdma_task_write_allocate_init(
                aRscs.rdma, aRscs.clientConn, aRscs.sendBufs[i],
                aRscs.clientBufs[i], {.ptr = &aRscs.userDatas[i]}, &writeTask),
            "alloc and init write task");
        aRscs.writeTasks.push_back(writeTask);
    }
    return DOCA_SUCCESS;
}

doca_error_t init(const CdnCfg &aCfg, CdnRscs &aRscs) {
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");

    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    size_t mmapSize = aCfg.blkSize * kNbDataBlks * 2 * aCfg.nbTasks;
    CHECK_RETURN(initMemory(8192, aRscs.dev, mmapSize, aRscs.localMemAddr,
                            aRscs.localMmap, aRscs.bufInv),
                 "init memory");

    uint32_t missingIndices[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    CHECK_RETURN(initEc(aRscs.dev, aRscs.pe, kNbDataBlks, kNbRdncBlks,
                        missingIndices, 32, nullptr, nullptr, ecSuccCb, ecErrCb,
                        aRscs.ec, aRscs.encMat, aRscs.decMat, aRscs.ecCtx),
                 "init ec");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          WriteSuccCb, WriteErrCb, aRscs.rdma, aRscs.rdmaCtx),
                 "init rdma");

    CHECK_RETURN(rdmaConnect(aRscs.dev, aCfg.clientIpAddr, aRscs.port,
                             aRscs.rdma, aRscs.clientConn, aRscs.clientMmap),
                 "connect to client");

    DOCA_LOG_INFO("Thread %u connection established", aRscs.threadId);

    CHECK_RETURN(initBufs(aCfg, aRscs), "init bufs");

    CHECK_RETURN(initTasks(aCfg, aRscs), "init tasks");

    return DOCA_SUCCESS;
}

void runTasks(const CdnCfg &aCfg, CdnRscs &aRscs) {
    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        aRscs.beginTimes.push_back(std::chrono::high_resolution_clock::now());
        CHECK_LOG(
            doca_task_submit(doca_ec_task_recover_as_task(aRscs.ecTasks[i])),
            "submit ec task");
    }

    while (!gForceQuit) {
        doca_pe_progress(aRscs.pe);
    }

    while (aRscs.nbFreedTasks < aCfg.nbTasks) {
        doca_pe_progress(aRscs.pe);
    }

    for (u32 i = 0; i < aRscs.nbFinishedTasks; i++) {
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