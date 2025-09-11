#include "common.h"
#include "shm.h"
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>

const u32 gSla = atof(getenv("SLA"));
u32 gShmFd;
SharedData *gSharedData;
u32 gAppId;

class Initializer {
  public:
    Initializer() {
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