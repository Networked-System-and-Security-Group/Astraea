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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "cdn_dpu.h"
#include "common.h"
#include "ec.h"
#include "memory.h"
#include "rdma.h"

DOCA_LOG_REGISTER(CDN:DPU : CORE);

extern bool gForceQuit;

std::vector<Request> loadTrace(const char *aPath) {
    std::ifstream ifs(aPath);
    std::vector<Request> reqs;
    if (!ifs) {
        DOCA_LOG_ERR("Failed to open trace %s", aPath);
        return reqs;
    }

    std::string line;
    auto nextDataLine = [&]() -> bool {
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            if (line[0] == '#') continue;
            return true;
        }
        return false;
    };

    if (!nextDataLine()) {
        DOCA_LOG_ERR("Trace %s is empty", aPath);
        return reqs;
    }

    size_t n = 0;
    {
        std::istringstream iss(line);
        if (!(iss >> n)) {
            DOCA_LOG_ERR("Trace %s: bad count line", aPath);
            return reqs;
        }
    }
    reqs.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        if (!nextDataLine()) {
            DOCA_LOG_ERR("Trace %s: unexpected EOF at request %zu", aPath, i);
            break;
        }
        std::istringstream iss(line);
        uint64_t ts = 0, sz = 0;
        if (!(iss >> ts >> sz)) {
            DOCA_LOG_ERR("Trace %s: bad data line %zu: %s", aPath, i,
                         line.c_str());
            break;
        }
        reqs.push_back({ts, sz});
    }
    return reqs;
}

static inline size_t roundUp(size_t aSize, size_t aGran) {
    return (aSize + aGran - 1) / aGran * aGran;
}

static void computeReqParams(StageCtx &aStage, size_t aRawSize) {
    size_t capped = std::min<size_t>(aRawSize, kMaxDataSize);
    if (capped == 0) capped = kMinDataSize;

    aStage.wireBytes = std::min<size_t>(roundUp(capped, 64), kMaxDataSize);
    aStage.ecBytes = roundUp(aStage.wireBytes, kMinDataSize);
    size_t blockSize = aStage.ecBytes / kNbDataBlks;
    aStage.rdncBytes = blockSize * kNbRdncBlks;
}

static std::chrono::high_resolution_clock::time_point computeTarget(
    const CdnRscs &aRscs, const CdnCfg &aCfg, u32 aReqIdx) {
    if (aReqIdx >= aRscs.nbRequests) return aRscs.wallStart;
    uint64_t tick = aRscs.requests[aReqIdx].ts;
    uint64_t deltaTicks = tick >= aRscs.traceT0 ? tick - aRscs.traceT0 : 0;
    double deltaUs =
        static_cast<double>(deltaTicks) * aCfg.traceTickUs / aCfg.replaySpeed;
    auto deltaNs =
        std::chrono::nanoseconds(static_cast<int64_t>(deltaUs * 1000.0));
    return aRscs.wallStart + deltaNs;
}

static void submitEc(StageCtx &aStage);
static void submitWrite(StageCtx &aStage);
static void onRequestDone(StageCtx &aStage);

static void ecSuccCb(doca_ec_task_create *task, doca_data task_user_data,
                     doca_data ctx_user_data) {
    StageCtx &stage = *static_cast<StageCtx *>(task_user_data.ptr);
    if (gForceQuit) {
        stage.state = StageState::Done;
        return;
    }
    submitWrite(stage);
}

static void ecErrCb(doca_ec_task_create *task, doca_data task_user_data,
                    doca_data ctx_user_data) {
    StageCtx &stage = *static_cast<StageCtx *>(task_user_data.ptr);
    DOCA_LOG_ERR("EC task failed at stage %u req %u", stage.stageId,
                 stage.reqIdx);
    gForceQuit = true;
    stage.state = StageState::Done;
}

static void writeSuccCb(doca_rdma_task_write *task, doca_data task_user_data,
                        doca_data ctx_user_data) {
    StageCtx &stage = *static_cast<StageCtx *>(task_user_data.ptr);
    onRequestDone(stage);
}

static void writeErrCb(doca_rdma_task_write *task, doca_data task_user_data,
                       doca_data ctx_user_data) {
    StageCtx &stage = *static_cast<StageCtx *>(task_user_data.ptr);
    DOCA_LOG_ERR("RDMA write failed at stage %u req %u", stage.stageId,
                 stage.reqIdx);
    gForceQuit = true;
    stage.state = StageState::Done;
}

static void submitEc(StageCtx &aStage) {
    CdnRscs &rscs = *aStage.rscs;
    StageBufs &bufs = rscs.stageBufs[aStage.stageId];
    StageTasks &tasks = rscs.stageTasks[aStage.stageId];

    aStage.reqBegin = std::chrono::high_resolution_clock::now();
    aStage.state = StageState::EcInFlight;

    CHECK_LOG(doca_buf_set_data_len(bufs.dataBuf, aStage.ecBytes),
              "set ec data len");
    CHECK_LOG(doca_buf_set_data_len(bufs.rdncBuf, 0),
              "reset ec rdnc len");
    CHECK_LOG(doca_task_submit(doca_ec_task_create_as_task(tasks.ecTask)),
              "submit ec task");
}

