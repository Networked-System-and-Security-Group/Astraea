#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <iostream>
#include <thread>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

#include <doca_pe.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include <doca_erasure_coding.h>
#include <ratio>
#include <vector>

#include "Buf.h"
#include "Ctx.h"
#include "Ec.h"
#include "Pe.h"
#include "Task.h"
#include "common.h"
#include "doca_types.h"
#include "original.h"
#include "shm.h"

using namespace astraea;

auto gLastExpectTime = std::chrono::high_resolution_clock::now();

DOCA_LOG_REGISTER(ASTRAEA : EC)

/* Original function pointers */
doca_error_t (*original_doca_ec_create)(doca_dev *, doca_ec **) =
    reinterpret_cast<doca_error_t (*)(doca_dev *, doca_ec **)>(
        dlsym(RTLD_NEXT, "doca_ec_create"));

doca_error_t (*original_doca_ec_destroy)(doca_ec *) =
    reinterpret_cast<doca_error_t (*)(doca_ec *)>(dlsym(RTLD_NEXT,
                                                        "doca_ec_destroy"));

doca_error_t (*original_doca_ec_matrix_create)(doca_ec *,
                                               enum doca_ec_matrix_type, size_t,
                                               size_t, doca_ec_matrix **) =
    reinterpret_cast<doca_error_t (*)(doca_ec *, enum doca_ec_matrix_type,
                                      size_t, size_t, doca_ec_matrix **)>(
        dlsym(RTLD_NEXT, "doca_ec_matrix_create"));

doca_error_t (*original_doca_ec_matrix_create_recover)(doca_ec *,
                                                       const doca_ec_matrix *,
                                                       uint32_t[], size_t,
                                                       doca_ec_matrix **) =
    reinterpret_cast<doca_error_t (*)(doca_ec *, const doca_ec_matrix *,
                                      uint32_t[], size_t, doca_ec_matrix **)>(
        dlsym(RTLD_NEXT, "doca_ec_matrix_create_recover"));

doca_error_t (*original_doca_ec_matrix_destroy)(doca_ec_matrix *) =
    reinterpret_cast<doca_error_t (*)(doca_ec_matrix *)>(
        dlsym(RTLD_NEXT, "doca_ec_matrix_destroy"));

doca_error_t (*original_doca_ec_task_create_set_conf)(
    doca_ec *, doca_ec_task_create_completion_cb_t,
    doca_ec_task_create_completion_cb_t, uint32_t) =
    reinterpret_cast<
        doca_error_t (*)(doca_ec *, doca_ec_task_create_completion_cb_t,
                         doca_ec_task_create_completion_cb_t, uint32_t)>(
        dlsym(RTLD_NEXT, "doca_ec_task_create_set_conf"));

doca_error_t (*original_doca_ec_task_create_allocate_init)(
    doca_ec *, const doca_ec_matrix *, const doca_buf *, doca_buf *, doca_data,
    doca_ec_task_create **) =
    reinterpret_cast<
        doca_error_t (*)(struct doca_ec *, const struct doca_ec_matrix *,
                         const struct doca_buf *, struct doca_buf *,
                         union doca_data, struct doca_ec_task_create **)>(
        dlsym(RTLD_NEXT, "doca_ec_task_create_allocate_init"));

doca_error_t (*original_doca_ec_task_recover_set_conf)(
    doca_ec *, doca_ec_task_recover_completion_cb_t,
    doca_ec_task_recover_completion_cb_t, uint32_t) =
    reinterpret_cast<
        doca_error_t (*)(doca_ec *, doca_ec_task_recover_completion_cb_t,
                         doca_ec_task_recover_completion_cb_t, uint32_t)>(
        dlsym(RTLD_NEXT, "doca_ec_task_recover_set_conf"));

doca_error_t (*original_doca_ec_task_recover_allocate_init)(
    doca_ec *, const doca_ec_matrix *, const doca_buf *, doca_buf *, doca_data,
    doca_ec_task_recover **) =
    reinterpret_cast<doca_error_t (*)(doca_ec *, const doca_ec_matrix *,
                                      const doca_buf *, doca_buf *, doca_data,
                                      doca_ec_task_recover **)>(
        dlsym(RTLD_NEXT, "doca_ec_task_recover_allocate_init"));

