#pragma once

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_pe.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "common.h"

/* Forward declarations */
namespace astraea {
class Ctx;
}
////////////////////////////////

namespace astraea {

class Pe {
   public:
    doca_pe *mPe;
    std::vector<Ctx *> mCtxs;
    std::vector<std::mutex *> mLocks;
    /* We have to use uint8_t instead of bool
    as bool* is hard to acquire from std::vector<bool> */
    std::vector<u8> mIsStoppeds;
    std::mutex mWorkerMtx;
    std::condition_variable mWorkerCv;
    std::jthread *mWorker = nullptr;

    void notifyWorker() {
        std::lock_guard<std::mutex> lock(mWorkerMtx);
        mWorkerCv.notify_one();
    }
};
}  // namespace astraea
