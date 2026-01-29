#include "Pe.h"

#include <dlfcn.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>

#include "Ctx.h"
#include "common.h"
#include "original.h"
#include "shm.h"

using namespace astraea;

DOCA_LOG_REGISTER(ASTRAEA : PE)

/* Original function pointers */
uint8_t (*original_doca_pe_progress)(doca_pe *pe) =
    reinterpret_cast<uint8_t (*)(doca_pe *)>(dlsym(RTLD_NEXT,
                                                   "doca_pe_progress"));
doca_error_t (*original_doca_pe_create)(doca_pe **pe) =
    reinterpret_cast<doca_error_t (*)(doca_pe **)>(dlsym(RTLD_NEXT,
                                                         "doca_pe_create"));
doca_error_t (*original_doca_pe_destroy)(doca_pe *pe) =
    reinterpret_cast<doca_error_t (*)(doca_pe *)>(dlsym(RTLD_NEXT,
                                                        "doca_pe_destroy"));
doca_error_t (*original_doca_pe_connect_ctx)(doca_pe *pe, doca_ctx *ctx) =
    reinterpret_cast<doca_error_t (*)(doca_pe *, doca_ctx *)>(
        dlsym(RTLD_NEXT, "doca_pe_connect_ctx"));
////////////////////////////////////////////////////////////////////////////////
__attribute__((unused)) static void workerWoSched(std::stop_token aToken,
                                                  Pe *aPe) {
    u32 submitPos = 0;
    while (!aToken.stop_requested()) {
        for (u32 i = 0; i < aPe->mLocks.size(); i++) {
            aPe->mLocks[i]->lock();
            if (aPe->mIsStoppeds[i]) {
                aPe->mLocks[i]->unlock();
                continue;
            }

            const auto ctx = aPe->mCtxs[i];

            while (submitPos != ctx->mTail.load(std::memory_order_acquire)) {
                CHECK_LOG(original_doca_task_submit(ctx->mSubTaskQ[submitPos]),
                          "submit subtask");

                submitPos = (submitPos + 1) & kTaskQueueMask;
            }

            aPe->mLocks[i]->unlock();
        }
    }
}

static void worker(std::stop_token aToken, Pe *aPe) {
    u32 submitPos = 0;
    LockHelper lockHelper;
    while (!aToken.stop_requested()) {
        for (u32 i = 0; i < aPe->mLocks.size(); i++) {
            while (true) {
                /* Skip the stoppted ctx */
                if (aPe->mIsStoppeds[i]) {
                    break;
                }

                const auto ctx = aPe->mCtxs[i];

                u32 current_tail = ctx->mTail.load(std::memory_order_acquire);
                if (submitPos != current_tail) {
                    lockHelper.lock(gSharedData->appDatas[gAppId].timeLock);
                    u32 &availTime = gSharedData->appDatas[gAppId].ecTime;
                    u32 &cost = ctx->mTaskCosts[submitPos];
                    u32 &usage = gSharedData->appDatas[gAppId].usage;
                    /* Avoid fragment to improve utilization */
                    bool haveAvailTime = availTime > 0;
                    bool nearViolated =
                        ctx->mUserDatas[submitPos].rawTask->mExpectTime -
                            std::chrono::high_resolution_clock::now() <
                        std::chrono::microseconds(static_cast<u32>(gSla * 1.1));
                    // if (nearViolated) {
                    //     DOCA_LOG_INFO("near violated");
                    // }
                    if (haveAvailTime || nearViolated) {
                        aPe->mLocks[i]->lock();
                        CHECK_LOG(original_doca_task_submit(
                                      ctx->mSubTaskQ[submitPos]),
                                  "submit subtask");
                        aPe->mLocks[i]->unlock();
                        submitPos = (submitPos + 1) & kTaskQueueMask;
                        availTime = availTime > cost ? availTime - cost : 0;
                        usage += cost;
                    }
                    lockHelper.unlock(gSharedData->appDatas[gAppId].timeLock);
                } else {
                    std::unique_lock<std::mutex> lock(ctx->mtx);
                    ctx->not_empty_cv.wait(lock, [&] {
                        return submitPos !=
                                   ctx->mTail.load(std::memory_order_relaxed) ||
                               aPe->mIsStoppeds[i];
                    });
                }
            }
        }
    }
}

uint8_t doca_pe_progress(doca_pe *pe) {
    Pe *myPe = reinterpret_cast<Pe *>(pe);
    for (auto &lock : myPe->mLocks) {
        lock->lock();
    }

    u8 ret = original_doca_pe_progress(myPe->mPe);

    for (auto &lock : myPe->mLocks) {
        lock->unlock();
    }

    return ret;
}

doca_error_t doca_pe_create(doca_pe **pe) {
    Pe **myPe = reinterpret_cast<Pe **>(pe);
    *myPe = new Pe;
    (*myPe)->mWorker = new std::jthread{worker, *myPe};
    return original_doca_pe_create(&(*myPe)->mPe);
}

doca_error_t doca_pe_destroy(doca_pe *pe) {
    Pe *myPe = reinterpret_cast<Pe *>(pe);
    myPe->mWorker->request_stop();
    myPe->mWorker->join();
    doca_error_t status = original_doca_pe_destroy(myPe->mPe);

    for (auto &lock : myPe->mLocks) {
        delete lock;
    }

    delete myPe;

    return status;
}

doca_error_t doca_pe_connect_ctx(doca_pe *pe, doca_ctx *ctx) {
    Pe *myPe = reinterpret_cast<Pe *>(pe);
    Ctx *myCtx = reinterpret_cast<Ctx *>(ctx);

    return myCtx->connectToPe(myPe);
}