doca_ctx *(*original_doca_ec_as_ctx)(doca_ec *) =
    reinterpret_cast<doca_ctx *(*)(doca_ec *)>(dlsym(RTLD_NEXT,
                                                     "doca_ec_as_ctx"));

doca_task *(*original_doca_ec_task_create_as_task)(doca_ec_task_create *) =
    reinterpret_cast<doca_task *(*)(doca_ec_task_create *)>(
        dlsym(RTLD_NEXT, "doca_ec_task_create_as_task"));

doca_task *(*original_doca_ec_task_recover_as_task)(doca_ec_task_recover *) =
    reinterpret_cast<doca_task *(*)(doca_ec_task_recover *)>(
        dlsym(RTLD_NEXT, "doca_ec_task_recover_as_task"));
////////////////////////////////////////////////////////////////////////////////

doca_error_t Ec::start() {
    CHECK_LOG(original_doca_ctx_start(mCtx), "start ctx");

    for (u32 i = 0; i < kBufPoolSize; i++) {
        CHECK_LOG(
            original_doca_buf_inventory_buf_get_by_args(
                mInv, mMmap, static_cast<u8 *>(mMemAddr) + i * kPerBufSize,
                kPerBufSize, static_cast<u8 *>(mMemAddr) + i * kPerBufSize, 0,
                &mDstBufPool[i]),
            "alloc dst buf pool");
    }

    /* These are actually Buf* */
    doca_buf *tmpBuf1, *tmpBuf2;
    CHECK_LOG(doca_buf_inventory_buf_get_by_data(mInv, mMmap, mMemAddr,
                                                 128 * 128, &tmpBuf1),
              "alloc tmp buf1");
    CHECK_LOG(doca_buf_inventory_buf_get_by_addr(
                  mInv, mMmap, static_cast<char *>(mMemAddr) + 128 * 128,
                  128 * 128, &tmpBuf2),
              "alloc tmp buf2");

    doca_buf *realBuf1 = reinterpret_cast<Buf *>(tmpBuf1)->mBuf;
    doca_buf *realBuf2 = reinterpret_cast<Buf *>(tmpBuf2)->mBuf;

    doca_ec_matrix *tmpEncMat, *tmpDecMat;
    u32 missingIndices[] = {0, 1, 2, 3};
    CHECK_LOG(original_doca_ec_matrix_create(mEc, DOCA_EC_MATRIX_TYPE_CAUCHY,
                                             128, 32, &tmpEncMat),
              "create tmp enc mat");
    CHECK_LOG(original_doca_ec_matrix_create_recover(
                  mEc, tmpEncMat, missingIndices, 4, &tmpDecMat),
              "create tmp dec mat");

    for (u32 i = 0; i < kTaskQueueSize; i++) {
        CHECK_LOG(original_doca_ec_task_create_allocate_init(
                      mEc, tmpEncMat, realBuf1, realBuf2,
                      {.ptr = &mUserDatas[i]}, &mCreatePool[i]),
                  "populate create task pool");
        CHECK_LOG(original_doca_ec_task_recover_allocate_init(
                      mEc, tmpDecMat, realBuf1, realBuf2,
                      {.ptr = &mUserDatas[i]}, &mRecoverPool[i]),
                  "populate recover task pool");
    }

    CHECK_LOG(original_doca_ec_matrix_destroy(tmpEncMat),
              "destroy tmp enc matrix");
    CHECK_LOG(original_doca_ec_matrix_destroy(tmpDecMat),
              "destroy tmp dec matrix");
    CHECK_LOG(doca_buf_dec_refcount(tmpBuf1, nullptr),
              "dec tmp buf1 ref count");
    CHECK_LOG(doca_buf_dec_refcount(tmpBuf2, nullptr),
              "dec tmp buf2 ref count");

    return DOCA_SUCCESS;
}

