#pragma once

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using u32 = uint32_t;

constexpr u32 kNbDataBlks = 128;
constexpr u32 kNbRdncBlks = 32;
constexpr size_t kMinBlockSize = 64;
constexpr size_t kMaxBlockSize = 1024 * 1024;
constexpr size_t kMinDataSize = kMinBlockSize * kNbDataBlks;
constexpr size_t kMaxDataSize = kMaxBlockSize * kNbDataBlks;
constexpr size_t kMaxRdncSize = kMaxBlockSize * kNbRdncBlks;
constexpr size_t kStageMemSize = kMaxDataSize + kMaxRdncSize;

struct Request {
    uint64_t ts;
    uint64_t size;
};

enum class StageState : u32 {
    WaitingToSubmit,
    EcInFlight,
    WriteInFlight,
    Done,
};

struct alignas(64) CdnRscs;

struct StageCtx {
    CdnRscs *rscs;
    u32 stageId;
    StageState state;
    u32 reqIdx;
    size_t wireBytes;
    size_t ecBytes;
    size_t rdncBytes;
    std::chrono::high_resolution_clock::time_point reqBegin;
};

struct StageBufs {
    doca_buf *dataBuf;
    doca_buf *rdncBuf;
    doca_buf *clientBuf;
};

struct StageTasks {
    doca_ec_task_create *ecTask;
    doca_rdma_task_write *writeTask;
};

struct alignas(64) CdnRscs {
    doca_pe *pe;
    doca_dev *dev;

    doca_rdma *rdma;
    doca_ctx *rdmaCtx;
    doca_rdma_connection *clientConn;

    doca_ec *ec;
    doca_ctx *ecCtx;
    doca_ec_matrix *encMat;

    doca_buf_inventory *bufInv;
    doca_mmap *localMmap;
    void *localMemAddr;

    doca_mmap *clientMmap;

    std::vector<StageCtx> stageCtxs;
    std::vector<StageBufs> stageBufs;
    std::vector<StageTasks> stageTasks;

    std::vector<Request> requests;
    u32 nbRequests;
    uint64_t traceT0;

    std::vector<double> jcts;
    double nbWrittenGB;
    double nbEcGB;
    u32 nbCompletedReqs;

    u32 threadId;
    uint16_t port;
    std::chrono::high_resolution_clock::time_point wallStart;
};

struct alignas(64) CdnCfg {
    char ibdevName[1024];
    char tracePath[1024];
    u32 gidIdx;
    uint16_t nbThreads;
    u32 nbPipelineStages;
    u32 nbRequests;
    double traceTickUs;
    double replaySpeed;
};

doca_error_t init(const CdnCfg &aCfg, CdnRscs &aRscs);
void destroy(CdnRscs &aRscs);
void runTasks(const CdnCfg &aCfg, CdnRscs &aRscs);
std::vector<Request> loadTrace(const char *aPath);
