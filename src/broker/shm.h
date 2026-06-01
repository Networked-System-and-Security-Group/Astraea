#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

constexpr char kShmName[] = "/astraea_shm";
constexpr uint32_t kMaxNbApps = 4;

/* The initialization here is actually useless, we perform shared memory
 * initialization in Scheduler */
struct AppData {
    alignas(64) uint32_t ecTime = 0;
    alignas(64) uint32_t dmaTime = 0;
    alignas(64) uint32_t hasEc = 0;
    alignas(64) uint32_t hasDma = 0;
    alignas(64) uint32_t ecUsage = 0;
    alignas(64) uint32_t dmaUsage = 0;
    alignas(64) size_t ecGranularity = 4096;
    alignas(64) size_t dmaGranularity = 4096;
    alignas(64) uint32_t ecVioTimes = 0;
    alignas(64) uint32_t dmaVioTimes = 0;
    alignas(64) std::atomic_flag ecTimeLock = ATOMIC_FLAG_INIT;
    alignas(64) std::atomic_flag dmaTimeLock = ATOMIC_FLAG_INIT;
    alignas(64) std::atomic_flag ecVioLock = ATOMIC_FLAG_INIT;
    alignas(64) std::atomic_flag dmaVioLock = ATOMIC_FLAG_INIT;
};

struct SharedData {
    alignas(64) uint32_t nbApps = 0;
    alignas(64) std::atomic_flag nbAppsLock = ATOMIC_FLAG_INIT;
    alignas(64) AppData appDatas[kMaxNbApps];
};

class LockHelper {
   public:
    void lock(std::atomic_flag &flag) {
        while (flag.test_and_set(std::memory_order_acquire));
    }

    void unlock(std::atomic_flag &flag) {
        flag.clear(std::memory_order_release);
    }
};