doca_error_t Ec::stop() {
    *mIsStopped = true;
    not_empty_cv.notify_one();
    for (u32 i = 0; i < kBufPoolSize; i++) {
        CHECK_LOG(original_doca_buf_dec_refcount(mDstBufPool[i], nullptr),
                  "destroy dst buf in pool");
    }

    for (u32 i = 0; i < kTaskQueueSize; i++) {
        doca_task *task = original_doca_ec_task_create_as_task(mCreatePool[i]);
        original_doca_task_free(task);
        task = original_doca_ec_task_recover_as_task(mRecoverPool[i]);
        original_doca_task_free(task);
    }

    return original_doca_ctx_stop(mCtx);
}

doca_error_t Ec::connectToPe(Pe *aPe) {
    u32 idx = aPe->mLocks.size();
    aPe->mIsStoppeds.push_back(false);
    this->mIsStopped = &aPe->mIsStoppeds[idx];

    aPe->mCtxs.push_back(this);

    auto lock = new std::mutex;
    this->mLock = lock;
    aPe->mLocks.push_back(lock);

    CHECK_RETURN(original_doca_pe_connect_ctx(aPe->mPe, this->mCtx),
                 "connect pe to ctx");

    return DOCA_SUCCESS;
}

static u32 calCreateTimeCostPipeline(u32 aNbDataBlks, u32 aNbRdncBlks,
                                     size_t aBlkSize) {
    return (2.073565 * aNbDataBlks + 7.007787) *
           (-0.000020 * aNbRdncBlks + -0.000658) *
           (-0.023635 * aBlkSize + 34.032006);
}

static u32 calRecoverTimeCostPipeline(u32 aNbDataBlks, u32 aNbRdncBlks,
                                      size_t aBlkSize) {
    return (2.077807 * aNbDataBlks + 7.055615) *
           (-0.011992 * aNbRdncBlks - 0.398876) *
           (-0.000039 * aBlkSize + 0.033573);
}

static inline doca_error_t submitCreateSubTask(Ec *aEc, doca_buf *aSrcBuf,
                                               doca_buf *aDstBuf,
                                               EcTaskCreate *aRawTask,
                                               u32 aStripId, bool aIsSub,
                                               bool aIsLast, u32 aCost) {

    while (true) {
        const auto curTail = aEc->mTail.load(std::memory_order_relaxed);
        const auto nextTail = (curTail + 1) & kTaskQueueMask;
        const auto current_head = aEc->mHead.load(std::memory_order_acquire);

        if (nextTail != current_head) {
            // 往队列里生产数据
            /* Submit subtasks to queue (i.e. set task data) */
            doca_ec_task_create *specTask = aEc->mCreatePool[curTail];
            UserData &userData = aEc->mUserDatas[curTail];

            userData.rawData = aRawTask->mUserData;
            userData.rawTask = aRawTask;
            userData.srcBuf = aSrcBuf;
            userData.dstBuf = aDstBuf;
            userData.isLast = aIsLast;
            userData.isSub = aIsSub;

            doca_ec_task_create_set_coding_matrix(specTask,
                                                  aRawTask->mMat->mMat);
            doca_ec_task_create_set_original_data_blocks(specTask, aSrcBuf);
            doca_ec_task_create_set_rdnc_blocks(specTask, aDstBuf);
            aEc->mSubTaskQ[curTail] =
                original_doca_ec_task_create_as_task(aEc->mCreatePool[curTail]);

            aEc->mTaskCosts[curTail] = aCost;
            ////////////////////////////////////////////////////////////////
            aEc->mTail.store(nextTail, std::memory_order_release);

            aEc->not_empty_cv.notify_one();

            break;
        } else {
            std::unique_lock<std::mutex> lock(aEc->mtx);
            aEc->not_full_cv.wait(lock, [&] {
                return ((aEc->mTail.load(std::memory_order_relaxed) + 1) &
                        kTaskQueueMask) !=
                       aEc->mHead.load(std::memory_order_relaxed);
            });
        }
    }

    return DOCA_SUCCESS;
}

