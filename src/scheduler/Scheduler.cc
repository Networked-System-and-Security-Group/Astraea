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

constexpr size_t kEcGranularitySmall = 8192;
constexpr size_t kEcGranularityMedium = 16384;
constexpr size_t kEcGranularityLarge = 32768;
constexpr size_t kDmaGranularitySmall = 262094;
constexpr size_t kDmaGranularityMedium = 524188;
constexpr size_t kDmaGranularityLarge = 1048376;

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

    for (auto &allocation : mEcAllocations) {
        allocation = kUssPerPeriod;
    }
    for (auto &allocation : mDmaAllocations) {
        allocation = kUssPerPeriod;
    }

    /* Initialize the data on shared memory */
    mShmData->nbApps = 0;
    mShmData->nbAppsLock.clear(std::memory_order_release);
    for (u32 i = 0; i < kMaxNbApps; i++) {
        mShmData->appDatas[i].ecTimeLock.clear(std::memory_order_release);
        mShmData->appDatas[i].dmaTimeLock.clear(std::memory_order_release);
        mShmData->appDatas[i].ecVioLock.clear(std::memory_order_release);
        mShmData->appDatas[i].dmaVioLock.clear(std::memory_order_release);
        mShmData->appDatas[i].ecTime = kUssPerPeriod;
        mShmData->appDatas[i].dmaTime = kUssPerPeriod;
        mShmData->appDatas[i].hasEc = 0;
        mShmData->appDatas[i].hasDma = 0;
        mShmData->appDatas[i].ecVioTimes = 0;
        mShmData->appDatas[i].dmaVioTimes = 0;
        mShmData->appDatas[i].ecGranularity = kEcGranularitySmall;
        mShmData->appDatas[i].dmaGranularity = kDmaGranularitySmall;
        mShmData->appDatas[i].ecUsage = 0;
        mShmData->appDatas[i].dmaUsage = 0;
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

static inline size_t increaseEcGranularity(size_t aPreGranularity) {
    switch (aPreGranularity) {
        case kEcGranularitySmall: {
            return kEcGranularityMedium;
        }
        case kEcGranularityMedium: {
            return kEcGranularityLarge;
        }
        default: {
            return kEcGranularityLarge;
        }
    }
}

static inline size_t decreaseEcGranularity(size_t aPreGranularity) {
    switch (aPreGranularity) {
        case kEcGranularityLarge: {
            return kEcGranularityMedium;
        }
        case kEcGranularityMedium: {
            return kEcGranularitySmall;
        }
        default: {
            return kEcGranularitySmall;
        }
    }
}

static inline size_t increaseDmaGranularity(size_t aPreGranularity) {
    switch (aPreGranularity) {
        case kDmaGranularitySmall: {
            return kDmaGranularityMedium;
        }
        case kDmaGranularityMedium: {
            return kDmaGranularityLarge;
        }
        default: {
            return kDmaGranularityLarge;
        }
    }
}

static inline size_t decreaseDmaGranularity(size_t aPreGranularity) {
    switch (aPreGranularity) {
        case kDmaGranularityLarge: {
            return kDmaGranularityMedium;
        }
        case kDmaGranularityMedium: {
            return kDmaGranularitySmall;
        }
        default: {
            return kDmaGranularitySmall;
        }
    }
}

static void scheduleAccelerator(
    SharedData *aShmData, std::array<u32, kMaxNbApps> &aAllocations,
    u32 AppData::*aActiveField, u32 AppData::*aTimeField,
    u32 AppData::*aUsageField, size_t AppData::*aGranularityField,
    u32 AppData::*aVioField, size_t (*aIncreaseGranularity)(size_t),
    size_t (*aDecreaseGranularity)(size_t)) {
    double predictions[kMaxNbApps];
    double utilizations[kMaxNbApps];
    double predSum = 0;
    double vioSum = 0;

    for (u32 i = 0; i < aShmData->nbApps; i++) {
        if (!(aShmData->appDatas[i].*aActiveField)) {
            continue;
        }
        u32 usage = (aShmData->appDatas[i].*aUsageField);
        utilizations[i] = usage * 1.0 / aAllocations[i];
        predictions[i] = kEwmaCoef * usage + (1 - kEwmaCoef) * aAllocations[i];
        predSum += predictions[i];
        vioSum += (aShmData->appDatas[i].*aVioField);
    }

    if (predSum == 0) {
        return;
    }

    for (u32 i = 0; i < aShmData->nbApps; i++) {
        if (!(aShmData->appDatas[i].*aActiveField)) {
            continue;
        }
        if (utilizations[i] < 0.5) {
            aShmData->appDatas[i].*aGranularityField =
                aIncreaseGranularity(aShmData->appDatas[i].*aGranularityField);
        }

        for (u32 j = 0; j < aShmData->nbApps; j++) {
            if (!(aShmData->appDatas[j].*aActiveField)) {
                continue;
            }
            if (j != i && (aShmData->appDatas[j].*aVioField) > 3) {
                aShmData->appDatas[i].*aGranularityField = aDecreaseGranularity(
                    aShmData->appDatas[i].*aGranularityField);
                break;
            }
        }
    }

    for (u32 i = 0; i < aShmData->nbApps; i++) {
        if (!(aShmData->appDatas[i].*aActiveField)) {
            aShmData->appDatas[i].*aTimeField = 0;
            aShmData->appDatas[i].*aUsageField = 0;
            aShmData->appDatas[i].*aVioField = 0;
            continue;
        }
        u32 allocation = vioSum == 0
                             ? predictions[i] / predSum * kUssPerPeriod
                             : predictions[i] / predSum * kAvailUssPerPeriod +
                                   (aShmData->appDatas[i].*aVioField) / vioSum *
                                       kResvUssPerPeriod;

        allocation = allocation < 1 ? kUssPerPeriod / 2 : allocation;
        aAllocations[i] = allocation;

        aShmData->appDatas[i].*aVioField = 0;
        aShmData->appDatas[i].*aUsageField = 0;
    }
}

void Scheduler::schedule() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    LockHelper lockHelper;

    do {
        lockHelper.lock(mShmData->nbAppsLock);

        scheduleAccelerator(mShmData, mEcAllocations, &AppData::hasEc,
                            &AppData::ecTime, &AppData::ecUsage,
                            &AppData::ecGranularity, &AppData::ecVioTimes,
                            increaseEcGranularity, decreaseEcGranularity);
        scheduleAccelerator(mShmData, mDmaAllocations, &AppData::hasDma,
                            &AppData::dmaTime, &AppData::dmaUsage,
                            &AppData::dmaGranularity, &AppData::dmaVioTimes,
                            increaseDmaGranularity, decreaseDmaGranularity);

        lockHelper.unlock(mShmData->nbAppsLock);

        std::this_thread::sleep_for(std::chrono::milliseconds(kPeriodIsMs));
    } while (!gForceQuit);
}
