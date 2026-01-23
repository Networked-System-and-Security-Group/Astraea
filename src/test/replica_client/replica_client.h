#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <cstddef>
#include <cstdint>

constexpr uint32_t kTaskPoolSize = 32;
constexpr size_t kBlkSize = 65536;
constexpr uint32_t kNbDataBlks = 128;
constexpr uint32_t kNbRdncBlks = 32;
constexpr size_t kDataSize = kBlkSize * kNbDataBlks;
constexpr size_t kRdncSize = kBlkSize * kNbRdncBlks;
constexpr size_t kSendSize = kDataSize + kRdncSize;

struct ReplicaRscs {
    doca_rdma *rdma;
    doca_ctx *ctx;
    doca_pe *pe;
    doca_dev *dev;
    doca_rdma_connection *conn;

    doca_mmap *mmap;
    doca_buf_inventory *bufInv;
    void *memAddr;

    /* Thread metadata, should init by main thread */
    uint32_t threadId;
    uint16_t port;
};

struct ReplicaCfg {
    char ibdevName[1024];
    uint32_t gidIdx;
    size_t mmapSize;
    uint16_t nbThreads;
    char serverIpAddr[1024];
};

doca_error_t init(const ReplicaCfg &aCfg, ReplicaRscs &oCtx);
void destroy(ReplicaRscs &aCtx);