doca_error_t EcTaskCreate::submit() {
    const size_t granularity = gSharedData->appDatas[gAppId].granularity;
    const size_t blkSize = mSrcBuf->mDataLen / mMat->mNbDataBlks;
    const u32 nbStrips = gSharedData->nbApps > 1 ? blkSize / granularity : 1;

    auto curTime = std::chrono::high_resolution_clock::now();
    mExpectTime = std::chrono::microseconds(gSla) +
                  (curTime > gLastExpectTime ? curTime : gLastExpectTime);
    gLastExpectTime = mExpectTime;

    if (nbStrips > 1) {
        for (u32 stripId = 0; stripId < nbStrips; stripId++) {
            doca_buf *subSrcBuf;
            for (u32 blkId = 0; blkId < mMat->mNbDataBlks; blkId++) {
                doca_buf *subSubSrcBuf;
                CHECK_LOG(original_doca_buf_inventory_buf_get_by_args(
                              mEc->mInv, mSrcBuf->mMmap,
                              static_cast<u8 *>(mSrcBuf->mAddr) +
                                  blkId * blkSize + stripId * granularity,
                              granularity,
                              static_cast<u8 *>(mSrcBuf->mAddr) +
                                  blkId * blkSize + stripId * granularity,
                              granularity, &subSubSrcBuf),
                          "alloc sub src buf");
                if (blkId == 0) {
                    subSrcBuf = subSubSrcBuf;
                } else {
                    CHECK_LOG(doca_buf_chain_list(subSrcBuf, subSubSrcBuf),
                              "chain buf");
                }
            }
            doca_buf *subDstBuf = mEc->getPooledDstBuf();
            submitCreateSubTask(mEc, subSrcBuf, subDstBuf, this, stripId, true,
                                stripId == nbStrips - 1,
                                calCreateTimeCostPipeline(mMat->mNbDataBlks,
                                                          mMat->mNbRdncBlks,
                                                          granularity));
        }
    } else {
        CHECK_LOG(submitCreateSubTask(
                      mEc, mSrcBuf->mBuf, mDstBuf->mBuf, this, 0, false, true,
                      calCreateTimeCostPipeline(
                          mMat->mNbDataBlks, mMat->mNbRdncBlks, granularity)),
                  "submit sub task");
    }

    return DOCA_SUCCESS;
}
void EcTaskCreate::free() {}

static inline doca_error_t submitRecoverSubTask(Ec *aEc, doca_buf *aSrcBuf,
                                                doca_buf *aDstBuf,
                                                EcTaskRecover *aRawTask,
                                                u32 aStripId, bool aIsSub,
                                                bool aIsLast, u32 aCost) {

    while (true) {
        const auto curTail = aEc->mTail.load(std::memory_order_relaxed);
        const auto nextTail = (curTail + 1) & kTaskQueueMask;
        const auto current_head = aEc->mHead.load(std::memory_order_acquire);

        if (nextTail != current_head) {
            // 往队列里生产数据
            /* Submit subtasks to queue (i.e. set task data) */
            doca_ec_task_recover *specTask = aEc->mRecoverPool[curTail];
            UserData &userData = aEc->mUserDatas[curTail];

            userData.rawData = aRawTask->mUserData;
            userData.rawTask = aRawTask;
            userData.srcBuf = aSrcBuf;
            userData.dstBuf = aDstBuf;
            userData.isLast = aIsLast;
            userData.isSub = aIsSub;

            doca_ec_task_recover_set_recover_matrix(specTask,
                                                    aRawTask->mMat->mMat);
            doca_ec_task_recover_set_available_blocks(specTask, aSrcBuf);
            doca_ec_task_recover_set_recovered_data_blocks(specTask, aDstBuf);
            aEc->mSubTaskQ[curTail] = original_doca_ec_task_recover_as_task(
                aEc->mRecoverPool[curTail]);

            aEc->mTaskCosts[curTail] = aCost;
            ////////////////////////////////////////////////////////////////
            aEc->mTail.store(nextTail, std::memory_order_release);

            aEc->not_empty_cv.notify_one();

            break;
        } else {
            std::unique_lock<std::mutex> lock(aEc->mtx);
            aEc->not_full_cv.wait(lock, [&] {
                return ((aEc->mTail.load(std::memory_order_relaxed) + 1) &
                        kTaskQueueMask) !=
                       aEc->mHead.load(std::memory_order_relaxed);
            });
        }
    }

    return DOCA_SUCCESS;
}

