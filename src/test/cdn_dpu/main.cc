#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
/* Not treating gForceQuit as atomic doesn't matter a lot */
bool gForceQuit = false;

using TimePoint = std::chrono::high_resolution_clock::time_point;

TimePoint beginTime, endTime;

static void processAndWriteData(const std::vector<std::vector<double>> &data,
                                const char *path) {
    // 合并所有 vector<double> 到一个 vector 中
    std::vector<double> merged;
    for (const auto &vec : data) {
        merged.insert(merged.end(), vec.begin(), vec.end());
    }

    // 排序：从小到大
    std::sort(merged.begin(), merged.end());

    // 计算平均值
    double sum = 0.0;
    for (double val : merged) {
        sum += val;
    }
    double average = merged.empty() ? 0.0 : sum / merged.size();

    // 计算 p95 和 p99（百分位数）
    double p95 = 0.0, p99 = 0.0;
    if (!merged.empty()) {
        // 注意：百分位索引从0开始，p95 是第 95% 位置的元素
        size_t p95_index = static_cast<size_t>((merged.size() - 1) * 0.95);
        size_t p99_index = static_cast<size_t>((merged.size() - 1) * 0.99);
        p95 = merged[p95_index];
        p99 = merged[p99_index];
    }

    // 输出到终端
    std::cout << "p99: " << p99 << std::endl;
    std::cout << "p95: " << p95 << std::endl;
    std::cout << "average: " << average << std::endl;

    // 如果 path 非空，写入文件
    if (path != nullptr && path[0] != '\0') {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << "[";
            for (size_t i = 0; i < merged.size(); ++i) {
                if (i > 0) file << ", ";
                file << merged[i];
            }
            file << "]" << std::endl;
            file.close();
        } else {
            std::cerr << "无法打开文件: " << path << std::endl;
        }
    }
}

static doca_error_t ibdevNameCb(void *aIbdevName, void *aCfg) {
    size_t nameLen = strlen(static_cast<char *>(aIbdevName));
    memcpy(static_cast<CdnCfg *>(aCfg)->ibdevName, aIbdevName, nameLen);
    return DOCA_SUCCESS;
}

static doca_error_t nbThreadsCb(void *aNbThreads, void *aCfg) {
    static_cast<CdnCfg *>(aCfg)->nbThreads =
        *static_cast<uint16_t *>(aNbThreads);
    return DOCA_SUCCESS;
}

static doca_error_t nbPipelineStagesCb(void *aNbPipelineStages, void *aCfg) {
    static_cast<CdnCfg *>(aCfg)->nbPipelineStages =
        *static_cast<uint16_t *>(aNbPipelineStages);
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(CdnCfg &cfg) {
    CHECK_RETURN(
        registerOneParam("d", "ibdev name", DOCA_ARGP_TYPE_STRING, ibdevNameCb),
        "register ibdev name cb");

    CHECK_RETURN(registerOneParam("nth", "number of threads",
                                  DOCA_ARGP_TYPE_INT, nbThreadsCb),
                 "register threads number cb");
    CHECK_RETURN(registerOneParam("ntk", "number of tasks", DOCA_ARGP_TYPE_INT,
                                  nbPipelineStagesCb),
                 "register task number cb");
    return DOCA_SUCCESS;
}

doca_error_t worker(const CdnCfg &aCfg, CdnRscs &rscs) {
    CHECK_RETURN(init(aCfg, rscs), "init app");

    while (!gCanStart) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    runTasks(aCfg, rscs);

    destroy(rscs);
    return DOCA_SUCCESS;
}

static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        gForceQuit = true;
        endTime = std::chrono::high_resolution_clock::now();
    }
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    CdnCfg cfg = {.ibdevName = "mlx5_3",
                  .gidIdx = 1,
                  .clientIpAddr = "12.12.12.1",
                  .nbThreads = 1,
                  .nbPipelineStages = 4};

    CHECK_RETURN(doca_argp_init("replica_dpu", &cfg), "init argp");

    CHECK_RETURN(registerParams(cfg), "register params");

    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");

    CHECK_RETURN(doca_argp_destroy(), "destroy argp");
    DOCA_LOG_INFO("Cur config: nbThreads is %u, nbPipelineStages is %u",
                  cfg.nbThreads, cfg.nbPipelineStages);

    const uint16_t basePort = 22345;
    std::vector<CdnRscs> rscss;
    for (uint16_t i = 0; i < cfg.nbThreads; i++) {
        uint16_t portId = basePort + i;
        CdnRscs rscs = {.threadId = i, .nbFreedTasks = 0, .port = portId};
        rscss.push_back(rscs);
    }

    std::vector<std::thread> threads;
    for (CdnRscs &rscs : rscss) {
        threads.emplace_back(worker, cfg, std::ref(rscs));
    }

    DOCA_LOG_INFO("Press Enter to serve requests");
    int enter = 0;
    while (enter != '\r' && enter != '\n') enter = getchar();
    gCanStart = true;
    beginTime = std::chrono::high_resolution_clock::now();

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    DOCA_LOG_INFO("Press Ctrl-C to stop");

    for (std::thread &thread : threads) {
        thread.join();
    }

    std::vector<std::vector<double>> allCosts;
    for (const CdnRscs &rscs : rscss) {
        allCosts.push_back(rscs.timeCosts);
    }

    processAndWriteData(allCosts, getenv("RES_PATH"));

    double nbProcessedGBits = 0;
    uint32_t nbOps = 0;
    for (const CdnRscs &rscs : rscss) {
        nbOps += rscs.sizes.size();
        for (const size_t &size : rscs.sizes) {
            nbProcessedGBits += static_cast<double>(size) * 8 / 1e9;
        }
    }

    const double timeCost =
        std::chrono::duration_cast<std::chrono::nanoseconds>(endTime -
                                                             beginTime)
            .count() /
        1e9;
    const double gbps = nbProcessedGBits / timeCost;
    const double ops = nbOps / timeCost;
    DOCA_LOG_INFO("Processed %.0fGb in %fs, throughput is %.2fGbps, ops is %f",
                  nbProcessedGBits, timeCost, gbps, ops);

    return 0;
}