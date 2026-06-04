#pragma once

#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <cstddef>
#include <cstdint>

constexpr size_t kDefaultMmapSize = 1024ULL * 1024 * 1024;

struct alignas(64) CdnClientRscs {
    doca_rdma *rdma;
    doca_ctx *ctx;
    doca_pe *pe;
    doca_dev *dev;
    doca_rdma_connection *conn;

    doca_mmap *mmap;
    doca_buf_inventory *bufInv;
    void *memAddr;

    uint32_t threadId;
    uint16_t port;
};

struct alignas(64) CdnClientCfg {
    char ibdevName[1024];
    uint32_t gidIdx;
    char serverIpAddr[1024];
    size_t mmapSize;
    uint16_t nbThreads;
};

doca_error_t init(const CdnClientCfg &aCfg, CdnClientRscs &aRscs);
void destroy(CdnClientRscs &aRscs);