doca_error_t EcTaskRecover::submit() {
    const size_t granularity = gSharedData->appDatas[gAppId].granularity;
    const size_t blkSize = mSrcBuf->mDataLen / mMat->mNbDataBlks;
    const u32 nbStrips = gSharedData->nbApps > 1 ? blkSize / granularity : 1;

    auto curTime = std::chrono::high_resolution_clock::now();
    mExpectTime = std::chrono::microseconds(gSla) +
                  (curTime > gLastExpectTime ? curTime : gLastExpectTime);
    gLastExpectTime = mExpectTime;

    if (nbStrips > 1) {
        for (u32 stripId = 0; stripId < nbStrips; stripId++) {
            doca_buf *subSrcBuf;
            for (u32 blkId = 0; blkId < mMat->mNbDataBlks; blkId++) {
                doca_buf *subSubSrcBuf;
                CHECK_LOG(original_doca_buf_inventory_buf_get_by_args(
                              mEc->mInv, mSrcBuf->mMmap,
                              static_cast<u8 *>(mSrcBuf->mAddr) +
                                  blkId * blkSize + stripId * granularity,
                              granularity,
                              static_cast<u8 *>(mSrcBuf->mAddr) +
                                  blkId * blkSize + stripId * granularity,
                              granularity, &subSubSrcBuf),
                          "alloc sub src buf");
                if (blkId == 0) {
                    subSrcBuf = subSubSrcBuf;
                } else {
                    CHECK_LOG(doca_buf_chain_list(subSrcBuf, subSubSrcBuf),
                              "chain buf");
                }
            }
            doca_buf *subDstBuf = mEc->getPooledDstBuf();
            submitRecoverSubTask(mEc, subSrcBuf, subDstBuf, this, stripId, true,
                                 stripId == nbStrips - 1,
                                 calRecoverTimeCostPipeline(mMat->mNbDataBlks,
                                                            mMat->mNbRdncBlks,
                                                            granularity));
        }
    } else {
        CHECK_LOG(submitRecoverSubTask(
                      mEc, mSrcBuf->mBuf, mDstBuf->mBuf, this, 0, false, true,
                      calRecoverTimeCostPipeline(
                          mMat->mNbDataBlks, mMat->mNbRdncBlks, granularity)),
                  "submit sub task");
    }

    return DOCA_SUCCESS;
}
void EcTaskRecover::free() {}

