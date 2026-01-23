#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>

#include "common.h"
#include "doca_erasure_coding.h"
#include "ec.h"
#include "memory.h"
#include "rdma.h"
#include "replica_dpu.h"

DOCA_LOG_REGISTER(REPLICA:DPU : CORE);

extern bool gForceQuit;
extern std::chrono::high_resolution_clock::time_point gBeginTime, gEndTime;

std::vector<Request> load_requests_text(const std::string &path) {
    std::ifstream ifs{path};
    if (!ifs) throw std::runtime_error("open for read failed: " + path);

    std::vector<Request> events;
    std::string line;

    // 跳过注释行（以 # 开头）
    auto read_next_data_line = [&]() -> bool {
        while (std::getline(ifs, line)) {
            if (!line.empty() && line[0] == '#') continue;
            if (line.empty()) continue;
            return true;
        }
        return false;
    };

    // 读条数（如果你不想写条数，也可以改为“读到 EOF 为止”）
    if (!read_next_data_line()) return events;
    size_t n = 0;
    {
        std::istringstream iss(line);
        if (!(iss >> n)) throw std::runtime_error("bad count line");
    }
    events.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        if (!read_next_data_line()) throw std::runtime_error("unexpected EOF");
        std::istringstream iss(line);
        uint64_t ts = 0, sz = 0;
        if (!(iss >> ts >> sz))
            throw std::runtime_error("bad data line: " + line);
        events.emplace_back(ts, sz);
    }
    return events;
}

void ecSuccCb(doca_ec_task_create *task, doca_data task_user_data,
              doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t stageId = userData->stageId;
    if (!gForceQuit) {
        CHECK_LOG(doca_buf_set_data_len(rscs.clientBufs[stageId], 0),
                  "set client buf len to 0");
        CHECK_LOG(
            doca_buf_set_data_len(rscs.sendBufs[stageId],
                                  rscs.requests[rscs.requestIds[stageId]].size),
            "set write task size");
        CHECK_LOG(doca_task_submit(
                      doca_rdma_task_write_as_task(rscs.writeTasks[stageId])),
                  "submit write task in cb");
    } else {
        // doca_task_free(doca_rdma_task_read_as_task(rscs.readTasks[stageId]));
        doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[stageId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[stageId]));
        rscs.nbFreedTasks++;
    }
}

void ecErrCb(doca_ec_task_create *task, doca_data task_user_data,
             doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t stageId = userData->stageId;
    gForceQuit = true;
    doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[stageId]));
    doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[stageId]));
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("EC task failed");
}

void WriteSuccCb(doca_rdma_task_write *task, doca_data task_user_data,
                 doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t stageId = userData->stageId;
    rscs.endTimes.push_back(std::chrono::high_resolution_clock::now());
    if (!gForceQuit) {
        const uint64_t t0 = rscs.requests[stageId].ts_ms;
        u32 &requestId = rscs.requestIds[stageId];
        rscs.nbFinishedTasks++;
        rscs.nbProcessedGBits += rscs.requests[requestId].size * 8 / 1e9;

        requestId += userData->cfg.nbPipelineStages;

        if (requestId < rscs.requests.size()) {
            uint64_t rel_ms = (rscs.requests[requestId].ts_ms >= t0)
                                  ? (rscs.requests[requestId].ts_ms - t0)
                                  : 0;
            auto target = gBeginTime + std::chrono::milliseconds(rel_ms);
            std::this_thread::sleep_until(target);
            CHECK_LOG(doca_buf_set_data_len(rscs.rdncBufs[stageId], 0),
                      "set rdnc buf len to 0");
            CHECK_LOG(doca_task_submit(
                          doca_ec_task_create_as_task(rscs.ecTasks[stageId])),
                      "submit ec task in cb");
        } else {
            doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[stageId]));
            doca_task_free(
                doca_rdma_task_write_as_task(rscs.writeTasks[stageId]));
            rscs.nbFreedTasks++;
        }
    } else {
        doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[stageId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[stageId]));
        rscs.nbFreedTasks++;
    }
}

void WriteErrCb(doca_rdma_task_write *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t stageId = userData->stageId;
    gForceQuit = true;
    doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[stageId]));
    doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[stageId]));
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("Write task failed");
}

