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

constexpr uint32_t kNbDataBlks = 128;
constexpr uint32_t kNbRdncBlks = 32;

struct alignas(64) CdnRscs;
struct alignas(64) CdnCfg;

struct alignas(64) CdnUserData {
    CdnRscs &rscs;
    const CdnCfg &cfg;
    uint32_t taskId;
    uint32_t chunk_id;
    uint32_t nb_chunks;
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
    std::vector<doca_buf *> recvBufs;
    std::vector<doca_buf *> sendBufs;

    /* Host memory */
    doca_mmap *hostMmap;
    std::vector<doca_buf *> hostBufs;

    /* Client memory */
    doca_mmap *clientMmap;
    std::vector<doca_buf *> clientBufs;

    /* Task resources */
    /* Read Tasks */
    std::vector<CdnUserData> userDatas;
    std::vector<doca_rdma_task_receive *> recvTasks;
    std::vector<doca_rdma_task_write *> writeTasks;
    std::vector<doca_ec_task_recover *> ecTasks;

    /* Thread metadata, should init by main thread */
    uint32_t threadId;
    uint32_t nbFinishedTasks;
    uint32_t nbFreedTasks;
    uint16_t port;
    std::vector<std::chrono::high_resolution_clock::time_point> beginTimes;
    std::vector<std::chrono::high_resolution_clock::time_point> endTimes;
    std::vector<double> timeCosts;
};

struct alignas(64) CdnCfg {
    char ibdevName[1024];
    uint32_t gidIdx;
    char hostIpAddr[1024];
    char clientIpAddr[1024];
    uint16_t nbThreads;
    uint32_t nbTasks;
    size_t blkSize;
};

doca_error_t init(const CdnCfg &aCfg, CdnRscs &oCtx);
void destroy(CdnRscs &aCtx);
void runTasks(const CdnCfg &aCfg, CdnRscs &aCtx);