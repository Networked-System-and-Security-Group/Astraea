#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "cdn_client.h"
#include "common.h"

DOCA_LOG_REGISTER(CDN:CLIENT : MAIN);

bool gForceQuit = false;

static void copyString(char *dst, size_t dstSize, const char *src) {
    size_t len = strnlen(src, dstSize - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static doca_error_t ibdevNameCb(void *aVal, void *aCfg) {
    copyString(static_cast<CdnClientCfg *>(aCfg)->ibdevName,
               sizeof(static_cast<CdnClientCfg *>(aCfg)->ibdevName),
               static_cast<char *>(aVal));
    return DOCA_SUCCESS;
}

static doca_error_t serverIpCb(void *aVal, void *aCfg) {
    copyString(static_cast<CdnClientCfg *>(aCfg)->serverIpAddr,
               sizeof(static_cast<CdnClientCfg *>(aCfg)->serverIpAddr),
               static_cast<char *>(aVal));
    return DOCA_SUCCESS;
}

static doca_error_t nbThreadsCb(void *aVal, void *aCfg) {
    static_cast<CdnClientCfg *>(aCfg)->nbThreads =
        *static_cast<uint16_t *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t mmapSizeMbCb(void *aVal, void *aCfg) {
    uint32_t mb = *static_cast<uint32_t *>(aVal);
    static_cast<CdnClientCfg *>(aCfg)->mmapSize =
        static_cast<size_t>(mb) * 1024 * 1024;
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(CdnClientCfg &aCfg) {
    CHECK_RETURN(
        registerOneParam("d", "ibdev name", DOCA_ARGP_TYPE_STRING,
                         ibdevNameCb),
        "register ibdev name");
    CHECK_RETURN(
        registerOneParam("t", "number of threads", DOCA_ARGP_TYPE_INT,
                         nbThreadsCb),
        "register thread count");
    CHECK_RETURN(
        registerOneParam("i", "DPU server IP address", DOCA_ARGP_TYPE_STRING,
                         serverIpCb),
        "register server ip");
    CHECK_RETURN(
        registerOneParam("m", "exported mmap size in MB", DOCA_ARGP_TYPE_INT,
                         mmapSizeMbCb),
        "register mmap size");
    return DOCA_SUCCESS;
}

static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        gForceQuit = true;
    }
}

static doca_error_t worker(const CdnClientCfg &aCfg, CdnClientRscs &aRscs) {
    CHECK_RETURN(init(aCfg, aRscs), "init app");

    while (!gForceQuit) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    destroy(aRscs);
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    CdnClientCfg cfg = {.ibdevName = "mlx5_3",
                        .gidIdx = 1,
                        .serverIpAddr = "12.12.12.2",
                        .mmapSize = kDefaultMmapSize,
                        .nbThreads = 3};

    CHECK_RETURN(doca_argp_init("cdn_client", &cfg), "init argp");
    CHECK_RETURN(registerParams(cfg), "register params");
    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");
    CHECK_RETURN(doca_argp_destroy(), "destroy argp");

    if (cfg.nbThreads == 0 || cfg.mmapSize == 0) {
        DOCA_LOG_ERR("Invalid config: nbThreads=%u mmapSize=%zu",
                     cfg.nbThreads, cfg.mmapSize);
        return EXIT_FAILURE;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    const uint16_t basePort = 22345;
    std::vector<CdnClientRscs> rscss;
    rscss.reserve(cfg.nbThreads);
    for (uint16_t i = 0; i < cfg.nbThreads; i++) {
        rscss.push_back({.threadId = i,
                         .port = static_cast<uint16_t>(basePort + i)});
    }

    std::vector<std::jthread> threads;
    threads.reserve(cfg.nbThreads);
    for (CdnClientRscs &rscs : rscss) {
        threads.emplace_back(worker, cfg, std::ref(rscs));
    }

    DOCA_LOG_INFO("Client mmap exported; press Ctrl-C to stop");
    for (std::jthread &thread : threads) {
        thread.join();
    }

    return 0;
}