/* Ctx related */
static void createSubtaskSuccCb(doca_ec_task_create *task,
                                doca_data task_user_data,
                                doca_data ctx_user_data) {
    UserData *userData = static_cast<UserData *>(task_user_data.ptr);
    EcTaskCreate *rawTask = static_cast<EcTaskCreate *>(userData->rawTask);
    // DOCA_LOG_INFO("Subtask succeeded");
    if (userData->isSub) {
        CHECK_LOG(original_doca_buf_dec_refcount(userData->srcBuf, nullptr),
                  "destroy sub src buf");
        const size_t blkSize =
            rawTask->mSrcBuf->mDataLen / rawTask->mMat->mNbDataBlks;
        u8 *originalDstAddr = static_cast<u8 *>(rawTask->mDstBuf->mAddr);

        void *subDstAddr;
        size_t subDstLen;
        CHECK_LOG(doca_buf_get_data(userData->dstBuf, &subDstAddr),
                  "get sub dst addr");
        CHECK_LOG(doca_buf_get_data_len(userData->dstBuf, &subDstLen),
                  "get sub dst len");
        const size_t granularity = subDstLen / rawTask->mMat->mNbRdncBlks;

        for (u32 blkId = 0; blkId < rawTask->mMat->mNbRdncBlks; blkId++) {
            u8 *subSubdstAddr = originalDstAddr + blkId * blkSize +
                                granularity * userData->stripId;
            u8 *subSubSrcAddr =
                static_cast<u8 *>(subDstAddr) + blkId * granularity;
            memcpy(subSubdstAddr, subSubSrcAddr, granularity);
        }

        CHECK_LOG(original_doca_buf_set_data_len(userData->dstBuf, 0),
                  "set sub dst buf len to 0");
    }

    if (userData->isLast) {
        auto curTime = std::chrono::high_resolution_clock::now();
        if (curTime > rawTask->mExpectTime) {
            LockHelper lockHelper;
            lockHelper.lock(gSharedData->appDatas[gAppId].vioLock);
            gSharedData->appDatas[gAppId].vioTimes++;
            lockHelper.unlock(gSharedData->appDatas[gAppId].vioLock);
        }

        rawTask->mEc->mCreateSuccCb(
            reinterpret_cast<doca_ec_task_create *>(rawTask), userData->rawData,
            ctx_user_data);
    }

    uint32_t current_tail = rawTask->mEc->mTail.load(std::memory_order_acquire);

    const auto current_head =
        rawTask->mEc->mHead.load(std::memory_order_relaxed);
    rawTask->mEc->mHead.store((current_head + 1) & kTaskQueueMask,
                              std::memory_order_release);
    if (((current_tail + 1) & kTaskQueueMask) == current_head) {
        rawTask->mEc->not_full_cv.notify_one();
    }
}

static void createSubtaskErrCb(doca_ec_task_create *task,
                               doca_data task_user_data,
                               doca_data ctx_user_data) {
    DOCA_LOG_ERR("Subtask failed");
}

static void recoverSubtaskSuccCb(doca_ec_task_recover *task,
                                 doca_data task_user_data,
                                 doca_data ctx_user_data) {
    UserData *userData = static_cast<UserData *>(task_user_data.ptr);
    EcTaskRecover *rawTask = static_cast<EcTaskRecover *>(userData->rawTask);
    // DOCA_LOG_INFO("Subtask succeeded");
    if (userData->isSub) {
        CHECK_LOG(original_doca_buf_dec_refcount(userData->srcBuf, nullptr),
                  "destroy sub src buf");
        const size_t blkSize =
            rawTask->mSrcBuf->mDataLen / rawTask->mMat->mNbDataBlks;
        u8 *originalDstAddr = static_cast<u8 *>(rawTask->mDstBuf->mAddr);

        void *subDstAddr;
        size_t subDstLen;
        CHECK_LOG(doca_buf_get_data(userData->dstBuf, &subDstAddr),
                  "get sub dst addr");
        CHECK_LOG(doca_buf_get_data_len(userData->dstBuf, &subDstLen),
                  "get sub dst len");
        const size_t granularity = subDstLen / rawTask->mMat->mNbRdncBlks;

        for (u32 blkId = 0; blkId < rawTask->mMat->mNbDataBlks; blkId++) {
            u8 *subSubdstAddr = originalDstAddr + blkId * blkSize +
                                granularity * userData->stripId;
            u8 *subSubSrcAddr =
                static_cast<u8 *>(subDstAddr) + blkId * granularity;
            memcpy(subSubdstAddr, subSubSrcAddr, granularity);
        }

        CHECK_LOG(original_doca_buf_set_data_len(userData->dstBuf, 0),
                  "set sub dst buf len to 0");
    }

    if (userData->isLast) {
        auto curTime = std::chrono::high_resolution_clock::now();
        if (curTime > rawTask->mExpectTime) {
            LockHelper lockHelper;
            lockHelper.lock(gSharedData->appDatas[gAppId].vioLock);
            gSharedData->appDatas[gAppId].vioTimes++;
            lockHelper.unlock(gSharedData->appDatas[gAppId].vioLock);
        }

        rawTask->mEc->mRecoverSuccCb(
            reinterpret_cast<doca_ec_task_recover *>(rawTask),
            userData->rawData, ctx_user_data);
    }

    uint32_t current_tail = rawTask->mEc->mTail.load(std::memory_order_acquire);

    const auto current_head =
        rawTask->mEc->mHead.load(std::memory_order_relaxed);
    rawTask->mEc->mHead.store((current_head + 1) & kTaskQueueMask,
                              std::memory_order_release);
    if (((current_tail + 1) & kTaskQueueMask) == current_head) {
        rawTask->mEc->not_full_cv.notify_one();
    }
}

