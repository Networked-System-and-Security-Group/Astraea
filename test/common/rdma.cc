#include "rdma.h"

#include <arpa/inet.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>
#include <doca_types.h>
#include <unistd.h>

#include <cstdint>

#include "common.h"
#include "socket.h"

DOCA_LOG_REGISTER(COMMON : RDMA);

/* We only set read and write callbacks, extend this function when need */
doca_error_t initRdma(uint32_t aGidIdx, doca_dev *aDev, doca_pe *aPe,
                      doca_rdma_task_read_completion_cb_t aReadSuccCb,
                      doca_rdma_task_read_completion_cb_t aReadErrCb,
                      doca_rdma_task_write_completion_cb_t aWriteSuccCb,
                      doca_rdma_task_write_completion_cb_t aWriteErrCb,
                      doca_rdma_task_receive_completion_cb_t aRecvSuccCb,
                      doca_rdma_task_receive_completion_cb_t aRecvErrCb,
                      doca_rdma_task_write_imm_completion_cb_t aImmSuccCb,
                      doca_rdma_task_write_imm_completion_cb_t aImmErrCb,
                      doca_rdma *&aRdma, doca_ctx *&aCtx) {
    CHECK_RETURN(doca_rdma_create(aDev, &aRdma), "create rdma");
    /* Set all RDMA permission for ease */
    CHECK_RETURN(
        doca_rdma_set_permissions(aRdma, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE |
                                             DOCA_ACCESS_FLAG_RDMA_READ |
                                             DOCA_ACCESS_FLAG_RDMA_WRITE),
        "set rdma permission");
    CHECK_RETURN(doca_rdma_set_max_num_connections(aRdma, 8),
                 "set max number of connections");

    CHECK_RETURN(doca_rdma_set_gid_index(aRdma, aGidIdx), "set gid index");

    if (aReadSuccCb && aReadErrCb) {
        CHECK_RETURN(doca_rdma_task_read_set_conf(
                         aRdma, aReadSuccCb, aReadErrCb, MAX_NB_RDMA_TASKS),
                     "set read callback");
    }

    if (aWriteSuccCb && aWriteErrCb) {
        CHECK_RETURN(doca_rdma_task_write_set_conf(
                         aRdma, aWriteSuccCb, aWriteErrCb, MAX_NB_RDMA_TASKS),
                     "set write task conf");
    }

    if (aRecvSuccCb && aRecvErrCb) {
        CHECK_RETURN(doca_rdma_task_receive_set_conf(
                         aRdma, aRecvSuccCb, aRecvErrCb, MAX_NB_RDMA_TASKS),
                     "set recv conf");
    }

    if (aImmSuccCb && aImmErrCb) {
        CHECK_RETURN(doca_rdma_task_write_imm_set_conf(
                         aRdma, aImmSuccCb, aImmErrCb, MAX_NB_RDMA_TASKS),
                     "set write imm task cb");
    }

    aCtx = doca_rdma_as_ctx(aRdma);

    CHECK_RETURN(doca_pe_connect_ctx(aPe, aCtx), "connect pe to rdma ctx");

    CHECK_RETURN(doca_ctx_start(aCtx), "start rdma ctx");

    return DOCA_SUCCESS;
}

void destroyRdma(doca_rdma *aRdma, doca_ctx *aCtx) {
    CHECK_LOG(doca_ctx_stop(aCtx), "stop rdma ctx");
    CHECK_LOG(doca_rdma_destroy(aRdma), "destroy rdma");
}

doca_error_t rdmaConnectToServer(doca_dev *aDev, const char *aIpAddr,
                                 uint16_t aPort, doca_rdma *aRdma,
                                 doca_mmap *aMmap, uint16_t aThreadId,
                                 doca_rdma_connection *&aConn) {
    /* Send connection info use socket */
    /* 1. build connection */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{AF_INET, htons(aPort)};
    inet_pton(AF_INET, aIpAddr, &addr.sin_addr);
    int ret =
        connect(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (ret == -1) {
        DOCA_LOG_ERR("Failed to connect socket");
        close(sockfd);
        return DOCA_ERROR_IO_FAILED;
    }

    /* 2. Send local connection info */
    const void *localConnDesc;
    size_t localConnDescSize;
    CHECK_RETURN(
        doca_rdma_export(aRdma, &localConnDesc, &localConnDescSize, &aConn),
        "export connection");

    sendMsg(sockfd, static_cast<const char *>(localConnDesc),
            localConnDescSize);

    /* 3 receive host connection info */
    char remoteConnDesc[1024] = {0};
    size_t remoteConnDescSize;
    remoteConnDescSize = recvMsg(sockfd, remoteConnDesc);

    CHECK_RETURN(
        doca_rdma_connect(aRdma, remoteConnDesc, remoteConnDescSize, aConn),
        "connect to remote");

    /* 3. send local mmap info to remote */
    const void *localMmapDesc;
    size_t localMmapDescSize;
    CHECK_RETURN(
        doca_mmap_export_rdma(aMmap, aDev, &localMmapDesc, &localMmapDescSize),
        "export mmap through rdma");
    sendMsg(sockfd, static_cast<const char *>(localMmapDesc),
            localMmapDescSize);
    close(sockfd);

    DOCA_LOG_INFO("Thread %u connection established", aThreadId);
    return DOCA_SUCCESS;
}

doca_error_t rdmaConnectToClient(doca_dev *aDev, uint16_t aPort,
                                 doca_rdma *aRdma, uint16_t aThreadId,
                                 doca_rdma_connection *&aConn,
                                 doca_mmap *&remoteMmap) {
    const void *localConnDesc;
    size_t localConnDescSize;
    CHECK_RETURN(
        doca_rdma_export(aRdma, &localConnDesc, &localConnDescSize, &aConn),
        "export connection");

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{AF_INET, htons(aPort), {INADDR_ANY}};
    int ret = bind(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (ret == -1) {
        DOCA_LOG_ERR("Failed to bind socket");
        close(sockfd);
        return DOCA_ERROR_IO_FAILED;
    }

    DOCA_LOG_INFO("Server %u listening, port is %u", aThreadId, aPort);
    listen(sockfd, 1);

    int c = accept(sockfd, nullptr, nullptr);

    /* 1. receive remote connection info */
    char remoteConnDesc[1024];
    size_t remoteConnDescSize;
    remoteConnDescSize = recvMsg(c, remoteConnDesc);
    CHECK_RETURN(
        doca_rdma_connect(aRdma, remoteConnDesc, remoteConnDescSize, aConn),
        "connect to remote");

    /* 2. send local connection info to remote */
    sendMsg(c, static_cast<const char *>(localConnDesc), localConnDescSize);

    /* 4. receive host mmap info */
    char remoteMmapDesc[1024] = {0};
    size_t remoteMmapDescSize;
    remoteMmapDescSize = recvMsg(c, remoteMmapDesc);  // 使用 c 而不是 sockfd
    close(c);
    close(sockfd);

    CHECK_RETURN(
        doca_mmap_create_from_export(nullptr, remoteMmapDesc,
                                     remoteMmapDescSize, aDev, &remoteMmap),
        "create remote mmap from export");
    DOCA_LOG_INFO("Thread %u connection established", aThreadId);

    return DOCA_SUCCESS;
}