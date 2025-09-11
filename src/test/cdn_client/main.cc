#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>
#include <thread>
#include <vector>

#include "cdn_client.h"
#include "common.h"
#include "doca_types.h"
#include "rdma.h"

DOCA_LOG_REGISTER(CDN:CLIENT : MAIN);

doca_error_t ibdevNameCb(void *aIbdevName, void *aCfg) {
    size_t nameLen = strlen(static_cast<char *>(aIbdevName));
    memcpy(static_cast<CdnClientCfg *>(aCfg)->ibdevName, aIbdevName, nameLen);
    return DOCA_SUCCESS;
}

doca_error_t nbThreadsCb(void *aNbThreads, void *aCfg) {
    static_cast<CdnClientCfg *>(aCfg)->nbThreads =
        *static_cast<uint16_t *>(aNbThreads);
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(CdnClientCfg &cfg) {
    CHECK_RETURN(
        registerOneParam("d", "ibdev name", DOCA_ARGP_TYPE_STRING, ibdevNameCb),
        "register ibdev name cb");

    CHECK_RETURN(registerOneParam("nt", "number of threads", DOCA_ARGP_TYPE_INT,
                                  nbThreadsCb),
                 "register threads number cb");
    return DOCA_SUCCESS;
}

bool gForceQuit = false;
static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        gForceQuit = true;
    }
}

doca_error_t worker(const CdnClientCfg &aCfg, CdnClientRscs &rscs) {
    CHECK_RETURN(init(aCfg, rscs), "init app");

    while (!gForceQuit) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    destroy(rscs);
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    CdnClientCfg cfg = {.ibdevName = "mlx5_3",
                        .gidIdx = 1,
                        .mmapSize = kSendSize * kTaskPoolSize,
                        .nbThreads = 3};

    CHECK_RETURN(doca_argp_init("cdn_client", &cfg), "init argp");

    CHECK_RETURN(registerParams(cfg), "register params");

    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");
    CHECK_RETURN(doca_argp_destroy(), "destroy argp");

    std::vector<CdnClientRscs> rscss;
    const uint16_t basePort = 22345;

    for (uint16_t i = 0; i < cfg.nbThreads; i++) {
        uint16_t portId = basePort + i;
        CdnClientRscs rscs = {.threadId = i, .port = portId};
        rscss.push_back(rscs);
    }

    std::vector<std::jthread> threads;
    for (CdnClientRscs &rscs : rscss) {
        threads.emplace_back(worker, cfg, std::ref(rscs));
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    DOCA_LOG_INFO("Press Ctrl-C to stop");

    return 0;
}