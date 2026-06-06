#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "common.h"
#include "dma.h"
#include "ec.h"
#include "localec_dpu.h"
#include "memory.h"

DOCA_LOG_REGISTER(LOCALEC:DPU : CORE);

extern bool gForceQuit;

/* ===================== Trace parser ===================== */

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
            DOCA_LOG_ERR("Trace %s: unexpected EOF at line %zu", aPath, i);
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

/* ===================== Request helpers ===================== */

static inline size_t roundUp(size_t aSize, size_t aGran) {
    return (aSize + aGran - 1) / aGran * aGran;
}

static void computeReqParams(StageCtx &aStage, size_t aRawSize) {
    size_t capped = std::min<size_t>(aRawSize, kMaxDataSize);
    size_t rounded = roundUp(capped, kMinDataSize);
    if (rounded == 0) rounded = kMinDataSize;
    size_t blockSize = rounded / kNbDataBlks;
    aStage.rounded = rounded;
    aStage.rdncBytes = blockSize * kNbRdncBlks;
}

static u32 ecTaskIdx(const StageCtx &aStage) {
    return static_cast<u32>(aStage.rounded / kMinDataSize) - 1;
}

static std::chrono::high_resolution_clock::time_point computeTarget(
    const LocalEcRscs &aRscs, const LocalEcCfg &aCfg, u32 aReqIdx) {
    if (aReqIdx >= aRscs.requests.size()) return aRscs.wallStart;
    uint64_t tick = aRscs.requests[aReqIdx].ts;
    uint64_t deltaTicks = tick >= aRscs.traceT0 ? tick - aRscs.traceT0 : 0;
    double deltaUs =
        static_cast<double>(deltaTicks) * aCfg.traceTickUs / aCfg.replaySpeed;
    auto deltaNs =
        std::chrono::nanoseconds(static_cast<int64_t>(deltaUs * 1000.0));
    return aRscs.wallStart + deltaNs;
}

/* ===================== Forward decls ===================== */

static void submitRead(StageCtx &aStage);
static void submitEc(StageCtx &aStage);
static void submitWrites(StageCtx &aStage);
static void onRequestDone(StageCtx &aStage);

/* ===================== Callbacks ===================== */

static void dmaSuccCb(doca_dma_task_memcpy *task, doca_data task_user_data,
                      doca_data ctx_user_data) {
    DmaTaskCtx *ctx = static_cast<DmaTaskCtx *>(task_user_data.ptr);
    StageCtx &stage = *ctx->stage;
    if (ctx->phase == DmaPhase::Read) {
        if (gForceQuit) {
            stage.state = StageState::Done;
        } else {
            submitEc(stage);
        }
    } else {
        onRequestDone(stage);
    }
}

static void dmaErrCb(doca_dma_task_memcpy *task, doca_data task_user_data,
                     doca_data ctx_user_data) {
    DmaTaskCtx *ctx = static_cast<DmaTaskCtx *>(task_user_data.ptr);
    StageCtx &stage = *ctx->stage;
    DOCA_LOG_ERR("DMA task failed: stage=%u phase=%u", stage.stageId,
                 static_cast<u32>(ctx->phase));
    gForceQuit = true;
    stage.state = StageState::Done;
}

static void ecSuccCb(doca_ec_task_create *task, doca_data task_user_data,
                     doca_data ctx_user_data) {
    StageCtx &stage = *static_cast<StageCtx *>(task_user_data.ptr);
    if (gForceQuit) {
        stage.state = StageState::Done;
        return;
    }
    submitWrites(stage);
}

static void ecErrCb(doca_ec_task_create *task, doca_data task_user_data,
                    doca_data ctx_user_data) {
    StageCtx &stage = *static_cast<StageCtx *>(task_user_data.ptr);
    DOCA_LOG_ERR("EC task failed at stage %u req %u", stage.stageId,
                 stage.reqIdx);
    gForceQuit = true;
    stage.state = StageState::Done;
}