static void recoverSubtaskErrCb(doca_ec_task_recover *task,
                                doca_data task_user_data,
                                doca_data ctx_user_data) {
    DOCA_LOG_ERR("Subtask failed");
}

doca_error_t doca_ec_create(doca_dev *dev, doca_ec **ec) {
    auto myEc = reinterpret_cast<Ec **>(ec);

    *myEc = new Ec;

    int ret =
        posix_memalign(&(*myEc)->mMemAddr, 64, kBufPoolSize * kPerBufSize);
    if (ret) {
        DOCA_LOG_ERR("Failed to alloc buf pool memory");
    }

    CHECK_LOG(doca_buf_inventory_create(65536, &(*myEc)->mInv),
              "create buf inventory");

    CHECK_LOG(doca_buf_inventory_start((*myEc)->mInv), "start buf inventory");

    /* We use our overrided doca mmap functions */
    CHECK_LOG(doca_mmap_create(&(*myEc)->mMmap), "create ec internal mmap");

    CHECK_LOG(doca_mmap_add_dev((*myEc)->mMmap, dev),
              "add dev to internal mmap");

    CHECK_LOG(doca_mmap_set_memrange((*myEc)->mMmap, (*myEc)->mMemAddr,
                                     kBufPoolSize * kPerBufSize),
              "set ec internal mmap memrange");

    CHECK_LOG(doca_mmap_start((*myEc)->mMmap), "start ec internal mmap");

    CHECK_LOG(original_doca_ec_create(dev, &(*myEc)->mEc), "create ec");

    CHECK_LOG(original_doca_ec_task_create_set_conf(
                  (*myEc)->mEc, createSubtaskSuccCb, createSubtaskErrCb, 4096),
              "set ec create task conf");

    CHECK_LOG(original_doca_ec_task_recover_set_conf((*myEc)->mEc,
                                                     recoverSubtaskSuccCb,
                                                     recoverSubtaskErrCb, 4096),
              "set ec recover task conf");

    return DOCA_SUCCESS;
}

doca_error_t doca_ec_destroy(doca_ec *ec) {
    auto myEc = reinterpret_cast<Ec *>(ec);

    CHECK_LOG(original_doca_ec_destroy(myEc->mEc), "destroy ec");

    CHECK_LOG(doca_mmap_destroy(myEc->mMmap), "destroy mmap");
    free(myEc->mMemAddr);
    CHECK_LOG(doca_buf_inventory_destroy(myEc->mInv), "destroy buf inv");

    delete myEc;

    return DOCA_SUCCESS;
}

/* ec is Ec and we actually want to return Ctx
 * As Ec inherit Ctx, we can just return ec
 */
doca_ctx *doca_ec_as_ctx(doca_ec *ec) {
    auto myEc = reinterpret_cast<Ec *>(ec);

    myEc->mCtx = original_doca_ec_as_ctx(myEc->mEc);
    return reinterpret_cast<doca_ctx *>(ec);
}

doca_error_t doca_ec_matrix_create(doca_ec *ec, enum doca_ec_matrix_type type,
                                   size_t data_block_count,
                                   size_t rdnc_block_count,
                                   doca_ec_matrix **matrix) {
    auto myEc = reinterpret_cast<Ec *>(ec);
    auto myMat = reinterpret_cast<Matrix **>(matrix);
    *myMat = new Matrix{static_cast<u32>(data_block_count),
                        static_cast<u32>(rdnc_block_count)};

    return original_doca_ec_matrix_create(myEc->mEc, type, data_block_count,
                                          rdnc_block_count, &(*myMat)->mMat);
}

