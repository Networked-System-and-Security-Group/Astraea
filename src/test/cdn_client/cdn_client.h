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

constexpr uint32_t kTaskPoolSize = 32;
constexpr size_t kBlkSize = 65536;
constexpr uint32_t kNbDataBlks = 128;
constexpr uint32_t kNbRdncBlks = 32;
constexpr size_t kDataSize = kBlkSize * kNbDataBlks;
constexpr size_t kRdncSize = kBlkSize * kNbRdncBlks;
constexpr size_t kSendSize = kDataSize + kRdncSize;

struct Request {
    uint64_t ts_ms;
    uint64_t size;
};

struct CdnClientRscs {
    doca_rdma *rdma;
    doca_ctx *ctx;
    doca_pe *pe;
    doca_dev *dev;
    doca_rdma_connection *conn;

    doca_mmap *mmap;
    doca_buf_inventory *bufInv;
    void *memAddr;

    doca_rdma_task_write_imm *writeTask;
    bool isFreeded = false;

    /* Thread metadata, should init by main thread */
    uint32_t threadId;
    uint16_t port;

    std::chrono::high_resolution_clock::time_point beginTime;
    std::chrono::high_resolution_clock::time_point endTime;

    std::vector<Request> requests;
};

struct CdnClientCfg {
    char ibdevName[1024];
    uint32_t gidIdx;
    size_t mmapSize;
    uint16_t nbThreads;
};

doca_error_t init(const CdnClientCfg &aCfg, CdnClientRscs &oCtx);
void destroy(CdnClientRscs &aCtx);