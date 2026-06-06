#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "common.h"
#include "localec_dpu.h"

DOCA_LOG_REGISTER(LOCALEC:DPU : MAIN);

bool gForceQuit = false;

static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        gForceQuit = true;
    }
}

static void printStats(const std::vector<double> &jcts, double processedGB,
                       double wallSeconds) {
    if (jcts.empty()) {
        std::cout << "No requests completed." << std::endl;
        return;
    }

    std::vector<double> sorted = jcts;
    std::sort(sorted.begin(), sorted.end());

    double sum = 0.0;
    for (double v : sorted) sum += v;
    double avg = sum / sorted.size();

    size_t p95idx = static_cast<size_t>((sorted.size() - 1) * 0.95);
    size_t p99idx = static_cast<size_t>((sorted.size() - 1) * 0.99);

    double gbps = (processedGB * 8.0) / wallSeconds;
    double rps = static_cast<double>(jcts.size()) / wallSeconds;

    std::cout << "Requests: " << sorted.size() << std::endl;
    std::cout << "Wall:     " << wallSeconds << " s" << std::endl;
    std::cout << "Data:     " << processedGB << " GB" << std::endl;
    std::cout << "Tput:     " << gbps << " Gbps" << std::endl;
    std::cout << "Rate:     " << rps << " req/s" << std::endl;
    std::cout << "Avg JCT:  " << avg << " us" << std::endl;
    std::cout << "p95 JCT:  " << sorted[p95idx] << " us" << std::endl;
    std::cout << "p99 JCT:  " << sorted[p99idx] << " us" << std::endl;
    std::cout << "Min JCT:  " << sorted.front() << " us" << std::endl;
    std::cout << "Max JCT:  " << sorted.back() << " us" << std::endl;

    const char *path = getenv("RES_PATH");
    if (path && path[0] != '\0') {
        std::ofstream f(path, std::ios::out | std::ios::trunc);
        if (f.is_open()) {
            f << "[";
            for (size_t i = 0; i < jcts.size(); ++i) {
                if (i > 0) f << ", ";
                f << jcts[i];
            }
            f << "]" << std::endl;
        } else {
            std::cerr << "Cannot open output file: " << path << std::endl;
        }
    }
}

static doca_error_t ibdevNameCb(void *aVal, void *aCfg) {
    size_t len = strlen(static_cast<char *>(aVal));
    memcpy(static_cast<LocalEcCfg *>(aCfg)->ibdevName, aVal, len);
    return DOCA_SUCCESS;
}

static doca_error_t tracePathCb(void *aVal, void *aCfg) {
    size_t len = strlen(static_cast<char *>(aVal));
    memcpy(static_cast<LocalEcCfg *>(aCfg)->tracePath, aVal, len);
    return DOCA_SUCCESS;
}

static doca_error_t portCb(void *aVal, void *aCfg) {
    static_cast<LocalEcCfg *>(aCfg)->port = *static_cast<uint16_t *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t nbStagesCb(void *aVal, void *aCfg) {
    static_cast<LocalEcCfg *>(aCfg)->nbStages = *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t nbRequestsCb(void *aVal, void *aCfg) {
    static_cast<LocalEcCfg *>(aCfg)->nbRequests = *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t traceTickUsCb(void *aVal, void *aCfg) {
    static_cast<LocalEcCfg *>(aCfg)->traceTickUs = *static_cast<double *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t replaySpeedCb(void *aVal, void *aCfg) {
    static_cast<LocalEcCfg *>(aCfg)->replaySpeed = *static_cast<double *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(LocalEcCfg &aCfg) {
    CHECK_RETURN(
        registerOneParam("d", "ibdev name", DOCA_ARGP_TYPE_STRING, ibdevNameCb),
        "register ibdev name");
    CHECK_RETURN(registerOneParam("f", "FIU trace file path",
                                  DOCA_ARGP_TYPE_STRING, tracePathCb),
                 "register trace path");
    CHECK_RETURN(registerOneParam("p", "port for host mmap descriptor",
                                  DOCA_ARGP_TYPE_INT, portCb),
                 "register port");
    CHECK_RETURN(registerOneParam("s", "concurrent pipeline stages",
                                  DOCA_ARGP_TYPE_INT, nbStagesCb),
                 "register stages");
    CHECK_RETURN(registerOneParam("n", "request count cap (0 = all)",
                                  DOCA_ARGP_TYPE_INT, nbRequestsCb),
                 "register nbRequests");
    CHECK_RETURN(registerOneParam("t", "microseconds per trace tick",
                                  DOCA_ARGP_TYPE_DOUBLE, traceTickUsCb),
                 "register trace tick");
    CHECK_RETURN(registerOneParam("r", "replay speed multiplier",
                                  DOCA_ARGP_TYPE_DOUBLE, replaySpeedCb),
                 "register replay speed");
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    LocalEcCfg cfg = {
        .ibdevName = "mlx5_0",
        .tracePath = "data/fiu.txt",
        .nbStages = 4,
        .nbRequests = 0,
        .traceTickUs = 1.0,
        .replaySpeed = 1.0,
        .port = 22545,
    };

    CHECK_RETURN(doca_argp_init("localec_dpu", &cfg), "init argp");
    CHECK_RETURN(registerParams(cfg), "register params");
    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");
    CHECK_RETURN(doca_argp_destroy(), "destroy argp");

    DOCA_LOG_INFO(
        "Config: trace=%s nbStages=%u nbRequests=%u tickUs=%.3f replay=%.3f "
        "port=%u",
        cfg.tracePath, cfg.nbStages, cfg.nbRequests, cfg.traceTickUs,
        cfg.replaySpeed, cfg.port);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    LocalEcRscs rscs{};
    rscs.port = cfg.port;
    rscs.nbProcessedGB = 0.0;
    rscs.nbCompletedReqs = 0;

    if (init(cfg, rscs) != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to initialise localec DPU resources");
        return EXIT_FAILURE;
    }

    auto wallBegin = std::chrono::high_resolution_clock::now();
    runTasks(cfg, rscs);
    auto wallEnd = std::chrono::high_resolution_clock::now();
    double wallSec = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         wallEnd - wallBegin)
                         .count() /
                     1e9;

    printStats(rscs.jcts, rscs.nbProcessedGB, wallSec);

    destroy(rscs);
    return EXIT_SUCCESS;
}
