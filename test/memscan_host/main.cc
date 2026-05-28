#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "common.h"
#include "dma.h"

DOCA_LOG_REGISTER(MEMSCAN:HOST : MAIN);

static bool gRunning = true;

static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, shutting down...\n", signum);
        gRunning = false;
    }
}

struct HostCfg {
    char ibdevName[1024];
    char dpuIp[64];
    uint16_t port;
    u32 memSizeMb;
};

static doca_error_t ibdevNameCb(void *aVal, void *aCfg) {
    size_t len = strlen(static_cast<char *>(aVal));
    memcpy(static_cast<HostCfg *>(aCfg)->ibdevName, aVal, len);
    return DOCA_SUCCESS;
}

static doca_error_t dpuIpCb(void *aVal, void *aCfg) {
    size_t len = strlen(static_cast<char *>(aVal));
    memcpy(static_cast<HostCfg *>(aCfg)->dpuIp, aVal, len);
    return DOCA_SUCCESS;
}

static doca_error_t portCb(void *aVal, void *aCfg) {
    static_cast<HostCfg *>(aCfg)->port = *static_cast<uint16_t *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t memSizeMbCb(void *aVal, void *aCfg) {
    static_cast<HostCfg *>(aCfg)->memSizeMb = *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(HostCfg &aCfg) {
    CHECK_RETURN(registerOneParam("d", "ibdev name (host side)",
                                  DOCA_ARGP_TYPE_STRING, ibdevNameCb),
                 "register ibdev name");
    CHECK_RETURN(registerOneParam("s", "DPU server IP address",
                                  DOCA_ARGP_TYPE_STRING, dpuIpCb),
                 "register dpu ip");
    CHECK_RETURN(registerOneParam("p", "port for mmap descriptor exchange",
                                  DOCA_ARGP_TYPE_INT, portCb),
                 "register port");
    CHECK_RETURN(registerOneParam("m", "host memory size in MB",
                                  DOCA_ARGP_TYPE_INT, memSizeMbCb),
                 "register mem size");
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    HostCfg cfg = {
        .ibdevName = "mlx5_0",
        .dpuIp = "192.168.100.2",
        .port = 22445,
        .memSizeMb = 16,
    };

    CHECK_RETURN(doca_argp_init("memscan_host", &cfg), "init argp");
    CHECK_RETURN(registerParams(cfg), "register params");
    CHECK_RETURN(doca_argp_start(argc, argv), "start argp");
    CHECK_RETURN(doca_argp_destroy(), "destroy argp");

    DOCA_LOG_INFO("Config: ibdev=%s, dpuIp=%s, port=%u, memSizeMb=%u",
                  cfg.ibdevName, cfg.dpuIp, cfg.port, cfg.memSizeMb);

    doca_dev *dev;
    CHECK_RETURN(openDev(cfg.ibdevName, dev), "open device");

    size_t memSizeBytes = static_cast<size_t>(cfg.memSizeMb) * 1024 * 1024;
    void *memAddr;
    if (posix_memalign(&memAddr, 64, memSizeBytes) != 0) {
        DOCA_LOG_ERR("Failed to allocate %u MB of host memory", cfg.memSizeMb);
        doca_dev_close(dev);
        return EXIT_FAILURE;
    }
    /* Fill with a recognizable pattern so DPU-side inspection can verify */
    memset(memAddr, 0xAB, memSizeBytes);

    doca_mmap *mmap;
    CHECK_RETURN(doca_mmap_create(&mmap), "create mmap");
    CHECK_RETURN(doca_mmap_set_memrange(mmap, memAddr, memSizeBytes),
                 "set mmap range");
    /* PCI_READ_WRITE lets the DPU read (and write) this region via DMA */
    CHECK_RETURN(
        doca_mmap_set_permissions(mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE |
                                            DOCA_ACCESS_FLAG_PCI_READ_WRITE),
        "set mmap permissions");
    CHECK_RETURN(doca_mmap_add_dev(mmap, dev), "add dev to mmap");
    CHECK_RETURN(doca_mmap_start(mmap), "start mmap");

    DOCA_LOG_INFO("Allocated %u MB host memory, connecting to DPU %s:%u",
                  cfg.memSizeMb, cfg.dpuIp, cfg.port);

    CHECK_RETURN(dmaExportToServer(dev, cfg.dpuIp, cfg.port, mmap),
                 "export mmap to DPU");

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    DOCA_LOG_INFO("Memory exported. DPU may now scan. Press Ctrl-C to exit.");
    while (gRunning) {
        pause();
    }

    doca_mmap_destroy(mmap);
    free(memAddr);
    doca_dev_close(dev);

    return EXIT_SUCCESS;
}
