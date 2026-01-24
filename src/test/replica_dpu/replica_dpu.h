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
constexpr size_t kMaxDataSize = 128 * 1024 * 1024;
constexpr size_t kMinDataSize = 64 * 128;
constexpr u32 kNbDataBlks = 128;
constexpr u32 kNbRdncBlks = 32;

struct alignas(64) ReplicaRscs;
struct alignas(64) ReplicaCfg;
struct alignas(64) ReplicaUserData {
    ReplicaRscs &rscs;
    const ReplicaCfg &cfg;
    uint32_t stageId;
};

struct Request {
    uint64_t ts_ms;
    uint64_t size;
};

struct alignas(64) ReplicaRscs {
    doca_pe *pe;
    doca_dev *dev;

    /* RDMA resources */
    doca_rdma *rdma;
    doca_ctx *rdmaCtx;
    doca_rdma_connection *hostConn, *clientConn;

    /* EC resources */
    doca_ec *ec;
    doca_ctx *ecCtx;
    doca_ec_matrix *mat;

    /* Memory resources */
    doca_buf_inventory *bufInv;
    /* Local memory */
    doca_mmap *localMmap;
    void *localMemAddr;
    std::vector<doca_buf *> dataBufs;
    std::vector<doca_buf *> rdncBufs;
    std::vector<doca_buf *> sendBufs;

    /* Client memory */
    doca_mmap *clientMmap;
    std::vector<doca_buf *> clientBufs;

    /* Task resources */
    /* Read Tasks */
    std::vector<ReplicaUserData> userDatas;
    std::vector<doca_rdma_task_read *> readTasks;
    std::vector<doca_rdma_task_write *> writeTasks;
    std::vector<doca_ec_task_create *> ecTasks;

    /* Thread metadata, should init by main thread */
    uint32_t threadId;
    uint32_t nbFinishedTasks;
    uint32_t nbFreedTasks;
    uint16_t port;
    std::vector<std::chrono::high_resolution_clock::time_point> beginTimes;
    std::vector<std::chrono::high_resolution_clock::time_point> endTimes;
    std::vector<double> timeCosts;

    std::vector<Request> requests;
    std::vector<u32> requestIds;
    double nbProcessedGBits;
};

struct alignas(64) ReplicaCfg {
    char ibdevName[1024];
    uint32_t gidIdx;
    uint16_t nbThreads;
    uint32_t nbPipelineStages;
    size_t mmapSize;
};

doca_error_t init(const ReplicaCfg &aCfg, ReplicaRscs &oCtx);
void destroy(ReplicaRscs &aCtx);
void runTasks(const ReplicaCfg &aCfg, ReplicaRscs &aCtx);