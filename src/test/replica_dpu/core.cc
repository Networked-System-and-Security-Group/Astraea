#include <cstddef>
#include <cstdint>
#include <cstring>
#include <doca_error.h>

#include <doca_rdma.h>

#include "common.h"
#include "doca_ctx.h"
#include "doca_erasure_coding.h"
#include "ec.h"
#include "memory.h"
#include "rdma.h"
#include "replica_dpu.h"
#include <chrono>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <thread>
#include <fstream>
#include <sstream>
#include <string>

DOCA_LOG_REGISTER(REPLICA:DPU : CORE);

extern bool gForceQuit;

// Helper to parse CSV
static void loadTrace(const char* path, std::vector<TraceRecord>& traces) {
    std::ifstream file(path);
    if (!file.is_open()) {
        DOCA_LOG_ERR("Failed to open trace file: %s", path);
        return;
    }

    std::string line;
    // Optional: skip header if exists
    // std::getline(file, line); 

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string val;
        TraceRecord rec;
        
        // CSV: device_id,opcode,offset,length,timestamp
        std::getline(ss, val, ','); rec.device_id = std::stoul(val);
        std::getline(ss, val, ','); rec.opcode = val[0]; 
        std::getline(ss, val, ','); rec.offset = std::stoull(val);
        std::getline(ss, val, ','); rec.length = std::stoul(val);
        std::getline(ss, val, ','); rec.timestamp = std::stoull(val);

        traces.push_back(rec);
    }
    DOCA_LOG_INFO("Loaded %lu trace records", traces.size());
}

static void handleTaskCompletion(ReplicaUserData *userData, bool success) {
    ReplicaRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;

    std::unique_lock<std::mutex> lock(rscs.taskMutex);
    
    if (!gForceQuit && success) {
        rscs.freeTaskIds.push(taskId);
        rscs.taskCv.notify_all();
    } else {
        lock.unlock(); // 提前解锁

        if (!success && !gForceQuit) {
            gForceQuit = true;
            DOCA_LOG_ERR("Task failed, initiating shutdown...");
        }

        doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[taskId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        rscs.nbFreedTasks++;
        
        // 唤醒主线程以便其退出等待
        rscs.taskCv.notify_all();
    }
}

void ecSuccCb(doca_ec_task_create *task, doca_data task_user_data,
              doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    
    if (!gForceQuit) {
        doca_error_t res = doca_task_submit(
            doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        if (res != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Submit write task failed: %s", doca_error_get_name(res));
            handleTaskCompletion(userData, false);
        }
    } else {
        handleTaskCompletion(userData, false);
    }
}

void ecErrCb(doca_ec_task_create *task, doca_data task_user_data,
             doca_data ctx_user_data) {
    ReplicaUserData *userData = static_cast<ReplicaUserData *>(task_user_data.ptr);
    DOCA_LOG_ERR("EC Task Error Callback triggered");
    handleTaskCompletion(userData, false);
}

void WriteSuccCb(doca_rdma_task_write *task, doca_data task_user_data,
                 doca_data ctx_user_data) {
    ReplicaUserData *userData = static_cast<ReplicaUserData *>(task_user_data.ptr);
    
    userData->rscs.endTimes.push_back(std::chrono::high_resolution_clock::now());
    userData->rscs.nbFinishedTasks++;

    CHECK_LOG(doca_buf_set_data_len(userData->rscs.rdncBufs[userData->taskId], 0), "reset rdnc len");
    CHECK_LOG(doca_buf_set_data_len(userData->rscs.clientBufs[userData->taskId], 0), "reset client len");

    handleTaskCompletion(userData, true);
}

void WriteErrCb(doca_rdma_task_write *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    ReplicaUserData *userData = static_cast<ReplicaUserData *>(task_user_data.ptr);
    DOCA_LOG_ERR("Write Task Error Callback triggered");
    handleTaskCompletion(userData, false);
}

static doca_error_t initBufs(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    char *clientMemAddr;
    size_t clientMemAddrSize;
    CHECK_RETURN(
        doca_mmap_get_memrange(aRscs.clientMmap,
                               reinterpret_cast<void **>(&clientMemAddr),
                               &clientMemAddrSize),
        "get client memory addr");
    aRscs.clientBaseAddr = clientMemAddr;

    // Use Max K and Max M for buffer allocation
    size_t dataSize = aCfg.blkSize * aRscs.maxNbDataBlks;
    size_t rdncSize = aCfg.blkSize * aRscs.maxNbRdncBlks;
    size_t totalSize = dataSize + rdncSize;

    char *localMemAddrInChar = static_cast<char *>(aRscs.localMemAddr);
    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        doca_buf *recvBuf, *rdncBuf, *sendBuf, *clientBuf;

        CHECK_RETURN(doca_buf_inventory_buf_get_by_data(
                         aRscs.bufInv, aRscs.localMmap,
                         localMemAddrInChar + i * totalSize, dataSize,
                         &recvBuf),
                     "get recv buf by addr");
        aRscs.recvBufs.push_back(recvBuf);

        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.localMmap,
                         localMemAddrInChar + i * totalSize + dataSize,
                         rdncSize, &rdncBuf),
                     "get rdnc buf by addr");
        aRscs.rdncBufs.push_back(rdncBuf);

        CHECK_RETURN(doca_buf_inventory_buf_get_by_data(
                         aRscs.bufInv, aRscs.localMmap,
                         localMemAddrInChar + i * totalSize, totalSize,
                         &sendBuf),
                     "get send buf by data");
        aRscs.sendBufs.push_back(sendBuf);

        CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                         aRscs.bufInv, aRscs.clientMmap,
                         clientMemAddr, clientMemAddrSize, &clientBuf), 
                     "get client buf by addr");
        aRscs.clientBufs.push_back(clientBuf);
    }
    return DOCA_SUCCESS;
}

