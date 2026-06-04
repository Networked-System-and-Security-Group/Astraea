#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>

#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "cdn_client.h"
#include "common.h"

DOCA_LOG_REGISTER(CDN:CLIENT : MAIN);

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

    CHECK_RETURN(registerOneParam("t", "number of threads", DOCA_ARGP_TYPE_INT,
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
                        .nbThreads = 3,
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

    std::vector<std::vector<double>> allCosts;
    for (const CdnClientRscs &rscs : rscss) {
        allCosts.push_back(rscs.timeCosts);
    }

    processAndWriteData(allCosts, getenv("RES_PATH"));

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
