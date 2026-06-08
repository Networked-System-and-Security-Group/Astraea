#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "cdn_dpu.h"
#include "common.h"

DOCA_LOG_REGISTER(CDN:DPU : MAIN);

bool gCanStart = false;
bool gForceQuit = false;

static void copyString(char *dst, size_t dstSize, const char *src) {
    size_t len = strnlen(src, dstSize - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        gForceQuit = true;
    }
}

static double percentile(const std::vector<double> &sorted, double p) {
    if (sorted.empty()) return 0.0;
    size_t idx = static_cast<size_t>((sorted.size() - 1) * p);
    return sorted[idx];
}

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

static void printStats(const std::vector<CdnRscs> &rscss, double wallSeconds) {
    std::vector<double> jcts;
    double writtenGB = 0.0;
    double ecGB = 0.0;
    u32 completed = 0;

    for (const CdnRscs &rscs : rscss) {
        jcts.insert(jcts.end(), rscs.jcts.begin(), rscs.jcts.end());
        writtenGB += rscs.nbWrittenGB;
        ecGB += rscs.nbEcGB;
        completed += rscs.nbCompletedReqs;
    }

    double gbps = wallSeconds > 0.0 ? (writtenGB * 8.0) / wallSeconds : 0.0;
    writeStatsFile(getenv("RES_PATH"), gbps, jcts);

    if (jcts.empty()) {
        std::cout << "No requests completed." << std::endl;
        return;
    }

    std::vector<double> sorted = jcts;
    std::sort(sorted.begin(), sorted.end());

    double sum = 0.0;
    for (double v : sorted) sum += v;
    double mean = sum / sorted.size();
    double median = percentile(sorted, 0.50);
    double p90 = percentile(sorted, 0.90);
    double p99 = percentile(sorted, 0.99);
    double rps = wallSeconds > 0.0 ? static_cast<double>(completed) /
                                         wallSeconds
                                   : 0.0;

    std::cout << "Requests:   " << completed << std::endl;
    std::cout << "Wall:       " << wallSeconds << " s" << std::endl;
    std::cout << "Write Data: " << writtenGB << " GB" << std::endl;
    std::cout << "EC Data:    " << ecGB << " GB" << std::endl;
    std::cout << "Tput:       " << gbps << " Gbps" << std::endl;
    std::cout << "Rate:       " << rps << " req/s" << std::endl;
    std::cout << "Mean JCT:   " << mean << " us" << std::endl;
    std::cout << "Median JCT: " << median << " us" << std::endl;
    std::cout << "p90 JCT:    " << p90 << " us" << std::endl;
    std::cout << "p99 JCT:    " << p99 << " us" << std::endl;
    std::cout << "Min JCT:    " << sorted.front() << " us" << std::endl;
    std::cout << "Max JCT:    " << sorted.back() << " us" << std::endl;

}

static doca_error_t ibdevNameCb(void *aVal, void *aCfg) {
    copyString(static_cast<CdnCfg *>(aCfg)->ibdevName,
               sizeof(static_cast<CdnCfg *>(aCfg)->ibdevName),
               static_cast<char *>(aVal));
    return DOCA_SUCCESS;
}

static doca_error_t tracePathCb(void *aVal, void *aCfg) {
    copyString(static_cast<CdnCfg *>(aCfg)->tracePath,
               sizeof(static_cast<CdnCfg *>(aCfg)->tracePath),
               static_cast<char *>(aVal));
    return DOCA_SUCCESS;
}

