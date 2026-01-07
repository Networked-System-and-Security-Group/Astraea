#include <chrono>
#include <cstddef>
#include <cstdint>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include <doca_erasure_coding.h>
#include <doca_rdma.h>

constexpr uint32_t kNbDataBlks = 128;
constexpr uint32_t kNbRdncBlks = 32;

// Trace record structure
struct TraceRecord {
    uint32_t device_id;
    uint32_t opcode;
    uint64_t offset;
    uint32_t length;
    uint64_t timestamp;
};

struct alignas(64) ReplicaRscs;
struct alignas(64) ReplicaUserData {
    ReplicaRscs &rscs;
    uint32_t taskId;
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
    std::vector<doca_buf *> recvBufs;
    std::vector<doca_buf *> rdncBufs;
    std::vector<doca_buf *> sendBufs;

    /* Host memory */
    doca_mmap *hostMmap;
    std::vector<doca_buf *> hostBufs;

    /* Client memory */
    doca_mmap *clientMmap;
    std::vector<doca_buf *> clientBufs;
    // Base address of remote memory to calculate offsets
    char* clientBaseAddr; 

    /* Task resources */
    /* Read Tasks */
    std::vector<ReplicaUserData> userDatas;
    std::vector<doca_rdma_task_read *> readTasks;
    std::vector<doca_rdma_task_write *> writeTasks;
    std::vector<doca_ec_task_create *> ecTasks;

    /* Trace Data */
    std::vector<TraceRecord> traces;

    /* Concurrency Control for Trace Replay */
    std::queue<uint32_t> freeTaskIds;
    std::mutex taskMutex;
    std::condition_variable taskCv;

    /* Thread metadata, should init by main thread */
    uint32_t threadId;
    uint32_t nbFinishedTasks;
    uint32_t nbFreedTasks;
    uint16_t port;
    std::vector<std::chrono::high_resolution_clock::time_point> beginTimes;
    std::vector<std::chrono::high_resolution_clock::time_point> endTimes;
    std::vector<double> timeCosts;
};

struct alignas(64) ReplicaCfg {
    char ibdevName[1024];
    uint32_t gidIdx;
    char hostIpAddr[1024];
    char clientIpAddr[1024];
    uint16_t nbThreads;
    uint32_t nbTasks;
    size_t blkSize;
    char tracePath[1024]; // Path to CSV file
};

doca_error_t init(const ReplicaCfg &aCfg, ReplicaRscs &oCtx);
void destroy(ReplicaRscs &aCtx);
void runTasks(const ReplicaCfg &aCfg, ReplicaRscs &aCtx);