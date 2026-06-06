#include "Pe.h"

#include <dlfcn.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

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

static std::atomic_flag &timeLockFor(AppData &aAppData, AccelKind aKind) {
    return aKind == AccelKind::Dma ? aAppData.dmaTimeLock
                                   : aAppData.ecTimeLock;
}

static u32 &availTimeFor(AppData &aAppData, AccelKind aKind) {
    return aKind == AccelKind::Dma ? aAppData.dmaTime : aAppData.ecTime;
}

static u32 &usageFor(AppData &aAppData, AccelKind aKind) {
    return aKind == AccelKind::Dma ? aAppData.dmaUsage : aAppData.ecUsage;
}

static u32 slaFor(AccelKind aKind) {
    return aKind == AccelKind::Dma ? gDmaSla : gEcSla;
}

static u32 activeCountFor(AccelKind aKind) {
    const u32 nbApps = std::min(gSharedData->nbApps, kMaxNbApps);
    u32 nbActive = 0;
    for (u32 i = 0; i < nbApps; i++) {
        const AppData &appData = gSharedData->appDatas[i];
        if (aKind == AccelKind::Dma ? appData.hasDma : appData.hasEc) {
            nbActive++;
        }
    }
    return nbActive;
}

static bool hasPendingWork(Pe *aPe, const std::vector<u32> &aSubmitPoss) {
    if (aSubmitPoss.size() < aPe->mCtxs.size()) {
        return true;
    }

    for (u32 i = 0; i < aPe->mCtxs.size(); i++) {
        if (aPe->mIsStoppeds[i]) {
            continue;
        }
        const auto ctx = aPe->mCtxs[i];
        if (aSubmitPoss[i] != ctx->mTail.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

static void worker(std::stop_token aToken, Pe *aPe) {
    std::vector<u32> submitPoss;
    LockHelper lockHelper;
    while (!aToken.stop_requested()) {
        bool didWork = false;
        if (submitPoss.size() < aPe->mCtxs.size()) {
            submitPoss.resize(aPe->mCtxs.size(), 0);
        }

        for (u32 i = 0; i < aPe->mLocks.size(); i++) {
            /* Skip the stopped ctx */
            if (aPe->mIsStoppeds[i]) {
                continue;
            }

            const auto ctx = aPe->mCtxs[i];
            u32 &submitPos = submitPoss[i];

            u32 currentTail = ctx->mTail.load(std::memory_order_acquire);
            if (submitPos == currentTail) {
                continue;
            }

            AppData &appData = gSharedData->appDatas[gAppId];
            auto &timeLock = timeLockFor(appData, ctx->mAccelKind);
            lockHelper.lock(timeLock);

            u32 &availTime = availTimeFor(appData, ctx->mAccelKind);
            u32 &cost = ctx->mTaskCosts[submitPos];
            u32 &usage = usageFor(appData, ctx->mAccelKind);
            /* Avoid fragment to improve utilization */
            bool haveAvailTime = availTime > 0;
            const u32 sla = slaFor(ctx->mAccelKind);
            bool nearViolated =
                ctx->mUserDatas[submitPos].rawTask->mExpectTime -
                    std::chrono::high_resolution_clock::now() <
                std::chrono::microseconds(static_cast<u32>(sla * 1.1));
            // if (nearViolated) {
            //     DOCA_LOG_INFO("near violated");
            // }
            if (haveAvailTime || nearViolated ||
                activeCountFor(ctx->mAccelKind) <= 1) {
                aPe->mLocks[i]->lock();
                CHECK_LOG(original_doca_task_submit(ctx->mSubTaskQ[submitPos]),
                          "submit subtask");
                aPe->mLocks[i]->unlock();
                submitPos = (submitPos + 1) & kTaskQueueMask;
                availTime = availTime > cost ? availTime - cost : 0;
                usage += cost;
                didWork = true;
            }
            lockHelper.unlock(timeLock);
        }

        if (!didWork) {
            std::unique_lock<std::mutex> lock(aPe->mWorkerMtx);
            aPe->mWorkerCv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return aToken.stop_requested() ||
                       hasPendingWork(aPe, submitPoss);
            });
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
    myPe->notifyWorker();
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