static doca_error_t nbThreadsCb(void *aVal, void *aCfg) {
    static_cast<CdnCfg *>(aCfg)->nbThreads = *static_cast<uint16_t *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t nbPipelineStagesCb(void *aVal, void *aCfg) {
    static_cast<CdnCfg *>(aCfg)->nbPipelineStages =
        *static_cast<uint32_t *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t nbRequestsCb(void *aVal, void *aCfg) {
    static_cast<CdnCfg *>(aCfg)->nbRequests = *static_cast<uint32_t *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t traceTickUsCb(void *aVal, void *aCfg) {
    static_cast<CdnCfg *>(aCfg)->traceTickUs = *static_cast<double *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t replaySpeedCb(void *aVal, void *aCfg) {
    static_cast<CdnCfg *>(aCfg)->replaySpeed = *static_cast<double *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(CdnCfg &aCfg) {
    CHECK_RETURN(
        registerOneParam("d", "ibdev name", DOCA_ARGP_TYPE_STRING, ibdevNameCb),
        "register ibdev name");
    CHECK_RETURN(registerOneParam("f", "trace file path", DOCA_ARGP_TYPE_STRING,
                                  tracePathCb),
                 "register trace path");
    CHECK_RETURN(registerOneParam("t", "number of threads", DOCA_ARGP_TYPE_INT,
                                  nbThreadsCb),
                 "register threads number");
    CHECK_RETURN(registerOneParam("p", "number of pipeline stages",
                                  DOCA_ARGP_TYPE_INT, nbPipelineStagesCb),
                 "register pipeline stages");
    CHECK_RETURN(registerOneParam("r", "request count cap (0 = all)",
                                  DOCA_ARGP_TYPE_INT, nbRequestsCb),
                 "register requests number");
    CHECK_RETURN(registerOneParam("u", "microseconds per trace tick",
                                  DOCA_ARGP_TYPE_DOUBLE, traceTickUsCb),
                 "register trace tick");
    CHECK_RETURN(registerOneParam("s", "replay speed multiplier",
                                  DOCA_ARGP_TYPE_DOUBLE, replaySpeedCb),
                 "register replay speed");
    return DOCA_SUCCESS;
}

static doca_error_t worker(const CdnCfg &aCfg, CdnRscs &rscs) {
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

    CdnCfg cfg = {.ibdevName = "mlx5_3",
                  .tracePath = "data/meta.txt",
                  .gidIdx = 1,
                  .nbThreads = 3,
                  .nbPipelineStages = 4,
                  .nbRequests = 0,
                  .traceTickUs = 10.0,
                  .replaySpeed = 1.0};

    CHECK_RETURN(doca_argp_init("cdn_dpu", &cfg), "init argp");
    CHECK_RETURN(registerParams(cfg), "register params");
    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");
    CHECK_RETURN(doca_argp_destroy(), "destroy argp");

    if (cfg.nbThreads == 0 || cfg.nbPipelineStages == 0 ||
        cfg.replaySpeed <= 0.0) {
        DOCA_LOG_ERR(
            "Invalid config: nbThreads=%u nbPipelineStages=%u replaySpeed=%.3f",
            cfg.nbThreads, cfg.nbPipelineStages, cfg.replaySpeed);
        return EXIT_FAILURE;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    const uint16_t basePort = 22345;
    std::vector<CdnRscs> rscss;
    rscss.reserve(cfg.nbThreads);
    for (uint16_t i = 0; i < cfg.nbThreads; i++) {
        rscss.push_back({.nbWrittenGB = 0.0,
                         .nbEcGB = 0.0,
                         .nbCompletedReqs = 0,
                         .threadId = i,
                         .port = static_cast<uint16_t>(basePort + i)});
    }

    std::vector<std::thread> threads;
    threads.reserve(cfg.nbThreads);
    for (CdnRscs &rscs : rscss) {
        threads.emplace_back(worker, cfg, std::ref(rscs));
    }

    DOCA_LOG_INFO("Press Enter to run CDN DPU workload");
    int enter = 0;
    while (enter != '\r' && enter != '\n') enter = getchar();
    gCanStart = true;
    auto wallBegin = std::chrono::high_resolution_clock::now();

    for (std::thread &thread : threads) {
        thread.join();
    }

    auto wallEnd = std::chrono::high_resolution_clock::now();
    double wallSec = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         wallEnd - wallBegin)
                         .count() /
                     1e9;

    printStats(rscss, wallSec);
    return 0;
}
