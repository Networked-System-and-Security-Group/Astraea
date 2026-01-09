#include <cstddef>
#include <cstdint>
#include <cstring>
#include <doca_error.h>

#include <doca_rdma.h>

#include "common.h"
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
#include <signal.h>
#include <thread>
#include <fstream>
#include <sstream>
#include <string>
#include <iostream>

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

void ecSuccCb(doca_ec_task_create *task, doca_data task_user_data,
              doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    if (!gForceQuit) {
        CHECK_LOG(doca_task_submit(
                      doca_rdma_task_write_as_task(rscs.writeTasks[taskId])),
                  "submit write task in cb");
    } else {
        doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[taskId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        
        // Return to queue even on quit to avoid deadlocks in destroy if needed
        std::unique_lock<std::mutex> lock(rscs.taskMutex);
        rscs.freeTaskIds.push(taskId);
        rscs.taskCv.notify_one();
        
        rscs.nbFreedTasks++;
    }
}
void ecErrCb(doca_ec_task_create *task, doca_data task_user_data,
             doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    gForceQuit = true;
    doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[taskId]));
    doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("EC task failed");
}

void WriteSuccCb(doca_rdma_task_write *task, doca_data task_user_data,
                 doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    rscs.endTimes.push_back(std::chrono::high_resolution_clock::now());
    
    // Instead of resubmitting loop, we free the resource index
    {
        std::unique_lock<std::mutex> lock(rscs.taskMutex);
        rscs.freeTaskIds.push(taskId);
    }
    rscs.taskCv.notify_one();

    rscs.nbFinishedTasks++;
    
    CHECK_LOG(doca_buf_set_data_len(rscs.rdncBufs[taskId], 0), "reset rdnc len");
    CHECK_LOG(doca_buf_set_data_len(rscs.clientBufs[taskId], 0), "reset client len");
    
    if (gForceQuit) {
        doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[taskId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        rscs.nbFreedTasks++;
    }
}

void WriteErrCb(doca_rdma_task_write *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    gForceQuit = true;
    doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[taskId]));
    doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
    rscs.nbFreedTasks++;
    DOCA_LOG_ERR("Write task failed");
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

    // Use max possible size for buffer init to be safe, actual usage depends on trace
    size_t dataSize = aCfg.blkSize * kNbDataBlks;
    size_t rdncSize = aCfg.blkSize * kNbRdncBlks;
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
                         clientMemAddr, totalSize, &clientBuf), // Point to base initially
                     "get client buf by addr");
        aRscs.clientBufs.push_back(clientBuf);
    }
    return DOCA_SUCCESS;
}

static void destroyBufs(ReplicaRscs &aRscs) {
    for (doca_buf *&clientBuf : aRscs.clientBufs) {
        CHECK_LOG(doca_buf_dec_refcount(clientBuf, nullptr),
                  "dec client buf ref cnt");
    }

    for (doca_buf *&recvBuf : aRscs.recvBufs) {
        CHECK_LOG(doca_buf_dec_refcount(recvBuf, nullptr),
                  "dec recv buf ref cnt");
    }

    for (doca_buf *&rdncBuf : aRscs.rdncBufs) {
        CHECK_LOG(doca_buf_dec_refcount(rdncBuf, nullptr),
                  "dec rdnc buf ref cnt");
    }

    for (doca_buf *&sendBuf : aRscs.sendBufs) {
        CHECK_LOG(doca_buf_dec_refcount(sendBuf, nullptr),
                  "dec send buf ref cnt");
    }
}

