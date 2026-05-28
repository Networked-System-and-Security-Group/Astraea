#include "Scheduler.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../broker/shm.h"

Scheduler::Scheduler() {
    mShmFd = shm_open(kShmName, O_CREAT | O_RDWR, 0666);
    ftruncate(mShmFd, sizeof(SharedData));
    mShmData = static_cast<SharedData *>(mmap(nullptr, sizeof(SharedData),
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, mShmFd, 0));
    if (mShmData == MAP_FAILED) {
        printf("Failed to map shared memory\n");
        return;
    }

    for (auto &allocation : mAllocations) {
        allocation = kUssPerPeriod;
    }

    /* Initialize the data on shared memory */
    mShmData->nbApps = 0;
    mShmData->nbAppsLock.clear(std::memory_order_release);
    for (u32 i = 0; i < kMaxNbApps; i++) {
        mShmData->appDatas[i].timeLock.clear(std::memory_order_release);
        mShmData->appDatas[i].vioLock.clear(std::memory_order_release);
        mShmData->appDatas[i].ecTime = kUssPerPeriod;
        mShmData->appDatas[i].vioTimes = 0;
        mShmData->appDatas[i].granularity = 8192;
        mShmData->appDatas[i].usage = 0;
    }
    ////////////////////////////////////////////////////////////////
}

Scheduler::~Scheduler() {
    munmap(mShmData, sizeof(SharedData));
    close(mShmFd);
    shm_unlink(kShmName);
}

bool gForceQuit = false;

static void signalHandler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("Signal %d received, preparing to exit...\n", signum);
        gForceQuit = true;
    }
}

static inline size_t increaseGranularity(size_t aPreGranularity) {
    switch (aPreGranularity) {
            // case 2048: {
            //     return 4096;
            // }
            // case 4096: {
            //     return 8192;
            // }
            // default: {
            //     return 8192;
            // }

        case 8192: {
            return 16384;
        }
        case 16384: {
            return 32768;
        }
        default: {
            return 32768;
        }
    }
}

static inline size_t decreaseGranularity(size_t aPreGranularity) {
    switch (aPreGranularity) {
            // case 8192: {
            //     return 4096;
            // }
            // case 4096: {
            //     return 2048;
            // }
            // default: {
            //     return 2048;
            // }

        case 32768: {
            return 16384;
        }
        case 16384: {
            return 8192;
        }
        default: {
            return 8192;
        }
    }
}

void Scheduler::schedule() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    LockHelper lockHelper;
    double predictions[kMaxNbApps];
    double utilizations[kMaxNbApps];
    double predSum, vioSum;

    do {
        lockHelper.lock(mShmData->nbAppsLock);

        predSum = 0;
        vioSum = 0;
        for (u32 i = 0; i < mShmData->nbApps; i++) {
            u32 usage = mShmData->appDatas[i].usage;
            utilizations[i] = usage * 1.0 / mAllocations[i];
            predictions[i] =
                kEwmaCoef * usage + (1 - kEwmaCoef) * mAllocations[i];
            predSum += predictions[i];
            vioSum += mShmData->appDatas[i].vioTimes;
        }

        for (u32 i = 0; i < mShmData->nbApps; i++) {
            if (utilizations[i] < 0.5) {
                mShmData->appDatas[i].granularity =
                    increaseGranularity(mShmData->appDatas[i].granularity);
            }

            for (u32 j = 0; j < mShmData->nbApps; j++) {
                if (j != i && mShmData->appDatas[j].vioTimes > 3) {
                    mShmData->appDatas[i].granularity =
                        decreaseGranularity(mShmData->appDatas[i].granularity);
                    break;
                }
            }
        }

        for (u32 i = 0; i < mShmData->nbApps; i++) {
            u32 allocation =
                vioSum == 0 ? predictions[i] / predSum * kUssPerPeriod
                            : predictions[i] / predSum * kAvailUssPerPeriod +
                                  mShmData->appDatas[i].vioTimes / vioSum *
                                      kResvUssPerPeriod;

            allocation = allocation < 1 ? kUssPerPeriod / 2 : allocation;
            mAllocations[i] = allocation;
            mShmData->appDatas[i].ecTime = allocation;

            mShmData->appDatas[i].vioTimes = 0;
            mShmData->appDatas[i].usage = 0;
        }

        lockHelper.unlock(mShmData->nbAppsLock);

        std::this_thread::sleep_for(std::chrono::milliseconds(kPeriodIsMs));
    } while (!gForceQuit);
}