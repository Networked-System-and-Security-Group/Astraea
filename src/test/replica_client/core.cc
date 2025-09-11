#include <cstddef>
#include <cstring>
#include <doca_error.h>

#include <arpa/inet.h>
#include <doca_rdma.h>
#include <sys/socket.h>
#include <unistd.h>

#include <doca_dev.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <vector>

#include "common.h"
#include "memory.h"
#include "rdma.h"
#include "replica_client.h"
#include "socket.h"

DOCA_LOG_REGISTER(REPLICA:CLIENT : CORE);

doca_error_t init(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");
    /* Server only need pe to built connection */
    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    CHECK_RETURN(initMemory(8192, aRscs.dev, aCfg.mmapSize, aRscs.memAddr,
                            aRscs.mmap, aRscs.bufInv),
                 "init memory");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          nullptr, nullptr, aRscs.rdma, aRscs.ctx),
                 "init rdma");
    const void *localConnDesc;
    size_t localConnDescSize;
    CHECK_RETURN(doca_rdma_export(aRscs.rdma, &localConnDesc,
                                  &localConnDescSize, &aRscs.conn),
                 "export connection");

    /* Send connection info using socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{AF_INET, htons(aRscs.port), {INADDR_ANY}};
    int ret = bind(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (ret == -1) {
        DOCA_LOG_ERR("Failed to bind socket");
        close(sockfd);
        return DOCA_ERROR_IO_FAILED;
    }

    DOCA_LOG_INFO("Server %u listening, port is %u", aRscs.threadId,
                  aRscs.port);
    listen(sockfd, 1);

    int c = accept(sockfd, nullptr, nullptr);

    /* 1. receive remote connection info */
    char remoteConnDesc[1024];
    size_t remoteConnDescSize;
    remoteConnDescSize = recvMsg(c, remoteConnDesc);

    /* 2. send local connection info to remote */
    sendMsg(c, static_cast<const char *>(localConnDesc), localConnDescSize);

    /* 3. send local mmap info to remote */
    const void *localMmapDesc;
    size_t localMmapDescSize;
    CHECK_RETURN(doca_mmap_export_rdma(aRscs.mmap, aRscs.dev, &localMmapDesc,
                                       &localMmapDescSize),
                 "export mmap through rdma");
    sendMsg(c, static_cast<const char *>(localMmapDesc), localMmapDescSize);

    close(c);
    close(sockfd);

    CHECK_RETURN(doca_rdma_connect(aRscs.rdma, remoteConnDesc,
                                   remoteConnDescSize, aRscs.conn),
                 "connect to remote");
    DOCA_LOG_INFO("Thread %u connection established", aRscs.threadId);

    return DOCA_SUCCESS;
}

void destroy(ReplicaRscs &aRscs) {
    destroyRdma(aRscs.rdma, aRscs.ctx);
    destroyMemory(aRscs.memAddr, aRscs.mmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}