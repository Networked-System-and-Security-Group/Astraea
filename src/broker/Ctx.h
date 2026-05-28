#pragma once

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_pe.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "Task.h"
#include "common.h"

const u32 kTaskQueueSize = 128;
const u32 kTaskQueueMask = kTaskQueueSize - 1;

namespace astraea {
class Pe;
class Ctx {
   public:
    doca_ctx *mCtx = nullptr;

    std::mutex *mLock = nullptr;
    Pe *mPe = nullptr;
    u32 mIsStoppedIdx = 0;

    alignas(64) std::atomic<u32> mHead = 0;
    alignas(64) std::atomic<u32> mTail = 0;
    alignas(64) doca_task *mSubTaskQ[kTaskQueueSize] = {nullptr};
    alignas(64) UserData mUserDatas[kTaskQueueSize] = {{0}};
    alignas(64) u32 mTaskCosts[kTaskQueueSize] = {0};

    alignas(64) std::mutex mtx;
    alignas(64) std::condition_variable not_empty_cv;
    alignas(64) std::condition_variable not_full_cv;

    virtual ~Ctx(){};

    virtual doca_error_t start() = 0;
    virtual doca_error_t stop() = 0;
    virtual doca_error_t connectToPe(Pe *aPe) = 0;
};
}  // namespace astraea