static void destroyBufs(ReplicaRscs &aRscs) {
    for (doca_buf *&clientBuf : aRscs.clientBufs) {
        CHECK_LOG(doca_buf_dec_refcount(clientBuf, nullptr), "dec client buf ref cnt");
    }
    for (doca_buf *&recvBuf : aRscs.recvBufs) {
        CHECK_LOG(doca_buf_dec_refcount(recvBuf, nullptr), "dec recv buf ref cnt");
    }
    for (doca_buf *&rdncBuf : aRscs.rdncBufs) {
        CHECK_LOG(doca_buf_dec_refcount(rdncBuf, nullptr), "dec rdnc buf ref cnt");
    }
    for (doca_buf *&sendBuf : aRscs.sendBufs) {
        CHECK_LOG(doca_buf_dec_refcount(sendBuf, nullptr), "dec send buf ref cnt");
    }
}

static doca_error_t initTasks(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        ReplicaUserData userData = {.rscs = aRscs, .taskId = i};
        aRscs.userDatas.push_back(userData);
        aRscs.freeTaskIds.push(i); 
    }

    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        doca_ec_task_create *ecTask;
        // 使用默认/最大矩阵初始化
        CHECK_RETURN(doca_ec_task_create_allocate_init(
                         aRscs.ec, aRscs.mat, aRscs.recvBufs[i],
                         aRscs.rdncBufs[i], {.ptr = &aRscs.userDatas[i]},
                         &ecTask),
                     "alloc and init ec task");
        aRscs.ecTasks.push_back(ecTask);

        doca_rdma_task_write *writeTask;
        CHECK_RETURN(doca_rdma_task_write_allocate_init(
                         aRscs.rdma, aRscs.clientConn, aRscs.sendBufs[i],
                         aRscs.clientBufs[i], {.ptr = &aRscs.userDatas[i]},
                         &writeTask),
                     "alloc and init write task");
        aRscs.writeTasks.push_back(writeTask);
    }
    return DOCA_SUCCESS;
}

