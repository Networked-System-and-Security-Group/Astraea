#include <arpa/inet.h>
#include <cstdint>
#include <unistd.h>
#include <vector>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_types.h>

#include <doca_mmap.h>

#include <doca_rdma.h>

#include "common.h"
#include "rdma.h"
#include "socket.h"

DOCA_LOG_REGISTER(COMMON : RDMA);

/* We only set read and write callbacks, extend this function when need */
doca_error_t initRdma(uint32_t aGidIdx, doca_dev *aDev, doca_pe *aPe,
                      doca_rdma_task_read_completion_cb_t aReadSuccCb,
                      doca_rdma_task_read_completion_cb_t aReadErrCb,
                      doca_rdma_task_write_completion_cb_t aWriteSuccCb,
                      doca_rdma_task_write_completion_cb_t aWriteErrCb,
                      doca_rdma *&aRdma, doca_ctx *&aCtx) {
    /* We want a device that support both READ and WRITE */
    doca_devinfo *devinfo = doca_dev_as_devinfo(aDev);
    CHECK_RETURN(doca_rdma_cap_task_read_is_supported(devinfo),
                 "support rdma read");
    CHECK_RETURN(doca_rdma_cap_task_write_is_supported(devinfo),
                 "support rdma write");

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

    aCtx = doca_rdma_as_ctx(aRdma);

    CHECK_RETURN(doca_pe_connect_ctx(aPe, aCtx), "connect pe to rdma ctx");

    CHECK_RETURN(doca_ctx_start(aCtx), "start rdma ctx");

    return DOCA_SUCCESS;
}

void destroyRdma(doca_rdma *aRdma, doca_ctx *aCtx) {
    CHECK_LOG(doca_ctx_stop(aCtx), "stop rdma ctx");
    CHECK_LOG(doca_rdma_destroy(aRdma), "destroy rdma");
}

doca_error_t rdmaConnect(doca_dev *aDev, const char *aIpAddr, uint16_t aPort,
                         doca_rdma *aRdma, doca_rdma_connection *&aConn,
                         doca_mmap *&remoteMmap) {
    const void *localConnDesc;
    size_t localConnDescSize;
    CHECK_RETURN(
        doca_rdma_export(aRdma, &localConnDesc, &localConnDescSize, &aConn),
        "export connection");

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
    sendMsg(sockfd, static_cast<const char *>(localConnDesc),
            localConnDescSize);
    /* 3 receive host connection info */
    char remoteConnDesc[1024] = {0};
    size_t remoteConnDescSize;
    remoteConnDescSize = recvMsg(sockfd, remoteConnDesc);

    /* 4. receive host mmap info */
    char remoteMmapDesc[1024] = {0};
    size_t remoteMmapDescSize;
    remoteMmapDescSize = recvMsg(sockfd, remoteMmapDesc);
    close(sockfd);

    CHECK_RETURN(doca_mmap_create_from_export(nullptr, remoteMmapDesc,
                                              remoteMmapDescSize, aDev,
                                              &remoteMmap),
                 "create remote mmap from export");

    CHECK_RETURN(
        doca_rdma_connect(aRdma, remoteConnDesc, remoteConnDescSize, aConn),
        "connect to remote");
    return DOCA_SUCCESS;
}