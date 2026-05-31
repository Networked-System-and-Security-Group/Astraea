#pragma once

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_erasure_coding.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using u32 = uint32_t;

constexpr u32 kNbDataBlks = 128;
constexpr u32 kNbRdncBlks = 32;
constexpr size_t kDmaChunkSize = 2 * 1024 * 1024;              /* 2MB */
constexpr size_t kMaxBlockSize = 1024 * 1024;                  /* 1MB */
constexpr size_t kMinBlockSize = 64;
constexpr size_t kMinDataSize = kMinBlockSize * kNbDataBlks;   /* 8KB */
constexpr size_t kMaxDataSize = kMaxBlockSize * kNbDataBlks;   /* 128MB */
constexpr size_t kMaxRdncSize = kMaxBlockSize * kNbRdncBlks;   /* 32MB */
constexpr size_t kStageMemSize = kMaxDataSize + kMaxRdncSize;  /* 160MB */
constexpr size_t kHostHalfSize = kStageMemSize;                /* 160MB */

constexpr u32 kMaxReadChunks =
    (kMaxDataSize + kDmaChunkSize - 1) / kDmaChunkSize;        /* 64 */
constexpr u32 kMaxRdncChunks =
    (kMaxRdncSize + kDmaChunkSize - 1) / kDmaChunkSize;        /* 16 */

struct Request {
    uint64_t ts;
    uint64_t size;
};

enum class DmaPhase : u32 { Read, WriteData, WriteRdnc };

enum class StageState : u32 {
    WaitingToSubmit,
    ReadInFlight,
    EcInFlight,
    WriteInFlight,
    Done,
};

struct alignas(64) LocalEcRscs;

struct StageCtx {
    LocalEcRscs *rscs;
    u32 stageId;
    StageState state;

    /* Per-request state */
    u32 reqIdx;
    size_t rounded;
    size_t rdncBytes;
    u32 nbReadChunks;
    u32 nbWriteDataChunks;
    u32 nbWriteRdncChunks;
    u32 nbReadDone;
    u32 nbWriteDone;

    std::chrono::high_resolution_clock::time_point targetTime;
    std::chrono::high_resolution_clock::time_point reqBegin;
};

struct DmaTaskCtx {
    StageCtx *stage;
    DmaPhase phase;
};

struct StageBufs {
    doca_buf *ecDataBuf;
    doca_buf *ecRdncBuf;

    std::vector<doca_buf *> readSrcBufs;       /* host src half slices */
    std::vector<doca_buf *> readDstBufs;       /* DPU data area slices */
    std::vector<doca_buf *> writeDataSrcBufs;  /* DPU data area slices */
    std::vector<doca_buf *> writeDataDstBufs;  /* host dst data slices */
    std::vector<doca_buf *> writeRdncSrcBufs;  /* DPU rdnc area slices */
    std::vector<doca_buf *> writeRdncDstBufs;  /* host dst rdnc slices */
};

struct StageTasks {
    std::vector<doca_dma_task_memcpy *> readTasks;
    std::vector<doca_dma_task_memcpy *> writeDataTasks;
    std::vector<doca_dma_task_memcpy *> writeRdncTasks;
    std::vector<DmaTaskCtx> readCtxs;
    std::vector<DmaTaskCtx> writeDataCtxs;
    std::vector<DmaTaskCtx> writeRdncCtxs;
    doca_ec_task_create *ecTask;
};

struct alignas(64) LocalEcRscs {
    doca_pe *pe;
    doca_dev *dev;

    /* DMA resources */
    doca_dma *dma;
    doca_ctx *dmaCtx;

    /* EC resources */
    doca_ec *ec;
    doca_ctx *ecCtx;
    doca_ec_matrix *encMat;

    /* Memory resources */
    doca_buf_inventory *bufInv;
    doca_mmap *localMmap;
    void *localMemAddr;
    doca_mmap *hostMmap;
    void *hostMemAddr;

    /* Per-stage resources */
    std::vector<StageCtx> stageCtxs;
    std::vector<StageBufs> stageBufs;
    std::vector<StageTasks> stageTasks;

    /* Trace (owned) */
    std::vector<Request> requests;
    u32 nbRequests;        /* effective request count */
    uint64_t traceT0;      /* trace tick at request 0 */

    /* Stats */
    std::vector<double> jcts;        /* per-request JCT in microseconds */
    double nbProcessedGB;
    u32 nbCompletedReqs;

    /* Misc */
    uint16_t port;
    std::chrono::high_resolution_clock::time_point wallStart;
};

struct alignas(64) LocalEcCfg {
    char ibdevName[1024];
    char tracePath[1024];
    u32 nbStages;          /* concurrent pipelines within one thread */
    u32 nbRequests;        /* 0 = use entire trace */
    double traceTickUs;    /* microseconds per trace tick */
    double replaySpeed;    /* trace replay speed multiplier */
    uint16_t port;
};

doca_error_t init(const LocalEcCfg &aCfg, LocalEcRscs &aRscs);
void destroy(LocalEcRscs &aRscs);
void runTasks(const LocalEcCfg &aCfg, LocalEcRscs &aRscs);
std::vector<Request> loadTrace(const char *aPath);
