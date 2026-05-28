#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using u32 = uint32_t;

constexpr size_t kMaxMsgSize = 1024 * 1024 * 1024;

struct Request {
    uint64_t ts_ms;
    uint64_t size;
};

struct alignas(64) CdnClientRscs;
struct alignas(64) CdnClientCfg;

struct alignas(64) CdnClientUserData {
    CdnClientRscs &rscs;
    const CdnClientCfg &cfg;
    u32 stageId;
};

struct alignas(64) CdnClientRscs {
    doca_rdma *rdma;
    doca_ctx *ctx;
    doca_pe *pe;
    doca_dev *dev;
    doca_rdma_connection *conn;

    doca_mmap *mmap;
    doca_buf_inventory *bufInv;
    void *memAddr;
    std::vector<doca_buf *> bufs;

    std::vector<CdnClientUserData> userDatas;
    std::vector<doca_rdma_task_write_imm *> immTasks;
    std::vector<doca_rdma_task_receive *> recvTasks;
    std::vector<bool> canWrites;
    std::vector<bool> pendingWrites;

    /* Thread metadata, should init by main thread */
    u32 threadId;
    uint16_t port;

    u32 nbFreedTasks;
    u32 nbFinishedTasks;
    double nbProcessedGBits;

    std::vector<std::chrono::high_resolution_clock::time_point> beginTimes;
    std::vector<std::chrono::high_resolution_clock::time_point> endTimes;
    std::vector<double> timeCosts;

    std::vector<Request> requests;
    std::vector<u32> requestIds;
    std::vector<u32> recvIds;
};

struct alignas(64) CdnClientCfg {
    char ibdevName[1024];
    u32 gidIdx;
    char serverIpAddr[1024];
    size_t mmapSize;
    uint16_t nbThreads;
    u32 nbPipelineStages;
};

doca_error_t init(const CdnClientCfg &aCfg, CdnClientRscs &oCtx);
void runTasks(const CdnClientCfg &aCfg, CdnClientRscs &aRscs);
void destroy(CdnClientRscs &aCtx);