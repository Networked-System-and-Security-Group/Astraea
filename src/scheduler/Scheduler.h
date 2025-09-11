#ifndef ASTRAEA_SCHEDULER_H_
#define ASTRAEA_SCHEDULER_H_

#include "../broker/shm.h"
#include <array>
#include <cstdint>

using u32 = uint32_t;
constexpr double kEwmaCoef = 0.1;
constexpr double kReservePortion = 0.7;

constexpr u32 kPeriodIsMs = 1;
constexpr u32 kUssPerPeriod = 1000 * kPeriodIsMs;
constexpr u32 kResvUssPerPeriod = kUssPerPeriod * kReservePortion;
constexpr u32 kAvailUssPerPeriod = kUssPerPeriod - kResvUssPerPeriod;

class Scheduler {
    int mShmFd = 0;
    SharedData *mShmData = nullptr;
    std::array<u32, kMaxNbApps> mAllocations = {0};

  public:
    Scheduler();
    ~Scheduler();
    void schedule();
};

#endif