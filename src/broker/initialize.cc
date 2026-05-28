#include <fcntl.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>

#include "common.h"
#include "shm.h"

u32 gSla;
u32 gShmFd;
SharedData *gSharedData;
u32 gAppId;

class Initializer {
   public:
    Initializer() {
        const char *slaStr = getenv("SLA");
        if (!slaStr) {
            fprintf(stderr, "[Astraea] Error: SLA environment variable is not set\n");
            exit(EXIT_FAILURE);
        }
        gSla = static_cast<u32>(atof(slaStr));

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