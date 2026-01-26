#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using u32 = uint32_t;

constexpr u32 kNbDataBlks = 128;
constexpr u32 kNbRdncBlks = 32;
constexpr size_t kMinChunkSize = 128 * 64;
constexpr size_t kMaxChunkSize = 128 * 1024 * 1024;
// Max mmap size is 1GB, 512MB for data buf and 512MB for rdnc buf
constexpr u32 kMaxNbChunks = 4;
constexpr u32 kMaxDataSize = kMaxChunkSize * kMaxNbChunks;

struct alignas(64) CdnRscs;
struct alignas(64) CdnCfg;

struct alignas(64) CdnUserData {
    CdnRscs &rscs;
    const CdnCfg &cfg;
    u32 stageId;
    size_t requestSize;
    u32 chunkId;
    u32 nbChunks;
};

struct TaskPack {
    std::vector<CdnUserData> userDatas;

    // Bufs
    std::vector<doca_buf *> dataBufs;
    std::vector<doca_buf *> rdncBufs;

    doca_buf *sendBuf;
    doca_buf *clientBuf;

    // Tasks
    doca_rdma_task_receive *recvTask;
    doca_rdma_task_write_imm *immTask;
    std::vector<doca_ec_task_recover *> ecTasks;
};

struct alignas(64) CdnRscs {
    doca_pe *pe;
    doca_dev *dev;

    /* RDMA resources */
    doca_rdma *rdma;
    doca_ctx *rdmaCtx;
    doca_rdma_connection *clientConn;

    /* EC resources */
    doca_ec *ec;
    doca_ctx *ecCtx;
    doca_ec_matrix *encMat, *decMat;

    /* Memory resources */
    doca_buf_inventory *bufInv;
    /* Local memory */
    doca_mmap *localMmap;
    void *localMemAddr;
    // std::vector<doca_buf *> recvBufs;
    // std::vector<doca_buf *> sendBufs;

    /* Client memory */
    doca_mmap *clientMmap;
    // std::vector<doca_buf *> clientBufs;

    /* Task resources */
    std::vector<TaskPack> packs;
    std::vector<bool> pendingWrites;

    /* Thread metadata, should init by main thread */
    u32 threadId;
    // u32 nbFinishedTasks;
    u32 nbFreedTasks;
    uint16_t port;

    std::vector<u32> recvIds;
};

struct alignas(64) CdnCfg {
    char ibdevName[1024];
    u32 gidIdx;
    uint16_t nbThreads;
    u32 nbPipelineStages;
    u32 nbRequests;
};

doca_error_t init(const CdnCfg &aCfg, CdnRscs &oCtx);
void destroy(CdnRscs &aCtx);
void runTasks(const CdnCfg &aCfg, CdnRscs &aCtx);