doca_error_t doca_ec_matrix_create_recover(doca_ec *ec,
                                           const doca_ec_matrix *coding_matrix,
                                           uint32_t missing_indices[],
                                           size_t n_missing,
                                           doca_ec_matrix **matrix) {
    auto myEc = reinterpret_cast<Ec *>(ec);
    auto myEncMat = reinterpret_cast<const Matrix *>(coding_matrix);
    auto myDecMat = reinterpret_cast<Matrix **>(matrix);

    *myDecMat = new Matrix{myEncMat->mNbDataBlks, myEncMat->mNbRdncBlks};
    return original_doca_ec_matrix_create_recover(myEc->mEc, myEncMat->mMat,
                                                  missing_indices, n_missing,
                                                  &(*myDecMat)->mMat);
}

doca_error_t doca_ec_matrix_destroy(doca_ec_matrix *matrix) {
    auto myMatrix = reinterpret_cast<Matrix *>(matrix);
    doca_error_t status = original_doca_ec_matrix_destroy(myMatrix->mMat);
    delete myMatrix;
    return status;
}

/* Task related */

doca_error_t doca_ec_task_create_set_conf(
    doca_ec *ec,
    doca_ec_task_create_completion_cb_t successful_task_completion_cb,
    doca_ec_task_create_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) {
    auto myEc = reinterpret_cast<Ec *>(ec);
    /* Register user cb to myEc and set subtask completion cb */
    myEc->mCreateSuccCb = successful_task_completion_cb;
    myEc->mCreateErrCb = error_task_completion_cb;
    return DOCA_SUCCESS;
}

doca_error_t doca_ec_task_create_allocate_init(
    doca_ec *ec, const doca_ec_matrix *coding_matrix,
    const doca_buf *original_data_blocks, doca_buf *rdnc_blocks,
    doca_data user_data, doca_ec_task_create **task) {
    auto myEc = reinterpret_cast<Ec *>(ec);
    auto myMat = reinterpret_cast<const Matrix *>(coding_matrix);
    auto mySrcBuf = reinterpret_cast<const Buf *>(original_data_blocks);
    auto myDstBuf = reinterpret_cast<Buf *>(rdnc_blocks);
    auto myTask = reinterpret_cast<EcTaskCreate **>(task);

    *myTask = new EcTaskCreate{myEc, myMat, mySrcBuf, myDstBuf, user_data};

    return DOCA_SUCCESS;
}

doca_task *doca_ec_task_create_as_task(doca_ec_task_create *task) {
    return reinterpret_cast<doca_task *>(task);
}

doca_error_t doca_ec_task_recover_set_conf(
    doca_ec *ec,
    doca_ec_task_recover_completion_cb_t successful_task_completion_cb,
    doca_ec_task_recover_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) {
    auto myEc = reinterpret_cast<Ec *>(ec);
    myEc->mRecoverSuccCb = successful_task_completion_cb;
    myEc->mRecoverErrCb = error_task_completion_cb;
    (void)num_tasks;
    return DOCA_SUCCESS;
}

doca_error_t doca_ec_task_recover_allocate_init(
    doca_ec *ec, const doca_ec_matrix *recover_matrix,
    const doca_buf *available_blocks, doca_buf *recovered_data_blocks,
    doca_data user_data, doca_ec_task_recover **task) {
    auto myEc = reinterpret_cast<Ec *>(ec);
    auto myMat = reinterpret_cast<const Matrix *>(recover_matrix);
    auto mySrcBuf = reinterpret_cast<const Buf *>(available_blocks);
    auto myDstBuf = reinterpret_cast<Buf *>(recovered_data_blocks);
    auto myTask = reinterpret_cast<EcTaskRecover **>(task);

    *myTask = new EcTaskRecover{myEc, myMat, mySrcBuf, myDstBuf, user_data};
    return DOCA_SUCCESS;
}

doca_task *doca_ec_task_recover_as_task(doca_ec_task_recover *task) {
    return reinterpret_cast<doca_task *>(task);
}