#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>
#include <netinet/in.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "cdn_client.h"
#include "common.h"
#include "doca_buf.h"
#include "doca_buf_inventory.h"
#include "memory.h"
#include "rdma.h"

DOCA_LOG_REGISTER(CDN:CLIENT : CORE);

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

static void freeTasks(CdnClientRscs &aRscs, u32 aStageId) {
    doca_task_free(doca_rdma_task_receive_as_task(aRscs.recvTasks[aStageId]));
    doca_task_free(doca_rdma_task_write_imm_as_task(aRscs.immTasks[aStageId]));
}

void immSuccCb(doca_rdma_task_write_imm *task, doca_data task_user_data,
               doca_data ctx_user_data) {
    CdnClientUserData *userData =
        static_cast<CdnClientUserData *>(task_user_data.ptr);
    CdnClientRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;

    if (!gForceQuit) {
        CHECK_LOG(doca_buf_set_data_len(rscs.bufs[stageId], 0),
                  "set recv buf len to 0");
        CHECK_LOG(doca_task_submit(
                      doca_rdma_task_receive_as_task(rscs.recvTasks[stageId])),
                  "submit write imm task");
    } else {
        freeTasks(rscs, stageId);
        rscs.nbFreedTasks++;
    }
}

void immErrCb(doca_rdma_task_write_imm *task, doca_data task_user_data,
              doca_data ctx_user_data) {
    CdnClientUserData *userData =
        static_cast<CdnClientUserData *>(task_user_data.ptr);
    CdnClientRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;
    gForceQuit = true;

    freeTasks(rscs, stageId);
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("Write imm task failed");
}

void recvSuccCb(doca_rdma_task_receive *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    CdnClientUserData *userData =
        static_cast<CdnClientUserData *>(task_user_data.ptr);
    const CdnClientCfg &cfg = userData->cfg;
    CdnClientRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;
    rscs.endTimes.push_back(std::chrono::high_resolution_clock::now());

    if (!gForceQuit) {
        rscs.nbFinishedTasks++;
        const uint64_t t0 = rscs.requests[stageId].ts_ms;
        u32 &requestId = rscs.requestIds[stageId];
        rscs.nbProcessedGBits += rscs.requests[requestId].size * 8 / 1e9;

        requestId += cfg.nbPipelineStages;

        if (requestId < rscs.requests.size()) {
            uint64_t rel_ms = (rscs.requests[requestId].ts_ms >= t0)
                                  ? (rscs.requests[requestId].ts_ms - t0)
                                  : 0;
            auto target = gBeginTime + std::chrono::milliseconds(rel_ms);
            std::this_thread::sleep_until(target);

            doca_rdma_task_write_imm_set_immediate_data(
                rscs.immTasks[stageId], htonl(rscs.requests[stageId].size));

            CHECK_LOG(doca_task_submit(doca_rdma_task_write_imm_as_task(
                          rscs.immTasks[stageId])),
                      "submit write imm task");
        } else {
            gForceQuit = true;
        }
    }

    if (gForceQuit) {
        freeTasks(rscs, stageId);
        rscs.nbFreedTasks++;
    }
}

void recvErrCb(doca_rdma_task_receive *task, doca_data task_user_data,
               doca_data ctx_user_data) {
    CdnClientUserData *userData =
        static_cast<CdnClientUserData *>(task_user_data.ptr);
    CdnClientRscs &rscs = userData->rscs;
    u32 stageId = userData->stageId;
    gForceQuit = true;

    freeTasks(rscs, stageId);
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("Recv task failed");
}

static doca_error_t initTasks(const CdnClientCfg &aCfg, CdnClientRscs &aRscs) {
    for (u32 i = 0; i < aCfg.nbPipelineStages; ++i) {
        CdnClientUserData userData = {.rscs = aRscs, .cfg = aCfg, .stageId = i};
        aRscs.userDatas.push_back(userData);

        doca_rdma_task_write_imm *immTask;
        CHECK_RETURN(doca_rdma_task_write_imm_allocate_init(
                         aRscs.rdma, aRscs.conn, nullptr, nullptr, 8192,
                         {.ptr = &aRscs.userDatas[i]}, &immTask),
                     "alloc and init write imm task");
        aRscs.immTasks.push_back(immTask);

        doca_rdma_task_receive *recvTask;
        CHECK_RETURN(doca_rdma_task_receive_allocate_init(
                         aRscs.rdma, aRscs.bufs[i],
                         {.ptr = &aRscs.userDatas[i]}, &recvTask),
                     "alloc and init recv task");
        aRscs.recvTasks.push_back(recvTask);
    }

    return DOCA_SUCCESS;
}

doca_error_t initBufs(const CdnClientCfg &aCfg, CdnClientRscs &aRscs) {
    for (u32 i = 0; i < aCfg.nbPipelineStages; ++i) {
        doca_buf *buf;
        CHECK_RETURN(
            doca_buf_inventory_buf_get_by_addr(
                aRscs.bufInv, aRscs.mmap, aRscs.memAddr, aCfg.mmapSize, &buf),
            "get buf by addr");
        aRscs.bufs.push_back(buf);
    }
    return DOCA_SUCCESS;
}

void destroyBufs(CdnClientRscs &aRscs) {
    for (auto &buf : aRscs.bufs) {
        CHECK_LOG(doca_buf_dec_refcount(buf, nullptr), "dec buf ref count");
    }
}

doca_error_t init(const CdnClientCfg &aCfg, CdnClientRscs &aRscs) {
    aRscs.requests = load_requests_text("./output.txt");
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");
    /* Server only need pe to built connection */
    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    CHECK_RETURN(initMemory(8192, aRscs.dev, aCfg.mmapSize, aRscs.memAddr,
                            aRscs.mmap, aRscs.bufInv),
                 "init memory");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          nullptr, nullptr, recvSuccCb, recvErrCb, immSuccCb,
                          immErrCb, aRscs.rdma, aRscs.ctx),
                 "init rdma");
    CHECK_RETURN(initBufs(aCfg, aRscs), "init bufs");
    CHECK_RETURN(
        rdmaConnectToServer(aRscs.dev, aCfg.serverIpAddr, aRscs.port,
                            aRscs.rdma, aRscs.mmap, aRscs.threadId, aRscs.conn),
        "connect to server");

    CHECK_RETURN(initTasks(aCfg, aRscs), "init tasks");

    return DOCA_SUCCESS;
}

void runTasks(const CdnClientCfg &aCfg, CdnClientRscs &aRscs) {
    DOCA_LOG_INFO("Nb requests is %lu", aRscs.requests.size());

    for (u32 i = 0; i < aCfg.nbPipelineStages; ++i) {
        aRscs.requestIds.push_back(i);
        aRscs.beginTimes.push_back(std::chrono::high_resolution_clock::now());
        doca_rdma_task_write_imm_set_immediate_data(
            aRscs.immTasks[i], htonl(aRscs.requests[aRscs.requestIds[i]].size));
        CHECK_LOG(doca_task_submit(
                      doca_rdma_task_write_imm_as_task(aRscs.immTasks[i])),
                  "submit write imm task");
    }

    while (!gForceQuit) {
        doca_pe_progress(aRscs.pe);
    }

    while (aRscs.nbFreedTasks < aCfg.nbPipelineStages) {
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

void destroy(CdnClientRscs &aRscs) {
    destroyRdma(aRscs.rdma, aRscs.ctx);
    destroyBufs(aRscs);
    destroyMemory(aRscs.memAddr, aRscs.mmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}
