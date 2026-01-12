#pragma once

#include <cstddef>
#include <cstdint>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>
#include <vector>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include <doca_rdma.h>

constexpr uint32_t kTaskPoolSize = 32;
constexpr size_t kBlkSize = 2048;
constexpr uint32_t kNbDataBlks = 32;
constexpr uint32_t kNbRdncBlks = 8;
constexpr size_t kDataSize = kBlkSize * kNbDataBlks;
constexpr size_t kRdncSize = kBlkSize * kNbRdncBlks;
constexpr size_t kSendSize = kDataSize + kRdncSize;

// 共享资源结构体：包含设备句柄和内存
struct SharedRscs {
    doca_dev *dev;          // 全局唯一的设备句柄
    doca_mmap *mmap;        // 全局唯一的注册内存
    void *memAddr;
    size_t memSize;
};

struct ReplicaRscs {
    doca_rdma *rdma;
    doca_ctx *ctx;
    doca_pe *pe;
    doca_rdma_connection *conn;
    
    // 指向共享资源的指针
    doca_dev *dev;
    doca_mmap *mmap;
    void *memAddr;
    
    // 线程独立的 Inventory
    doca_buf_inventory *bufInv;

    /* Thread metadata */
    uint32_t threadId;
    uint16_t port;
};

struct ReplicaCfg {
    char ibdevName[1024];
    uint32_t gidIdx;
    size_t mmapSize;
    uint16_t nbThreads;
};

// init 使用 SharedRscs 初始化
doca_error_t init(const ReplicaCfg &aCfg, const SharedRscs &shared, ReplicaRscs &oCtx);
void destroy(ReplicaRscs &aCtx);