/* ===================== Phase submitters ===================== */

static void submitRead(StageCtx &aStage) {
    LocalEcRscs &rscs = *aStage.rscs;
    StageBufs &bufs = rscs.stageBufs[aStage.stageId];
    StageTasks &tasks = rscs.stageTasks[aStage.stageId];

    aStage.reqBegin = std::chrono::high_resolution_clock::now();
    aStage.state = StageState::ReadInFlight;

    CHECK_LOG(doca_buf_set_data_len(bufs.readSrcBuf, aStage.rounded),
              "set read src data len");
    CHECK_LOG(doca_buf_set_data_len(bufs.readDstBuf, 0),
              "reset read dst data len");
    CHECK_LOG(doca_task_submit(doca_dma_task_memcpy_as_task(tasks.readTask)),
              "submit dma read task");
}

static void submitEc(StageCtx &aStage) {
    LocalEcRscs &rscs = *aStage.rscs;
    StageBufs &bufs = rscs.stageBufs[aStage.stageId];
    StageTasks &tasks = rscs.stageTasks[aStage.stageId];
    u32 taskIdx = ecTaskIdx(aStage);

    aStage.state = StageState::EcInFlight;
    CHECK_LOG(doca_buf_set_data_len(bufs.ecDataBufs[taskIdx], aStage.rounded),
              "set ec data buf len");
    CHECK_LOG(doca_buf_set_data_len(bufs.ecRdncBufs[taskIdx], 0),
              "reset ec rdnc buf len");
    CHECK_LOG(
        doca_task_submit(doca_ec_task_create_as_task(tasks.ecTasks[taskIdx])),
        "submit ec task");
}

static void submitWrites(StageCtx &aStage) {
    LocalEcRscs &rscs = *aStage.rscs;
    StageBufs &bufs = rscs.stageBufs[aStage.stageId];
    StageTasks &tasks = rscs.stageTasks[aStage.stageId];

    aStage.state = StageState::WriteInFlight;
    size_t writeBytes = aStage.rounded + aStage.rdncBytes;

    CHECK_LOG(doca_buf_set_data_len(bufs.writeSrcBuf, writeBytes),
              "set combined write src len");
    CHECK_LOG(doca_buf_set_data_len(bufs.writeDstBuf, 0),
              "reset combined write dst len");
    CHECK_LOG(doca_task_submit(doca_dma_task_memcpy_as_task(tasks.writeTask)),
              "submit combined dma write task");
}

static void onRequestDone(StageCtx &aStage) {
    LocalEcRscs &rscs = *aStage.rscs;
    auto end = std::chrono::high_resolution_clock::now();
    double jctUs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       end - aStage.reqBegin)
                       .count() /
                   1000.0;
    rscs.jcts.push_back(jctUs);
    rscs.nbProcessedGB +=
        static_cast<double>(aStage.rounded + aStage.rdncBytes) /
        (1024.0 * 1024.0 * 1024.0);
    rscs.nbCompletedReqs++;

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

/* ===================== Init ===================== */