doca_error_t init(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");
    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    // 1. Load Trace first to calculate Max K and Max M
    if (strlen(aCfg.tracePath) > 0) {
        loadTrace(aCfg.tracePath, aRscs.traces);
    }
    
    // 2. Scan trace for max length
    uint32_t maxK = 1;
    for (const auto& rec : aRscs.traces) {
        uint32_t k = rec.length / aCfg.blkSize;
        if (k > maxK) maxK = k;
    }
    
    // [Hardware Limits]
    // K <= 128
    // M <= 32
    constexpr uint32_t kHardMaxK = 128;
    constexpr uint32_t kHardMaxM = 32;

    if (maxK > kHardMaxK) {
        DOCA_LOG_WARN("Trace MaxK=%u exceeds hardware limit %u. Clamping K to %u.", 
                      maxK, kHardMaxK, kHardMaxK);
        maxK = kHardMaxK;
    }

    // Default safe value
    if (maxK < 32) maxK = 32; 
    // Double check clamp
    if (maxK > kHardMaxK) maxK = kHardMaxK;

    aRscs.maxNbDataBlks = maxK;
    
    // Calculate M based on K (Ratio 2:1)
    uint32_t maxM = (maxK + 1) / 2;
    
    // [Fix] Clamp M to hardware limit
    if (maxM > kHardMaxM) {
        DOCA_LOG_WARN("Calculated MaxM=%u exceeds hardware limit %u. Clamping M to %u.", 
                      maxM, kHardMaxM, kHardMaxM);
        maxM = kHardMaxM;
    }

    aRscs.maxNbRdncBlks = maxM;
    aRscs.totalBytesProcessed = 0;

    DOCA_LOG_INFO("Configured MaxK=%u, MaxM=%u based on trace.", 
                  aRscs.maxNbDataBlks, aRscs.maxNbRdncBlks);

    // 3. Alloc Memory with Max sizes
    size_t mmapSize = aCfg.blkSize * (aRscs.maxNbDataBlks + aRscs.maxNbRdncBlks) * aCfg.nbTasks;
    CHECK_RETURN(initMemory(8192, aRscs.dev, mmapSize, aRscs.localMemAddr,
                            aRscs.localMmap, aRscs.bufInv),
                 "init memory");

    doca_ec_matrix *dummyMat = nullptr;
    // Init EC with max dimensions to set up context
    CHECK_RETURN(initEc(aRscs.dev, aRscs.pe, aRscs.maxNbDataBlks, aRscs.maxNbRdncBlks, nullptr,
                        0, ecSuccCb, ecErrCb, nullptr, nullptr, aRscs.ec,
                        aRscs.mat, dummyMat, aRscs.ecCtx),
                 "init ec");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          WriteSuccCb, WriteErrCb, aRscs.rdma, aRscs.rdmaCtx),
                 "init rdma");

    CHECK_RETURN(rdmaConnect(aRscs.dev, aCfg.clientIpAddr, aRscs.port,
                             aRscs.rdma, aRscs.clientConn, aRscs.clientMmap),
                 "connect to client");

    DOCA_LOG_INFO("Thread %u connection established", aRscs.threadId);

    CHECK_RETURN(initBufs(aCfg, aRscs), "init bufs");
    CHECK_RETURN(initTasks(aCfg, aRscs), "init tasks");

    return DOCA_SUCCESS;
}

