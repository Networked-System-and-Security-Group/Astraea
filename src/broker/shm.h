#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

constexpr char kShmName[] = "/astraea_shm";
constexpr uint32_t kMaxNbApps = 2;

/* The initialization here is actually useless, we perform shared memory
 * initialization in Scheduler */
struct AppData {
    alignas(64) uint32_t ecTime = 0;
    alignas(64) uint32_t usage = 0;
    alignas(64) size_t granularity = 4096;
    alignas(64) uint32_t vioTimes = 0;
    alignas(64) std::atomic_flag timeLock = ATOMIC_FLAG_INIT;
    alignas(64) std::atomic_flag vioLock = ATOMIC_FLAG_INIT;
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