#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include <cstring>

#include "common.h"
#include "memory.h"
#include "rdma.h"
#include "replica_client.h"

DOCA_LOG_REGISTER(REPLICA:CLIENT : CORE);

doca_error_t init(const ReplicaCfg &aCfg, ReplicaRscs &aRscs) {
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");
    /* Server only need pe to built connection */
    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    CHECK_RETURN(initMemory(8192, aRscs.dev, aCfg.mmapSize, aRscs.memAddr,
                            aRscs.mmap, aRscs.bufInv),
                 "init memory");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                          aRscs.rdma, aRscs.ctx),
                 "init rdma");

    CHECK_RETURN(
        rdmaConnectToServer(aRscs.dev, aCfg.serverIpAddr, aRscs.port,
                            aRscs.rdma, aRscs.mmap, aRscs.threadId, aRscs.conn),
        "connect to server");

    return DOCA_SUCCESS;
}

void destroy(ReplicaRscs &aRscs) {
    destroyRdma(aRscs.rdma, aRscs.ctx);
    destroyMemory(aRscs.memAddr, aRscs.mmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}