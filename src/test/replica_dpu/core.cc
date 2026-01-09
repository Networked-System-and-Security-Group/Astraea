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

static void handleTaskCompletion(ReplicaUserData *userData, bool success) {
    ReplicaRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;

    // 必须在锁内检查 gForceQuit，防止主线程刚清理完，这里又把任务放回去
    std::unique_lock<std::mutex> lock(rscs.taskMutex);
    
    if (!gForceQuit && success) {
        // 正常运行且任务成功：放回队列供下次使用
        rscs.freeTaskIds.push(taskId);
        rscs.taskCv.notify_all(); // 唤醒正在等待任务的主线程
    } else {
        // 正在退出 或 任务失败：直接释放资源
        // 此时持有锁，确保主线程不会同时操作该 taskId (虽然主线程只操作队列)
        // 但更重要的是，如果 gForceQuit 为 true，我们绝不把任务放回队列
        
        lock.unlock(); // 释放资源不需要持有锁，提前解锁减少竞争

        if (!success) {
            gForceQuit = true; // 发生错误强制退出
            DOCA_LOG_ERR("Task failed, aborting...");
        }

        doca_task_free(doca_ec_task_create_as_task(rscs.ecTasks[taskId]));
        doca_task_free(doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        rscs.nbFreedTasks++;
        
        // [至关重要] 唤醒主线程！否则主线程可能卡在 taskCv.wait() 永远无法退出
        rscs.taskCv.notify_all();
    }
}

void ecSuccCb(doca_ec_task_create *task, doca_data task_user_data,
              doca_data ctx_user_data) {
    ReplicaUserData *userData =
        static_cast<ReplicaUserData *>(task_user_data.ptr);
    ReplicaRscs &rscs = userData->rscs;
    uint32_t taskId = userData->taskId;
    
    // 注意：EC 完成后通常需要提交 Write 任务。
    // 这里如果还没退出，继续提交；如果退出了，直接释放。
    // 由于提交 Write 任务不是“完成整个流程”，我们需要特殊处理。
    
    if (!gForceQuit) {
        doca_error_t res = doca_task_submit(
            doca_rdma_task_write_as_task(rscs.writeTasks[taskId]));
        if (res != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Submit write task failed: %s", doca_error_get_name(res));
            handleTaskCompletion(userData, false); // 提交失败当做错误处理
        }
    } else {
        // 正在退出，不再提交下一阶段，直接销毁
        handleTaskCompletion(userData, false); // false 会触发释放逻辑
    }
}

void ecErrCb(doca_ec_task_create *task, doca_data task_user_data,
             doca_data ctx_user_data) {
    ReplicaUserData *userData = static_cast<ReplicaUserData *>(task_user_data.ptr);
    handleTaskCompletion(userData, false);
}

void WriteSuccCb(doca_rdma_task_write *task, doca_data task_user_data,
                 doca_data ctx_user_data) {
    ReplicaUserData *userData = static_cast<ReplicaUserData *>(task_user_data.ptr);
    
    userData->rscs.endTimes.push_back(std::chrono::high_resolution_clock::now());
    userData->rscs.nbFinishedTasks++; // 统计完成数

    CHECK_LOG(doca_buf_set_data_len(userData->rscs.rdncBufs[userData->taskId], 0), "reset rdnc len");
    CHECK_LOG(doca_buf_set_data_len(userData->rscs.clientBufs[userData->taskId], 0), "reset client len");

    handleTaskCompletion(userData, true);
}

void WriteErrCb(doca_rdma_task_write *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    ReplicaUserData *userData = static_cast<ReplicaUserData *>(task_user_data.ptr);
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

    std::jthread progressThread([&](){
        // 阶段 1: 处理 Trace
        while(!gForceQuit && (aRscs.nbFinishedTasks < aRscs.traces.size() || aRscs.traces.empty())) {
             doca_pe_progress(aRscs.pe);
        }
        // 阶段 2: 清理阶段，必须等待所有任务资源释放完毕
        while(aRscs.nbFreedTasks < aCfg.nbTasks) { // 移除 && gForceQuit，只要没释放完就一直转
             doca_pe_progress(aRscs.pe);
             if (aRscs.nbFreedTasks >= aCfg.nbTasks) break;
             // 可选：加个短暂 yield 防止空转占用 100% CPU
             // std::this_thread::yield(); 
        }
    });

    for (const auto& rec : aRscs.traces) {
        if (gForceQuit) break;

        // [修改] 优化等待逻辑：能够响应 gForceQuit
        auto targetTime = t0 + std::chrono::microseconds(rec.timestamp);
        while (std::chrono::high_resolution_clock::now() < targetTime && !gForceQuit) {
             std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (gForceQuit) break;

        // 2. Get free task
        uint32_t taskId;
        {
            std::unique_lock<std::mutex> lock(aRscs.taskMutex);
            // [关键] wait 会在收到 notify_all 时被唤醒，此时检查 gForceQuit 并退出
            aRscs.taskCv.wait(lock, [&] { return !aRscs.freeTaskIds.empty() || gForceQuit; });
            
            if(gForceQuit) break; // 退出循环
            
            taskId = aRscs.freeTaskIds.front();
            aRscs.freeTaskIds.pop();
        }

        // ... (任务配置代码保持不变，省略中间部分) ...
        size_t maxDataLen = aCfg.blkSize * kNbDataBlks;
        size_t len = std::min((size_t)rec.length, maxDataLen);
        CHECK_LOG(doca_buf_set_data_len(aRscs.recvBufs[taskId], len), "set buf len");
        size_t rdncLen = (len + kNbDataBlks - 1) / kNbDataBlks * kNbRdncBlks;
        void* clientBase = aRscs.clientBaseAddr;
        doca_buf_set_data(aRscs.clientBufs[taskId], static_cast<char*>(clientBase) + rec.offset, len + rdncLen);

        // 4. Submit Task
        aRscs.beginTimes.push_back(std::chrono::high_resolution_clock::now());
        CHECK_LOG(doca_task_submit(doca_ec_task_create_as_task(aRscs.ecTasks[taskId])), "submit trace ec task");
    }
    
    // 等待正在运行的任务自然结束（或者被 Ctrl-C 标记）
    while(!gForceQuit && aRscs.nbFinishedTasks < aRscs.traces.size()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    gForceQuit = true; 

    // 手动释放所有空闲任务
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
    // 此时 nbFreedTasks 应该等于 nbTasks，progressThread 将自动退出
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