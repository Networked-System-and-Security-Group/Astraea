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
constexpr size_t kMinBlockSize = 64;
constexpr size_t kMinDataSize = kMinBlockSize * kNbDataBlks;   /* 8KB */
constexpr size_t kMaxDataSize =
    (kDmaChunkSize * kNbDataBlks / (kNbDataBlks + kNbRdncBlks) /
     kMinDataSize) *
    kMinDataSize;                                              /* 1671168B */
constexpr size_t kMaxBlockSize = kMaxDataSize / kNbDataBlks;   /* 13056B */
constexpr size_t kMaxRdncSize = kMaxBlockSize * kNbRdncBlks;   /* 417792B */
constexpr size_t kStageMemSize = kMaxDataSize + kMaxRdncSize;  /* <= 2MB */
constexpr size_t kHostHalfSize = kStageMemSize;
constexpr u32 kNbEcTaskVariants = kMaxDataSize / kMinDataSize;

struct Request {
    uint64_t ts;
    uint64_t size;
};

enum class DmaPhase : u32 { Read, Write };

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

    std::chrono::high_resolution_clock::time_point targetTime;
    std::chrono::high_resolution_clock::time_point reqBegin;
};

struct DmaTaskCtx {
    StageCtx *stage;
    DmaPhase phase;
};

struct StageBufs {
    std::vector<doca_buf *> ecDataBufs;
    std::vector<doca_buf *> ecRdncBufs;

    doca_buf *readSrcBuf;   /* host src -> DPU data */
    doca_buf *readDstBuf;
    doca_buf *writeSrcBuf;  /* DPU data+rdnc -> host dst */
    doca_buf *writeDstBuf;
};

struct StageTasks {
    doca_dma_task_memcpy *readTask;
    doca_dma_task_memcpy *writeTask;
    DmaTaskCtx readCtx;
    DmaTaskCtx writeCtx;
    std::vector<doca_ec_task_create *> ecTasks;
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