static doca_error_t initBufs(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    char *clientMemAddr;
    size_t clientMemAddrSize;
    CHECK_RETURN(
        doca_mmap_get_memrange(aRscs.clientMmap,
                               reinterpret_cast<void **>(&clientMemAddr),
                               &clientMemAddrSize),
        "get client memory addr");

    constexpr size_t sendSize = kMaxMsgSize / 128 * (128 + 32);

    char *localMemAddrInChar = static_cast<char *>(aRscs.localMemAddr);
    for (uint32_t i = 0; i < aCfg.nbPipelineStages; i++) {
        doca_buf *recvBuf, *rdncBuf, *sendBuf, *clientBuf;
        CHECK_RETURN(doca_buf_inventory_buf_get_by_data(
                         aRscs.bufInv, aRscs.localMmap, localMemAddrInChar,
                         kMaxMsgSize, &recvBuf),
                     "get recv buf by addr");
        aRscs.recvBufs.push_back(recvBuf);

        CHECK_RETURN(
            doca_buf_inventory_buf_get_by_addr(aRscs.bufInv, aRscs.localMmap,
                                               localMemAddrInChar + kMaxMsgSize,
                                               kMaxMsgSize, &rdncBuf),
            "get rdnc buf by addr");
        aRscs.rdncBufs.push_back(rdncBuf);

        CHECK_RETURN(doca_buf_inventory_buf_get_by_data(
                         aRscs.bufInv, aRscs.localMmap, localMemAddrInChar,
                         sendSize, &sendBuf),
                     "get send buf by data");
        aRscs.sendBufs.push_back(sendBuf);

        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.clientMmap, clientMemAddr,
                         sendSize, &clientBuf),
                     "get client buf by addr");
        aRscs.clientBufs.push_back(clientBuf);
    }
    return DOCA_SUCCESS;
}

static void destroyBufs(ReplicaRscs &aRscs) {
    for (doca_buf *&clientBuf : aRscs.clientBufs) {
        CHECK_LOG(doca_buf_dec_refcount(clientBuf, nullptr),
                  "dec client buf ref cnt");
    }

    for (doca_buf *&recvBuf : aRscs.recvBufs) {
        CHECK_LOG(doca_buf_dec_refcount(recvBuf, nullptr),
                  "dec recv buf ref cnt");
    }

    for (doca_buf *&rdncBuf : aRscs.rdncBufs) {
        CHECK_LOG(doca_buf_dec_refcount(rdncBuf, nullptr),
                  "dec rdnc buf ref cnt");
    }

    for (doca_buf *&sendBuf : aRscs.sendBufs) {
        CHECK_LOG(doca_buf_dec_refcount(sendBuf, nullptr),
                  "dec send buf ref cnt");
    }
}

static doca_error_t initTasks(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    for (uint32_t i = 0; i < aCfg.nbPipelineStages; i++) {
        ReplicaUserData userData = {.rscs = aRscs, .cfg = aCfg, .stageId = i};
        aRscs.userDatas.push_back(userData);
    }

    for (uint32_t i = 0; i < aCfg.nbPipelineStages; i++) {
        doca_ec_task_create *ecTask;
        CHECK_RETURN(
            doca_ec_task_create_allocate_init(
                aRscs.ec, aRscs.mat, aRscs.recvBufs[i], aRscs.rdncBufs[i],
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

doca_error_t init(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    aRscs.requests = load_requests_text("./extracted_output.txt");
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");

    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    CHECK_RETURN(initMemory(8192, aRscs.dev, aCfg.mmapSize, aRscs.localMemAddr,
                            aRscs.localMmap, aRscs.bufInv),
                 "init memory");

    doca_ec_matrix *dummyMat = nullptr;
    CHECK_RETURN(initEc(aRscs.dev, aRscs.pe, kNbDataBlks, kNbRdncBlks, nullptr,
                        0, ecSuccCb, ecErrCb, nullptr, nullptr, aRscs.ec,
                        aRscs.mat, dummyMat, aRscs.ecCtx),
                 "init ec");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          WriteSuccCb, WriteErrCb, nullptr, nullptr, nullptr,
                          nullptr, aRscs.rdma, aRscs.rdmaCtx),
                 "init rdma");

    CHECK_RETURN(
        rdmaConnectToClient(aRscs.dev, aRscs.port, aRscs.rdma, aRscs.threadId,
                            aRscs.clientConn, aRscs.clientMmap),
        "connect to client");

    CHECK_RETURN(initBufs(aCfg, aRscs), "init bufs");

    CHECK_RETURN(initTasks(aCfg, aRscs), "init tasks");

    return DOCA_SUCCESS;
}

void runTasks(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    for (uint32_t i = 0; i < aCfg.nbPipelineStages; i++) {
        aRscs.requestIds.push_back(i);
        aRscs.beginTimes.push_back(std::chrono::high_resolution_clock::now());
        CHECK_LOG(
            doca_task_submit(doca_ec_task_create_as_task(aRscs.ecTasks[i])),
            "submit ec task");
    }

    while (!gForceQuit && aRscs.nbFreedTasks < aCfg.nbPipelineStages) {
        doca_pe_progress(aRscs.pe);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    for (u32 i = 0; i < aRscs.nbFinishedTasks; i++) {
        double timeCost = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              aRscs.endTimes[i] - aRscs.beginTimes[i])
                              .count() /
                          1000.0;
        aRscs.timeCosts.push_back(timeCost);
    }
}

void destroy(ReplicaRscs &aRscs) {
    destroyEc(aRscs.mat, nullptr, aRscs.ec, aRscs.ecCtx);
    destroyRdma(aRscs.rdma, aRscs.rdmaCtx);
    destroyBufs(aRscs);
    // CHECK_LOG(doca_mmap_destroy(aRscs.hostMmap), "destroy host mmap");
    CHECK_LOG(doca_mmap_destroy(aRscs.clientMmap), "destroy client mmap");
    destroyMemory(aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}