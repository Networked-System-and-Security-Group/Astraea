#include <fcntl.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>

#include "common.h"
#include "shm.h"

u32 gEcSla;
u32 gDmaSla;
u32 gShmFd;
SharedData *gSharedData;
u32 gAppId;

static u32 readSlaEnv(const char *name) {
    const char *slaStr = getenv(name);
    if (slaStr) {
        return static_cast<u32>(atof(slaStr));
    }

    return 0;
}

class Initializer {
   public:
    Initializer() {
        gEcSla = readSlaEnv("EC_SLA");
        gDmaSla = readSlaEnv("DMA_SLA");
        if (gEcSla == 0 && gDmaSla == 0) {
            fprintf(stderr,
                    "[Astraea] Error: EC_SLA or DMA_SLA environment variable "
                    "is not set\n");
            exit(EXIT_FAILURE);
        }

        gShmFd = shm_open(kShmName, O_RDWR, 0666);
        gSharedData = static_cast<SharedData *>(
            mmap(nullptr, sizeof(SharedData), PROT_READ | PROT_WRITE,
                 MAP_SHARED, gShmFd, 0));

        LockHelper lockHelper;
        lockHelper.lock(gSharedData->nbAppsLock);
        gAppId = gSharedData->nbApps;
        gSharedData->nbApps++;
        lockHelper.unlock(gSharedData->nbAppsLock);
    }

    ~Initializer() {
        munmap(gSharedData, sizeof(SharedData));
        close(gShmFd);
    }
};

Initializer initializer;