static void submitWrite(StageCtx &aStage) {
    CdnRscs &rscs = *aStage.rscs;
    StageBufs &bufs = rscs.stageBufs[aStage.stageId];
    StageTasks &tasks = rscs.stageTasks[aStage.stageId];

    /* EC completion gates the response; the client receives request bytes. */
    aStage.state = StageState::WriteInFlight;
    CHECK_LOG(doca_buf_set_data_len(bufs.dataBuf, aStage.wireBytes),
              "set rdma write len");
    CHECK_LOG(doca_buf_set_data_len(bufs.clientBuf, 0),
              "reset client buf len");
    CHECK_LOG(doca_task_submit(doca_rdma_task_write_as_task(tasks.writeTask)),
              "submit rdma write task");
}

static void onRequestDone(StageCtx &aStage) {
    CdnRscs &rscs = *aStage.rscs;
    auto end = std::chrono::high_resolution_clock::now();
    double jctUs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       end - aStage.reqBegin)
                       .count() /
                   1000.0;
    rscs.jcts.push_back(jctUs);
    rscs.nbWrittenGB +=
        static_cast<double>(aStage.wireBytes) / (1024.0 * 1024.0 * 1024.0);
    rscs.nbEcGB += static_cast<double>(aStage.ecBytes + aStage.rdncBytes) /
                   (1024.0 * 1024.0 * 1024.0);
    rscs.nbCompletedReqs++;
    if (rscs.nbCompletedReqs % 1000 == 0) {
        DOCA_LOG_INFO("Thread %u completed %u requests", rscs.threadId,
                      rscs.nbCompletedReqs);
    }

    if (gForceQuit) {
        aStage.state = StageState::Done;
        return;
    }

    u32 nbStages = static_cast<u32>(rscs.stageCtxs.size());
    u32 nextIdx = aStage.reqIdx + nbStages;
    if (nextIdx >= rscs.nbRequests) {
        aStage.state = StageState::Done;
        return;
    }

    aStage.reqIdx = nextIdx;
    aStage.state = StageState::WaitingToSubmit;
}

static doca_error_t initStageBufs(const CdnCfg &aCfg, CdnRscs &aRscs) {
    char *clientBase;
    size_t clientSize;
    CHECK_RETURN(
        doca_mmap_get_memrange(aRscs.clientMmap,
                               reinterpret_cast<void **>(&clientBase),
                               &clientSize),
        "get client mmap range");

    size_t requiredClientBytes =
        static_cast<size_t>(aCfg.nbPipelineStages) * kMaxDataSize;
    if (clientSize < requiredClientBytes) {
        DOCA_LOG_ERR("Client mmap too small: have %zu bytes, need %zu bytes",
                     clientSize, requiredClientBytes);
        return DOCA_ERROR_NO_MEMORY;
    }

    char *localBase = static_cast<char *>(aRscs.localMemAddr);
    aRscs.stageBufs.resize(aCfg.nbPipelineStages);

    for (u32 s = 0; s < aCfg.nbPipelineStages; ++s) {
        StageBufs &bufs = aRscs.stageBufs[s];
        char *stageData = localBase + static_cast<size_t>(s) * kStageMemSize;
        char *stageRdnc = stageData + kMaxDataSize;
        char *clientDst = clientBase + static_cast<size_t>(s) * kMaxDataSize;

        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.localMmap, stageData,
                         kMaxDataSize, &bufs.dataBuf),
                     "get stage data buf");
        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.localMmap, stageRdnc,
                         kMaxRdncSize, &bufs.rdncBuf),
                     "get stage rdnc buf");
        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.clientMmap, clientDst,
                         kMaxDataSize, &bufs.clientBuf),
                     "get client dst buf");
    }

    return DOCA_SUCCESS;
}

static void destroyStageBufs(CdnRscs &aRscs) {
    for (StageBufs &bufs : aRscs.stageBufs) {
        if (bufs.dataBuf)
            CHECK_LOG(doca_buf_dec_refcount(bufs.dataBuf, nullptr),
                      "dec data buf");
        if (bufs.rdncBuf)
            CHECK_LOG(doca_buf_dec_refcount(bufs.rdncBuf, nullptr),
                      "dec rdnc buf");
        if (bufs.clientBuf)
            CHECK_LOG(doca_buf_dec_refcount(bufs.clientBuf, nullptr),
                      "dec client buf");
    }
}

