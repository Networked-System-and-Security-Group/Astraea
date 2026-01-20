#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>

#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "cdn_client.h"
#include "common.h"

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

std::chrono::high_resolution_clock::time_point gBeginTime, gEndTime;

bool gCanStart = false;
bool gForceQuit = false;

static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        gForceQuit = true;
        gEndTime = std::chrono::high_resolution_clock::now();
    }
}

doca_error_t worker(const CdnClientCfg &aCfg, CdnClientRscs &rscs) {
    CHECK_RETURN(init(aCfg, rscs), "init app");

    while (!gCanStart) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    runTasks(aCfg, rscs);

    destroy(rscs);
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    CdnClientCfg cfg = {.ibdevName = "mlx5_3",
                        .gidIdx = 1,
                        .serverIpAddr = "12.12.12.2",
                        .mmapSize = kMaxMsgSize,
                        .nbThreads = 1,
                        .nbPipelineStages = 4};

    CHECK_RETURN(doca_argp_init("cdn_client", &cfg), "init argp");

    CHECK_RETURN(registerParams(cfg), "register params");

    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");
    CHECK_RETURN(doca_argp_destroy(), "destroy argp");

    std::vector<CdnClientRscs> rscss;
    const uint16_t basePort = 22345;

    for (uint16_t i = 0; i < cfg.nbThreads; i++) {
        uint16_t portId = basePort + i;
        CdnClientRscs rscs = {.threadId = i,
                              .port = portId,
                              .nbFreedTasks = 0,
                              .nbFinishedTasks = 0,
                              .nbProcessedGBits = 0};

        rscss.push_back(rscs);
    }

    std::vector<std::jthread> threads;
    for (CdnClientRscs &rscs : rscss) {
        threads.emplace_back(worker, cfg, std::ref(rscs));
    }

    DOCA_LOG_INFO("Press Enter to send requests");
    int enter = 0;
    while (enter != '\r' && enter != '\n') enter = getchar();
    gCanStart = true;
    gBeginTime = std::chrono::high_resolution_clock::now();

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    DOCA_LOG_INFO("Press Ctrl-C to stop");

    for (std::jthread &thread : threads) {
        thread.join();
    }

    gEndTime = std::chrono::high_resolution_clock::now();

    double nbProcessedGBits = 0;
    for (auto &rscs : rscss) {
        nbProcessedGBits += rscs.nbProcessedGBits;
    }

    double timeCost = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          gEndTime - gBeginTime)
                          .count() /
                      1e9;
    DOCA_LOG_INFO("Finished %.2fGb in %.2fs, throughtput is %.2fGbps",
                  nbProcessedGBits, timeCost, nbProcessedGBits / timeCost);
    return 0;
}
