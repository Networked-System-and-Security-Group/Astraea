#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_dev.h>
#include <signal.h>
#include <thread>
#include <vector>
#include <cstdlib>

#include "common.h"
#include "doca_types.h"
#include "rdma.h"
#include "replica_client.h"

DOCA_LOG_REGISTER(REPLICA:CLIENT : MAIN);

doca_error_t ibdevNameCb(void *aIbdevName, void *aCfg) {
    size_t nameLen = strlen(static_cast<char *>(aIbdevName));
    memcpy(static_cast<ReplicaCfg *>(aCfg)->ibdevName, aIbdevName, nameLen);
    return DOCA_SUCCESS;
}

doca_error_t nbThreadsCb(void *aNbThreads, void *aCfg) {
    static_cast<ReplicaCfg *>(aCfg)->nbThreads =
        *static_cast<uint16_t *>(aNbThreads);
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(ReplicaCfg &cfg) {
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

// Worker 使用共享资源
doca_error_t worker(const ReplicaCfg &aCfg, const SharedRscs &shared, ReplicaRscs &rscs) {
    CHECK_RETURN(init(aCfg, shared, rscs), "init app");

    while (!gForceQuit) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    destroy(rscs);
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    // 2GB 内存
    ReplicaCfg cfg = {.ibdevName = "mlx5_2",
                      .gidIdx = 1,
                      .mmapSize = 2UL * 1024 * 1024 * 1024,
                      .nbThreads = 1};

    CHECK_RETURN(doca_argp_init("replica_client", &cfg), "init argp");
    CHECK_RETURN(registerParams(cfg), "register params");
    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");
    CHECK_RETURN(doca_argp_destroy(), "destroy argp");

    // --- 全局资源初始化 ---
    SharedRscs shared;
    shared.memSize = cfg.mmapSize;

    // 1. 打开设备 (只打开一次，节省 UAR 资源)
    CHECK_RETURN(openDev(cfg.ibdevName, shared.dev), "open device");

    // 2. 分配大页内存 (只分配一次)
    shared.memAddr = std::aligned_alloc(64, shared.memSize);
    if (shared.memAddr == nullptr) {
        DOCA_LOG_ERR("Failed to allocate memory");
        return -1;
    }
    memset(shared.memAddr, 0, shared.memSize);

    // 3. 创建并启动 mmap (只注册一次，节省 Locked Memory 资源)
    CHECK_RETURN(doca_mmap_create(&shared.mmap), "create mmap");
    CHECK_RETURN(doca_mmap_set_memrange(shared.mmap, shared.memAddr, shared.memSize), "set memrange");
    CHECK_RETURN(doca_mmap_add_dev(shared.mmap, shared.dev), "add dev to mmap");
    CHECK_RETURN(doca_mmap_set_permissions(shared.mmap, 
        DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | 
        DOCA_ACCESS_FLAG_PCI_READ_WRITE | 
        DOCA_ACCESS_FLAG_RDMA_READ | 
        DOCA_ACCESS_FLAG_RDMA_WRITE), "set permissions");
    CHECK_RETURN(doca_mmap_start(shared.mmap), "start mmap");

    DOCA_LOG_INFO("Global resources initialized: Single Device, 2GB memory registered.");

    // --- 启动线程 ---
    std::vector<ReplicaRscs> rscss;
    const uint16_t basePort = 12345;

    for (uint16_t i = 0; i < cfg.nbThreads; i++) {
        uint16_t portId = basePort + i;
        ReplicaRscs rscs = {.threadId = i, .port = portId};
        rscss.push_back(rscs);
    }

    std::vector<std::jthread> threads;
    for (ReplicaRscs &rscs : rscss) {
        // 传递 shared 引用，让所有线程复用
        threads.emplace_back(worker, cfg, std::ref(shared), std::ref(rscs));
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    DOCA_LOG_INFO("Press Ctrl-C to stop");

    // 等待线程结束
    for (auto &t : threads) {
        if(t.joinable()) t.join();
    }

    // --- 全局资源释放 ---
    CHECK_LOG(doca_mmap_destroy(shared.mmap), "destroy shared mmap");
    std::free(shared.memAddr);
    CHECK_LOG(doca_dev_close(shared.dev), "close shared dev");

    return 0;
}