static doca_error_t initStageBufs(const LocalEcCfg &aCfg, LocalEcRscs &aRscs) {
    void *hostBase;
    size_t hostSize;
    CHECK_RETURN(doca_mmap_get_memrange(aRscs.hostMmap, &hostBase, &hostSize),
                 "get host mmap range");
    if (hostSize < kHostHalfSize + kStageMemSize) {
        DOCA_LOG_ERR("Host mmap too small: have %zu bytes, need %zu bytes",
                     hostSize, kHostHalfSize + kStageMemSize);
        return DOCA_ERROR_NO_MEMORY;
    }

    char *hostSrc = static_cast<char *>(hostBase);
    char *hostDst = hostSrc + kHostHalfSize;

    char *localBase = static_cast<char *>(aRscs.localMemAddr);

    aRscs.stageBufs.resize(aCfg.nbStages);
    for (u32 s = 0; s < aCfg.nbStages; ++s) {
        StageBufs &bufs = aRscs.stageBufs[s];
        char *stageData = localBase + s * kStageMemSize;

        /* One EC buffer pair per possible rounded size. The rdnc buffer starts
         * exactly after the data bytes, so data+rdnc is contiguous. */
        bufs.ecDataBufs.resize(kNbEcTaskVariants);
        bufs.ecRdncBufs.resize(kNbEcTaskVariants);
        for (u32 i = 0; i < kNbEcTaskVariants; ++i) {
            size_t rounded = static_cast<size_t>(i + 1) * kMinDataSize;
            size_t rdncBytes = rounded / kNbDataBlks * kNbRdncBlks;
            CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                             aRscs.bufInv, aRscs.localMmap, stageData, rounded,
                             &bufs.ecDataBufs[i]),
                         "get ec data buf");
            CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                             aRscs.bufInv, aRscs.localMmap,
                             stageData + rounded, rdncBytes,
                             &bufs.ecRdncBufs[i]),
                         "get ec rdnc buf");
        }

        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.hostMmap, hostSrc, kMaxDataSize,
                         &bufs.readSrcBuf),
                     "get read src buf");
        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.localMmap, stageData,
                         kMaxDataSize, &bufs.readDstBuf),
                     "get read dst buf");
        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.localMmap, stageData,
                         kStageMemSize, &bufs.writeSrcBuf),
                     "get combined write src buf");
        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.hostMmap, hostDst, kStageMemSize,
                         &bufs.writeDstBuf),
                     "get combined write dst buf");
    }
    return DOCA_SUCCESS;
}

static doca_error_t initStageTasks(const LocalEcCfg &aCfg, LocalEcRscs &aRscs) {
    aRscs.stageTasks.resize(aCfg.nbStages);
    for (u32 s = 0; s < aCfg.nbStages; ++s) {
        StageBufs &bufs = aRscs.stageBufs[s];
        StageTasks &tasks = aRscs.stageTasks[s];
        StageCtx *stagePtr = &aRscs.stageCtxs[s];

        tasks.readCtx = {.stage = stagePtr, .phase = DmaPhase::Read};
        CHECK_RETURN(doca_dma_task_memcpy_alloc_init(
                         aRscs.dma, bufs.readSrcBuf, bufs.readDstBuf,
                         {.ptr = &tasks.readCtx}, &tasks.readTask),
                     "alloc dma read task");

        tasks.writeCtx = {.stage = stagePtr, .phase = DmaPhase::Write};
        CHECK_RETURN(doca_dma_task_memcpy_alloc_init(
                         aRscs.dma, bufs.writeSrcBuf, bufs.writeDstBuf,
                         {.ptr = &tasks.writeCtx}, &tasks.writeTask),
                     "alloc combined dma write task");

        tasks.ecTasks.resize(kNbEcTaskVariants);
        for (u32 i = 0; i < kNbEcTaskVariants; ++i) {
            CHECK_RETURN(doca_ec_task_create_allocate_init(
                             aRscs.ec, aRscs.encMat, bufs.ecDataBufs[i],
                             bufs.ecRdncBufs[i], {.ptr = stagePtr},
                             &tasks.ecTasks[i]),
                         "alloc ec task");
        }
    }
    return DOCA_SUCCESS;
}

