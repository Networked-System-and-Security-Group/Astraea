#include <doca_argp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <signal.h>

#include <cstdlib>
#include <cstring>

#include "common.h"
#include "memscan_dpu.h"

DOCA_LOG_REGISTER(MEMSCAN:DPU : MAIN);

bool gForceQuit = false;

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
    static_cast<MemscanCfg *>(aCfg)->port =
        *static_cast<uint16_t *>(aVal);
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
    static_cast<MemscanCfg *>(aCfg)->regionSizeKb =
        *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t nbBurstsCb(void *aVal, void *aCfg) {
    static_cast<MemscanCfg *>(aCfg)->nbBursts = *static_cast<u32 *>(aVal);
    return DOCA_SUCCESS;
}

static doca_error_t registerParams(MemscanCfg &aCfg) {
    CHECK_RETURN(
        registerOneParam("d", "ibdev name", DOCA_ARGP_TYPE_STRING,
                         ibdevNameCb),
        "register ibdev name");
    CHECK_RETURN(
        registerOneParam("p", "port to listen for host mmap descriptor",
                         DOCA_ARGP_TYPE_INT, portCb),
        "register port");
    CHECK_RETURN(
        registerOneParam("t", "burst period in ms", DOCA_ARGP_TYPE_INT,
                         tScanMsCb),
        "register tScanMs");
    CHECK_RETURN(
        registerOneParam("n", "DMA pages per burst", DOCA_ARGP_TYPE_INT,
                         nPagesCb),
        "register nPages");
    CHECK_RETURN(
        registerOneParam("r", "scan region size in KB", DOCA_ARGP_TYPE_INT,
                         regionSizeKbCb),
        "register regionSizeKb");
    CHECK_RETURN(
        registerOneParam("b", "number of bursts (0 = unlimited)",
                         DOCA_ARGP_TYPE_INT, nbBurstsCb),
        "register nbBursts");
    return DOCA_SUCCESS;
}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");

    MemscanCfg cfg = {
        .ibdevName = "mlx5_0",
        .tScanMs = 5,
        .nPages = 64,
        .regionSizeKb = 64,
        .nbBursts = 100,
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

    runTasks(cfg, rscs);

    destroy(rscs);
    return EXIT_SUCCESS;
}