void runTasks(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    auto t0 = std::chrono::high_resolution_clock::now();
    
    uint64_t baseTimestamp = 0;
    if (!aRscs.traces.empty()) {
        baseTimestamp = aRscs.traces.front().timestamp;
    }

    std::jthread progressThread([&](){
        // Phase 1: Trace Progress
        while(!gForceQuit && (aRscs.nbFinishedTasks < aRscs.traces.size() || aRscs.traces.empty())) {
             doca_pe_progress(aRscs.pe);
        }
        
        // Phase 2: Cleanup tasks
        while(aRscs.nbFreedTasks < aCfg.nbTasks) {
             doca_pe_progress(aRscs.pe);
             if (aRscs.nbFreedTasks >= aCfg.nbTasks) break;
        }

        // Phase 3: Graceful Shutdown
        if (aRscs.ecCtx) doca_ctx_stop(aRscs.ecCtx);
        doca_ctx_states state;
        for(int i=0; i<5000; i++) {
             doca_pe_progress(aRscs.pe);
             doca_ctx_get_state(aRscs.ecCtx, &state);
             if (state == DOCA_CTX_STATE_IDLE) break;
             std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    for (const auto& rec : aRscs.traces) {
        if (gForceQuit) break;

        // 时间戳逻辑
        uint64_t relativeTimeUs = 0;
        if (rec.timestamp >= baseTimestamp) {
            relativeTimeUs = rec.timestamp - baseTimestamp;
        }
        auto targetTime = t0 + std::chrono::microseconds(relativeTimeUs);
        
        while (std::chrono::high_resolution_clock::now() < targetTime && !gForceQuit) {
             std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (gForceQuit) break;

        // Get free task
        uint32_t taskId;
        {
            std::unique_lock<std::mutex> lock(aRscs.taskMutex);
            aRscs.taskCv.wait(lock, [&] { return !aRscs.freeTaskIds.empty() || gForceQuit; });
            if(gForceQuit) break;
            taskId = aRscs.freeTaskIds.front();
            aRscs.freeTaskIds.pop();
        }

        // =================================================================
        // Dynamic K & M Calculation
        // K = length / blkSize
        // M = K / 2 (Ratio 2:1)
        // =================================================================
        uint32_t k = rec.length / aCfg.blkSize;
        if (k == 0) k = 1; 
        
        // [Fix] Enforce hardware limit dynamically
        if (k > aRscs.maxNbDataBlks) {
             k = aRscs.maxNbDataBlks;
        }

        uint32_t m = k / 2;
        if (m == 0) m = 1; 
        
        // [Fix] Enforce M limit
        if (m > aRscs.maxNbRdncBlks) {
             m = aRscs.maxNbRdncBlks;
        }

        // Matrix Selection
        doca_ec_matrix *targetMat = nullptr;
        std::pair<uint32_t, uint32_t> matKey = {k, m};
        
        if (aRscs.matCache.find(matKey) != aRscs.matCache.end()) {
            targetMat = aRscs.matCache[matKey];
        } else {
            // Create new matrix
            doca_error_t res = doca_ec_matrix_create(aRscs.ec, DOCA_EC_MATRIX_TYPE_CAUCHY, 
                                                     k, m, &targetMat);
            if (res != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to create dynamic matrix K=%u M=%u: %s", 
                             k, m, doca_error_get_name(res));
                gForceQuit = true;
                break;
            }
            aRscs.matCache[matKey] = targetMat;
        }

        // Update Task with new Matrix
        doca_ec_task_create_set_coding_matrix(aRscs.ecTasks[taskId], targetMat);

        // Update Buffers
        size_t dataLen = k * aCfg.blkSize;
        CHECK_LOG(doca_buf_set_data_len(aRscs.recvBufs[taskId], dataLen), "set data len");
        
        size_t rdncLen = m * aCfg.blkSize;
        size_t totalLen = dataLen + rdncLen;
        
        // Setup Client Write Buffer
        void* clientBase = aRscs.clientBaseAddr;
        doca_buf_set_data(aRscs.clientBufs[taskId], static_cast<char*>(clientBase), totalLen);

        // Stats accumulation
        aRscs.totalBytesProcessed += dataLen;

        // Submit Task
        aRscs.beginTimes.push_back(std::chrono::high_resolution_clock::now());
        
        doca_error_t res = doca_task_submit(doca_ec_task_create_as_task(aRscs.ecTasks[taskId]));
        if (res != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Submit failed: %s", doca_error_get_name(res));
            {
                std::unique_lock<std::mutex> tLock(aRscs.taskMutex);
                aRscs.freeTaskIds.push(taskId);
            }
            gForceQuit = true;
            break;
        }
    }
    
    // Wait for all tasks
    while(!gForceQuit && aRscs.nbFinishedTasks < aRscs.traces.size()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    gForceQuit = true;

    // Cleanup
    {
        std::unique_lock<std::mutex> lock(aRscs.taskMutex);
        while (!aRscs.freeTaskIds.empty()) {
            uint32_t taskId = aRscs.freeTaskIds.front();
            aRscs.freeTaskIds.pop();

            doca_task_free(doca_ec_task_create_as_task(aRscs.ecTasks[taskId]));
            doca_task_free(doca_rdma_task_write_as_task(aRscs.writeTasks[taskId]));
            
            aRscs.nbFreedTasks++;
        }
    }

    for (u32 i = 0; i < aRscs.nbFinishedTasks; i++) {
        double timeCost = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              aRscs.endTimes[i] - aRscs.beginTimes[i])
                              .count() /
                          1000.0;
        aRscs.timeCosts.push_back(timeCost);
    }
}

void destroy(ReplicaRscs &aRscs) {
    // Destroy cached matrices
    for (auto const& [key, mat] : aRscs.matCache) {
        if (mat && mat != aRscs.mat) {
             doca_ec_matrix_destroy(mat);
        }
    }
    
    destroyEc(aRscs.mat, nullptr, aRscs.ec, aRscs.ecCtx);
    destroyRdma(aRscs.rdma, aRscs.rdmaCtx);
    destroyBufs(aRscs);
    CHECK_LOG(doca_mmap_destroy(aRscs.clientMmap), "destroy client mmap");
    destroyMemory(aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}