static doca_error_t initTasks(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        ReplicaUserData userData = {.rscs = aRscs, .taskId = i};
        aRscs.userDatas.push_back(userData);
        aRscs.freeTaskIds.push(i); // Initial free tasks
    }

    for (uint32_t i = 0; i < aCfg.nbTasks; i++) {
        doca_ec_task_create *ecTask;
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

    size_t mmapSize = aCfg.blkSize * (kNbDataBlks + kNbRdncBlks) * aCfg.nbTasks;
    CHECK_RETURN(initMemory(8192, aRscs.dev, mmapSize, aRscs.localMemAddr,
                            aRscs.localMmap, aRscs.bufInv),
                 "init memory");

    doca_ec_matrix *dummyMat = nullptr;
    CHECK_RETURN(initEc(aRscs.dev, aRscs.pe, kNbDataBlks, kNbRdncBlks, nullptr,
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
    
    // Load Trace
    if (strlen(aCfg.tracePath) > 0) {
        loadTrace(aCfg.tracePath, aRscs.traces);
    }

    CHECK_RETURN(initBufs(aCfg, aRscs), "init bufs");
    CHECK_RETURN(initTasks(aCfg, aRscs), "init tasks");

    return DOCA_SUCCESS;
}

void runTasks(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // PE progress thread or interleaved in loop? 
    // Since we need to sleep for timestamps, it's better to run PE progress in a separate thread or non-blocking in loop.
    // For simplicity here, we assume a separate thread is handling PE or we call it when waiting.
    // But since doca_pe_progress is usually single threaded per PE, we must call it.
    // We will use a dedicated thread for PE progress in this design or interleave.
    
    // Let's spawn a helper for progress
    std::jthread progressThread([&](){
        while(!gForceQuit && (aRscs.nbFinishedTasks < aRscs.traces.size() || aRscs.traces.empty())) {
             doca_pe_progress(aRscs.pe);
             // Yield to avoid burning CPU if we are just waiting for time
             // std::this_thread::yield(); 
        }
        // Cleanup phase progress
        while(aRscs.nbFreedTasks < aCfg.nbTasks && gForceQuit) {
             doca_pe_progress(aRscs.pe);
        }
    });

    for (const auto& rec : aRscs.traces) {
        if (gForceQuit) break;

        // 1. Wait for timestamp
        auto targetTime = t0 + std::chrono::microseconds(rec.timestamp);
        std::this_thread::sleep_until(targetTime);

        // 2. Get free task
        uint32_t taskId;
        {
            std::unique_lock<std::mutex> lock(aRscs.taskMutex);
            aRscs.taskCv.wait(lock, [&] { return !aRscs.freeTaskIds.empty() || gForceQuit; });
            if(gForceQuit) break;
            taskId = aRscs.freeTaskIds.front();
            aRscs.freeTaskIds.pop();
        }

        // 3. Configure Task based on Trace
        // Ensure length doesn't exceed buffer limits
        size_t maxDataLen = aCfg.blkSize * kNbDataBlks;
        size_t len = std::min((size_t)rec.length, maxDataLen);
        
        // EC requires specific alignment usually, but we assume trace is valid or we process what we can.
        // Update local buffer data length (simulating reading 'len' bytes)
        CHECK_LOG(doca_buf_set_data_len(aRscs.recvBufs[taskId], len), "set buf len from trace");

        // Calculate expected redundancy size (simplified linear proportion)
        // Note: Real EC requires padding if len is not aligned to kNbDataBlks
        size_t rdncLen = (len + kNbDataBlks - 1) / kNbDataBlks * kNbRdncBlks;
        
        // Update Remote Buffer info to write to correct offset
        // We reuse the existing doca_buf object but point it to new address
        void* clientBase = aRscs.clientBaseAddr;
        doca_buf_set_data(aRscs.clientBufs[taskId], static_cast<char*>(clientBase) + rec.offset, len + rdncLen);

        // 4. Submit Task
        aRscs.beginTimes.push_back(std::chrono::high_resolution_clock::now());
        CHECK_LOG(doca_task_submit(doca_ec_task_create_as_task(aRscs.ecTasks[taskId])), "submit trace ec task");
    }
    
    // Wait for all tasks to drain if needed
    // The progress thread handles completion.
    // Just wait until finished count matches trace size
    while(!gForceQuit && aRscs.nbFinishedTasks < aRscs.traces.size()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    gForceQuit = true; // Signal progress thread to stop
}

void destroy(ReplicaRscs &aRscs) {
    destroyEc(aRscs.mat, nullptr, aRscs.ec, aRscs.ecCtx);
    destroyRdma(aRscs.rdma, aRscs.rdmaCtx);
    destroyBufs(aRscs);
    CHECK_LOG(doca_mmap_destroy(aRscs.clientMmap), "destroy client mmap");
    destroyMemory(aRscs.localMemAddr, aRscs.localMmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}