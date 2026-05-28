#include "dma.h"

#include <arpa/inet.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <unistd.h>

#include <cstdint>

#include "common.h"
#include "socket.h"

DOCA_LOG_REGISTER(COMMON : DMA)

doca_error_t initDma(doca_dev *dev, doca_pe *pe,
                     doca_dma_task_memcpy_completion_cb_t succCB,
                     doca_dma_task_memcpy_completion_cb_t errCb, doca_dma *&dma,
                     doca_ctx *&ctx) {
    CHECK_RETURN(doca_dma_create(dev, &dma), "create dma");
    CHECK_RETURN(
        doca_dma_task_memcpy_set_conf(dma, succCB, errCb, MAX_NB_DMA_TASKS),
        "set memcpy task conf");
    ctx = doca_dma_as_ctx(dma);
    CHECK_RETURN(doca_pe_connect_ctx(pe, ctx), "connect pe to dma ctx");
    CHECK_RETURN(doca_ctx_start(ctx), "start dma ctx");
    return DOCA_SUCCESS;
}

void destroyDma(doca_dma *dma, doca_ctx *ctx) {
    CHECK_LOG(doca_ctx_stop(ctx), "stop dma ctx");
    CHECK_LOG(doca_dma_destroy(dma), "destroy dma");
}

doca_error_t dmaExportToServer(doca_dev *aDev, const char *aServerIp,
                                uint16_t aPort, doca_mmap *aMmap) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{AF_INET, htons(aPort)};
    inet_pton(AF_INET, aServerIp, &addr.sin_addr);
    if (connect(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
        DOCA_LOG_ERR("Failed to connect to DPU server %s:%u", aServerIp, aPort);
        close(sockfd);
        return DOCA_ERROR_IO_FAILED;
    }

    const void *desc;
    size_t descSize;
    CHECK_RETURN(doca_mmap_export_pci(aMmap, aDev, &desc, &descSize),
                 "export mmap for pci");
    sendMsg(sockfd, static_cast<const char *>(desc), descSize);
    close(sockfd);
    DOCA_LOG_INFO("Sent mmap descriptor (%zu bytes) to DPU", descSize);
    return DOCA_SUCCESS;
}

doca_error_t dmaImportFromClient(uint16_t aPort, doca_dev *aDev,
                                  doca_mmap *&oHostMmap) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{AF_INET, htons(aPort), {INADDR_ANY}};
    if (bind(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
        DOCA_LOG_ERR("Failed to bind socket on port %u", aPort);
        close(sockfd);
        return DOCA_ERROR_IO_FAILED;
    }
    listen(sockfd, 1);
    DOCA_LOG_INFO("DPU listening on port %u for host mmap descriptor", aPort);

    int c = accept(sockfd, nullptr, nullptr);
    char desc[4096] = {0};
    size_t descSize = recvMsg(c, desc);
    close(c);
    close(sockfd);

    CHECK_RETURN(doca_mmap_create_from_export(nullptr, desc, descSize, aDev,
                                              &oHostMmap),
                 "create host mmap from export");
    DOCA_LOG_INFO("Imported host mmap (%zu byte descriptor)", descSize);
    return DOCA_SUCCESS;
}
