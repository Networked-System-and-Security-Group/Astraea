#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "common.h"
#include "memscan_dpu.h"

DOCA_LOG_REGISTER(MEMSCAN:DPU : MAIN);

bool gForceQuit = false;

static void writeStatsFile(const char *path, double tput,
                           const std::vector<double> &jcts) {
    if (path == nullptr || path[0] == '\0') return;

    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        f << "{'tput': " << tput << ", 'jcts': [";
        for (size_t i = 0; i < jcts.size(); ++i) {
            if (i > 0) f << ", ";
            f << jcts[i];
        }
        f << "]}" << std::endl;
    } else {
        std::cerr << "Cannot open output file: " << path << std::endl;
    }
}

static void printStats(const std::vector<double> &jcts, double processedGB,
                       double wallSeconds) {
    double gbps = wallSeconds > 0.0 ? (processedGB * 8.0) / wallSeconds : 0.0;
    writeStatsFile(getenv("RES_PATH"), gbps, jcts);

    if (jcts.empty()) {
        std::cout << "No bursts completed." << std::endl;
        return;
    }

    std::vector<double> sorted = jcts;
    std::sort(sorted.begin(), sorted.end());

    double sum = 0.0;
    for (double v : sorted) sum += v;
    double avg = sum / sorted.size();

    size_t p95idx = static_cast<size_t>((sorted.size() - 1) * 0.95);
    size_t p99idx = static_cast<size_t>((sorted.size() - 1) * 0.99);
    double burstRate =
        wallSeconds > 0.0 ? static_cast<double>(jcts.size()) / wallSeconds
                          : 0.0;

    std::cout << "Bursts:   " << sorted.size() << std::endl;
    std::cout << "Wall:     " << wallSeconds << " s" << std::endl;
    std::cout << "Data:     " << processedGB << " GB" << std::endl;
    std::cout << "Tput:     " << gbps << " Gbps" << std::endl;
    std::cout << "Rate:     " << burstRate << " burst/s" << std::endl;
    std::cout << "Avg JCT:  " << avg << " us" << std::endl;
    std::cout << "p95 JCT:  " << sorted[p95idx] << " us" << std::endl;
    std::cout << "p99 JCT:  " << sorted[p99idx] << " us" << std::endl;
    std::cout << "Min JCT:  " << sorted.front() << " us" << std::endl;
    std::cout << "Max JCT:  " << sorted.back() << " us" << std::endl;
}

static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        gForceQuit = true;
    }
}

static doca_error_t ibdevNameCb(void *aVal, void *aCfg) {
    size_t len = strlen(static_cast<char *>(aVal));
    memcpy(static_cast<MemscanCfg *>(aCfg)->ibdevName, aVal, len);
    return DOCA_SUCCESS;
}

static doca_error_t portCb(void *aVal, void *aCfg) {
    static_cast<MemscanCfg *>(aCfg)->port = *static_cast<uint16_t *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t tScanMsCb(void *aVal, void *aCfg) {
    static_cast<MemscanCfg *>(aCfg)->tScanMs = *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t nPagesCb(void *aVal, void *aCfg) {
    static_cast<MemscanCfg *>(aCfg)->nPages = *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t regionSizeKbCb(void *aVal, void *aCfg) {
    static_cast<MemscanCfg *>(aCfg)->regionSizeKb = *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t nbBurstsCb(void *aVal, void *aCfg) {
    static_cast<MemscanCfg *>(aCfg)->nbBursts = *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(MemscanCfg &aCfg) {
    CHECK_RETURN(
        registerOneParam("d", "ibdev name", DOCA_ARGP_TYPE_STRING, ibdevNameCb),
        "register ibdev name");
    CHECK_RETURN(
        registerOneParam("p", "port to listen for host mmap descriptor",
                         DOCA_ARGP_TYPE_INT, portCb),
        "register port");
    CHECK_RETURN(registerOneParam("t", "burst period in ms", DOCA_ARGP_TYPE_INT,
                                  tScanMsCb),
                 "register tScanMs");
    CHECK_RETURN(registerOneParam("n", "DMA pages per burst",
                                  DOCA_ARGP_TYPE_INT, nPagesCb),
                 "register nPages");
    CHECK_RETURN(registerOneParam("r", "scan region size in KB",
                                  DOCA_ARGP_TYPE_INT, regionSizeKbCb),
                 "register regionSizeKb");
    CHECK_RETURN(registerOneParam("b", "number of bursts (0 = unlimited)",
                                  DOCA_ARGP_TYPE_INT, nbBurstsCb),
                 "register nbBursts");
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    MemscanCfg cfg = {
        .ibdevName = "mlx5_0",
        .tScanMs = 1,
        .nPages = 64,
        .regionSizeKb = 2048,
        .nbBursts = 0,
        .port = 22445,
    };

    CHECK_RETURN(doca_argp_init("memscan_dpu", &cfg), "init argp");
    CHECK_RETURN(registerParams(cfg), "register params");
    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");
    CHECK_RETURN(doca_argp_destroy(), "destroy argp");

    DOCA_LOG_INFO(
        "Config: tScanMs=%u nPages=%u regionSizeKb=%u nbBursts=%u port=%u",
        cfg.tScanMs, cfg.nPages, cfg.regionSizeKb, cfg.nbBursts, cfg.port);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    MemscanRscs rscs = {.nbCompletedInBurst = 0, .port = cfg.port};

    if (init(cfg, rscs) != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to initialise memscan DPU resources");
        return EXIT_FAILURE;
    }

    DOCA_LOG_INFO("Press Enter to run Memscan DPU workload");
    int enter = 0;
    while (enter != '\r' && enter != '\n') enter = getchar();

    auto wallBegin = std::chrono::high_resolution_clock::now();
    runTasks(cfg, rscs);
    auto wallEnd = std::chrono::high_resolution_clock::now();
    double wallSec = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         wallEnd - wallBegin)
                         .count() /
                     1e9;

    double processedGB =
        static_cast<double>(rscs.burstJcts.size()) * cfg.nPages *
        cfg.regionSizeKb * 1024.0 / (1024.0 * 1024.0 * 1024.0);
    printStats(rscs.burstJcts, processedGB, wallSec);

    destroy(rscs);
    return EXIT_SUCCESS;
}