static doca_error_t initStageTasks(const CdnCfg &aCfg, CdnRscs &aRscs) {
    aRscs.stageTasks.resize(aCfg.nbPipelineStages);
    for (u32 s = 0; s < aCfg.nbPipelineStages; ++s) {
        StageBufs &bufs = aRscs.stageBufs[s];
        StageTasks &tasks = aRscs.stageTasks[s];
        StageCtx *stage = &aRscs.stageCtxs[s];

        CHECK_RETURN(doca_ec_task_create_allocate_init(
                         aRscs.ec, aRscs.encMat, bufs.dataBuf, bufs.rdncBuf,
                         {.ptr = stage}, &tasks.ecTask),
                     "alloc ec task");
        CHECK_RETURN(doca_rdma_task_write_allocate_init(
                         aRscs.rdma, aRscs.clientConn, bufs.dataBuf,
                         bufs.clientBuf, {.ptr = stage}, &tasks.writeTask),
                     "alloc rdma write task");
    }
    return DOCA_SUCCESS;
}

static void destroyStageTasks(CdnRscs &aRscs) {
    for (StageTasks &tasks : aRscs.stageTasks) {
        if (tasks.ecTask)
            doca_task_free(doca_ec_task_create_as_task(tasks.ecTask));
        if (tasks.writeTask)
            doca_task_free(doca_rdma_task_write_as_task(tasks.writeTask));
    }
}

doca_error_t init(const CdnCfg &aCfg, CdnRscs &aRscs) {
    aRscs.requests = loadTrace(aCfg.tracePath);
    if (aRscs.requests.empty()) {
        DOCA_LOG_ERR("No requests loaded from %s", aCfg.tracePath);
        return DOCA_ERROR_INVALID_VALUE;
    }
    aRscs.nbRequests =
        (aCfg.nbRequests == 0)
            ? static_cast<u32>(aRscs.requests.size())
            : std::min<u32>(aCfg.nbRequests,
                            static_cast<u32>(aRscs.requests.size()));
    aRscs.traceT0 = aRscs.requests[0].ts;

    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");
    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    size_t localMmapSize =
        static_cast<size_t>(aCfg.nbPipelineStages) * kStageMemSize;
    CHECK_RETURN(initMemory(aCfg.nbPipelineStages * 4 + 16, aRscs.dev,
                            localMmapSize, aRscs.localMemAddr,
                            aRscs.localMmap, aRscs.bufInv),
                 "init local memory");
    memset(aRscs.localMemAddr, 0x5a, localMmapSize);

    doca_ec_matrix *dummyDec = nullptr;
    CHECK_RETURN(initEc(aRscs.dev, aRscs.pe, kNbDataBlks, kNbRdncBlks, nullptr,
                        0, ecSuccCb, ecErrCb, nullptr, nullptr, aRscs.ec,
                        aRscs.encMat, dummyDec, aRscs.ecCtx),
                 "init ec");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          writeSuccCb, writeErrCb, nullptr, nullptr, nullptr,
                          nullptr, aRscs.rdma, aRscs.rdmaCtx),
                 "init rdma");

    CHECK_RETURN(
        rdmaConnectToClient(aRscs.dev, aRscs.port, aRscs.rdma, aRscs.threadId,
                            aRscs.clientConn, aRscs.clientMmap),
        "connect to client");

    aRscs.stageCtxs.reserve(aCfg.nbPipelineStages);
    for (u32 s = 0; s < aCfg.nbPipelineStages; ++s) {
        aRscs.stageCtxs.push_back({.rscs = &aRscs,
                                   .stageId = s,
                                   .state = StageState::WaitingToSubmit,
                                   .reqIdx = s});
    }

    CHECK_RETURN(initStageBufs(aCfg, aRscs), "init stage bufs");
    CHECK_RETURN(initStageTasks(aCfg, aRscs), "init stage tasks");

    return DOCA_SUCCESS;
}

void runTasks(const CdnCfg &aCfg, CdnRscs &aRscs) {
    aRscs.wallStart = std::chrono::high_resolution_clock::now();

    auto anyActive = [&]() {
        for (const StageCtx &stage : aRscs.stageCtxs) {
            if (stage.state != StageState::Done) return true;
        }
        return false;
    };

    while (anyActive()) {
        doca_pe_progress(aRscs.pe);

        auto now = std::chrono::high_resolution_clock::now();
        for (StageCtx &stage : aRscs.stageCtxs) {
            if (stage.state != StageState::WaitingToSubmit) continue;
            if (gForceQuit || stage.reqIdx >= aRscs.nbRequests) {
                stage.state = StageState::Done;
                continue;
            }

            auto target = computeTarget(aRscs, aCfg, stage.reqIdx);
            if (now < target) continue;

            computeReqParams(stage, aRscs.requests[stage.reqIdx].size);
            submitEc(stage);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }

    DOCA_LOG_INFO("Thread %u completed %u requests", aRscs.threadId,
                  aRscs.nbCompletedReqs);
}

void destroy(CdnRscs &aRscs) {
    destroyStageTasks(aRscs);
    destroyEc(aRscs.encMat, nullptr, aRscs.ec, aRscs.ecCtx);
    destroyRdma(aRscs.rdma, aRscs.rdmaCtx);
    destroyStageBufs(aRscs);
    CHECK_LOG(doca_mmap_destroy(aRscs.clientMmap), "destroy client mmap");
    destroyMemory(aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}