doca_error_t init(const LocalEcCfg &aCfg, LocalEcRscs &aRscs) {
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

    CHECK_RETURN(dmaImportFromClient(aCfg.port, aRscs.dev, aRscs.hostMmap),
                 "import host mmap");

    /* Local mmap: one stage region per pipeline stage. */
    size_t localMmapSize = static_cast<size_t>(aCfg.nbStages) * kStageMemSize;
    size_t maxNbBufs =
        static_cast<size_t>(aCfg.nbStages) * (2 * kNbEcTaskVariants + 4) + 16;
    CHECK_RETURN(initMemory(maxNbBufs, aRscs.dev, localMmapSize,
                            aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv),
                 "init local memory");

    CHECK_RETURN(initDma(aRscs.dev, aRscs.pe, dmaSuccCb, dmaErrCb, aRscs.dma,
                         aRscs.dmaCtx),
                 "init dma");

    doca_ec_matrix *dummyDec = nullptr;
    CHECK_RETURN(initEc(aRscs.dev, aRscs.pe, kNbDataBlks, kNbRdncBlks, nullptr,
                        0, ecSuccCb, ecErrCb, nullptr, nullptr, aRscs.ec,
                        aRscs.encMat, dummyDec, aRscs.ecCtx),
                 "init ec");

    /* Reserve capacity so addresses stay stable for pointers into stageCtxs. */
    aRscs.stageCtxs.reserve(aCfg.nbStages);
    for (u32 s = 0; s < aCfg.nbStages; ++s) {
        StageCtx ctx{};
        ctx.rscs = &aRscs;
        ctx.stageId = s;
        ctx.state = StageState::WaitingToSubmit;
        ctx.reqIdx = s;
        aRscs.stageCtxs.push_back(ctx);
    }

    CHECK_RETURN(initStageBufs(aCfg, aRscs), "init stage bufs");
    CHECK_RETURN(initStageTasks(aCfg, aRscs), "init stage tasks");

    return DOCA_SUCCESS;
}

/* ===================== Run loop ===================== */

void runTasks(const LocalEcCfg &aCfg, LocalEcRscs &aRscs) {
    aRscs.wallStart = std::chrono::high_resolution_clock::now();
    for (StageCtx &stage : aRscs.stageCtxs) {
        if (stage.reqIdx >= aRscs.nbRequests) {
            stage.state = StageState::Done;
            continue;
        }
    }

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
            if (gForceQuit) {
                stage.state = StageState::Done;
                continue;
            }
            stage.targetTime = computeTarget(aRscs, aCfg, stage.reqIdx);
            if (now < stage.targetTime) continue;
            computeReqParams(stage, aRscs.requests[stage.reqIdx].size);
            submitRead(stage);
        }
    }

    DOCA_LOG_INFO("Completed %u requests, processed %.2f GB",
                  aRscs.nbCompletedReqs, aRscs.nbProcessedGB);
}

/* ===================== Destroy ===================== */

static void destroyStageTasks(LocalEcRscs &aRscs) {
    for (StageTasks &tasks : aRscs.stageTasks) {
        if (tasks.readTask) {
            doca_task_free(doca_dma_task_memcpy_as_task(tasks.readTask));
        }
        if (tasks.writeTask) {
            doca_task_free(doca_dma_task_memcpy_as_task(tasks.writeTask));
        }
        for (doca_ec_task_create *t : tasks.ecTasks) {
            if (t) doca_task_free(doca_ec_task_create_as_task(t));
        }
    }
}

static void destroyStageBufs(LocalEcRscs &aRscs) {
    for (StageBufs &bufs : aRscs.stageBufs) {
        auto dec = [](doca_buf *b) {
            if (b)
                CHECK_LOG(doca_buf_dec_refcount(b, nullptr),
                          "dec buf refcount");
        };
        for (doca_buf *b : bufs.ecDataBufs) dec(b);
        for (doca_buf *b : bufs.ecRdncBufs) dec(b);
        dec(bufs.readSrcBuf);
        dec(bufs.readDstBuf);
        dec(bufs.writeSrcBuf);
        dec(bufs.writeDstBuf);
    }
}

void destroy(LocalEcRscs &aRscs) {
    destroyStageTasks(aRscs);
    destroyEc(aRscs.encMat, nullptr, aRscs.ec, aRscs.ecCtx);
    destroyDma(aRscs.dma, aRscs.dmaCtx);
    destroyStageBufs(aRscs);
    CHECK_LOG(doca_mmap_destroy(aRscs.hostMmap), "destroy host mmap");
    destroyMemory(aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close device");
}
