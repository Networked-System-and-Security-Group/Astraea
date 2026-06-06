#include "Dma.h"

#include <dlfcn.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include "Buf.h"
#include "Ctx.h"
#include "Ec.h"
#include "Pe.h"
#include "Task.h"
#include "common.h"
#include "doca_types.h"
#include "original.h"
#include "shm.h"

DOCA_LOG_REGISTER(ASTRAEA : DMA)

using namespace astraea;

/* Original function pointers */

doca_error_t (*original_doca_dma_create)(doca_dev *dev, doca_dma **dma) =
    reinterpret_cast<doca_error_t (*)(doca_dev *, doca_dma **)>(
        dlsym(RTLD_NEXT, "doca_dma_create"));

doca_error_t (*original_doca_dma_destroy)(doca_dma *dma) =
    reinterpret_cast<doca_error_t (*)(doca_dma *)>(dlsym(RTLD_NEXT,
                                                         "doca_dma_destroy"));

doca_error_t (*original_doca_dma_task_memcpy_set_conf)(
    doca_dma *dma, doca_dma_task_memcpy_completion_cb_t task_completion_cb,
    doca_dma_task_memcpy_completion_cb_t task_error_cb,
    uint32_t num_memcpy_tasks) =
    reinterpret_cast<
        doca_error_t (*)(doca_dma *, doca_dma_task_memcpy_completion_cb_t,
                         doca_dma_task_memcpy_completion_cb_t, uint32_t)>(
        dlsym(RTLD_NEXT, "doca_dma_task_memcpy_set_conf"));

doca_ctx *(*original_doca_dma_as_ctx)(doca_dma *dma) =
    reinterpret_cast<doca_ctx *(*)(doca_dma *)>(dlsym(RTLD_NEXT,
                                                      "doca_dma_as_ctx"));
doca_task *(*original_doca_dma_task_memcpy_as_task)(
    doca_dma_task_memcpy *task) =
    reinterpret_cast<doca_task *(*)(doca_dma_task_memcpy *)>(
        dlsym(RTLD_NEXT, "doca_dma_task_memcpy_as_task"));

doca_error_t (*original_doca_dma_task_memcpy_alloc_init)(
    doca_dma *dma, const doca_buf *src, doca_buf *dst, doca_data user_data,
    doca_dma_task_memcpy **task) =
    reinterpret_cast<doca_error_t (*)(doca_dma *, const doca_buf *, doca_buf *,
                                      doca_data, doca_dma_task_memcpy **)>(
        dlsym(RTLD_NEXT, "doca_dma_task_memcpy_alloc_init"));

void (*original_doca_dma_task_memcpy_set_src)(doca_dma_task_memcpy *task,
                                              const doca_buf *src) =
    reinterpret_cast<void (*)(doca_dma_task_memcpy *, const doca_buf *)>(
        dlsym(RTLD_NEXT, "doca_dma_task_memcpy_set_src"));

void (*original_doca_dma_task_memcpy_set_dst)(doca_dma_task_memcpy *task,
                                              doca_buf *dst) =
    reinterpret_cast<void (*)(doca_dma_task_memcpy *, doca_buf *)>(
        dlsym(RTLD_NEXT, "doca_dma_task_memcpy_set_dst"));

void (*original_doca_task_set_user_data)(doca_task *task, doca_data user_data) =
    reinterpret_cast<void (*)(doca_task *, doca_data)>(
        dlsym(RTLD_NEXT, "doca_task_set_user_data"));
////////////////////////////////////////////////////////////////////////////////

static doca_error_t submitNextMemcpySubTask(DmaTaskMemcpy *aRawTask);

doca_error_t Dma::start() {
    CHECK_LOG(original_doca_ctx_start(mCtx), "start ctx");

    auto tmpAddr = static_cast<u8 *>(mMemAddr);
    doca_buf *tmpSrcBuf = nullptr;
    doca_buf *tmpDstBuf = nullptr;
    CHECK_LOG(original_doca_buf_inventory_buf_get_by_args(
                  mInv, mMmap, tmpAddr, kDmaTmpBufSize / 2, tmpAddr,
                  kDmaTmpBufSize / 2, &tmpSrcBuf),
              "alloc tmp dma src buf");
    CHECK_LOG(original_doca_buf_inventory_buf_get_by_args(
                  mInv, mMmap, tmpAddr + kDmaTmpBufSize / 2, kDmaTmpBufSize / 2,
                  tmpAddr + kDmaTmpBufSize / 2, 0, &tmpDstBuf),
              "alloc tmp dma dst buf");

    for (u32 i = 0; i < kTaskQueueSize; i++) {
        CHECK_LOG(original_doca_dma_task_memcpy_alloc_init(
                      mDma, tmpSrcBuf, tmpDstBuf, {.ptr = &mUserDatas[i]},
                      &mMemcpyPool[i]),
                  "populate dma memcpy task pool");
    }

    CHECK_LOG(original_doca_buf_dec_refcount(tmpSrcBuf, nullptr),
              "dec tmp dma src buf ref count");
    CHECK_LOG(original_doca_buf_dec_refcount(tmpDstBuf, nullptr),
              "dec tmp dma dst buf ref count");

    return DOCA_SUCCESS;
}

doca_error_t Dma::stop() {
    if (mPe) {
        mPe->mIsStoppeds[mIsStoppedIdx] = true;
        not_empty_cv.notify_one();
        mPe->notifyWorker();
    }
    gSharedData->appDatas[gAppId].hasDma = 0;

    for (u32 i = 0; i < kTaskQueueSize; i++) {
        if (mMemcpyPool[i]) {
            doca_task *task =
                original_doca_dma_task_memcpy_as_task(mMemcpyPool[i]);
            original_doca_task_free(task);
            mMemcpyPool[i] = nullptr;
        }
    }

    return original_doca_ctx_stop(mCtx);
}

doca_error_t Dma::connectToPe(Pe *aPe) {
    if (gDmaSla == 0) {
        DOCA_LOG_ERR("DMA_SLA environment variable is not set");
        return DOCA_ERROR_INVALID_VALUE;
    }

    u32 idx = aPe->mLocks.size();
    aPe->mIsStoppeds.push_back(false);
    this->mPe = aPe;
    this->mIsStoppedIdx = idx;
    gSharedData->appDatas[gAppId].hasDma = 1;

    aPe->mCtxs.push_back(this);

    auto lock = new std::mutex;
    this->mLock = lock;
    aPe->mLocks.push_back(lock);

    CHECK_RETURN(original_doca_pe_connect_ctx(aPe->mPe, this->mCtx),
                 "connect pe to ctx");

    return DOCA_SUCCESS;
}

static u32 calMemcpyTimeCostPipeline(size_t aSize) {
    constexpr double kInterceptUs = 0.743571;
    constexpr double kSlopeUsPerByte = 0.000042000443;
    double cost = kInterceptUs + kSlopeUsPerByte * aSize;
    return static_cast<u32>(std::max(1.0, std::ceil(cost)));
}

static bool hasDmaContention() {
    const u32 nbApps = std::min(gSharedData->nbApps, kMaxNbApps);
    u32 nbDmaApps = 0;
    for (u32 i = 0; i < nbApps; i++) {
        if (gSharedData->appDatas[i].hasDma) {
            nbDmaApps++;
        }
    }
    return nbDmaApps > 1;
}

static void advanceDmaQueue(Dma *aDma) {
    uint32_t currentTail = aDma->mTail.load(std::memory_order_acquire);
    const auto currentHead = aDma->mHead.load(std::memory_order_relaxed);
    aDma->mHead.store((currentHead + 1) & kTaskQueueMask,
                      std::memory_order_release);
    if (((currentTail + 1) & kTaskQueueMask) == currentHead) {
        aDma->not_full_cv.notify_one();
    }
}

static inline doca_error_t submitMemcpySubTask(Dma *aDma, doca_buf *aSrcBuf,
                                               doca_buf *aDstBuf,
                                               DmaTaskMemcpy *aRawTask,
                                               u32 aSubtaskId, bool aIsSub,
                                               u32 aCost) {
    while (true) {
        const auto curTail = aDma->mTail.load(std::memory_order_relaxed);
        const auto nextTail = (curTail + 1) & kTaskQueueMask;
        const auto currentHead = aDma->mHead.load(std::memory_order_acquire);

        if (nextTail != currentHead) {
            UserData &userData = aDma->mUserDatas[curTail];

            userData.rawData = aRawTask->mUserData;
            userData.rawTask = aRawTask;
            userData.srcBuf = aSrcBuf;
            userData.dstBuf = aDstBuf;
            userData.isLast = aSubtaskId == aRawTask->mNbSubtasks - 1;
            userData.isSub = aIsSub;
            userData.stripId = aSubtaskId;

            doca_dma_task_memcpy *specTask = aDma->mMemcpyPool[curTail];
            doca_task *task = original_doca_dma_task_memcpy_as_task(specTask);
            original_doca_task_set_user_data(task, {.ptr = &userData});
            original_doca_dma_task_memcpy_set_src(specTask, aSrcBuf);
            original_doca_dma_task_memcpy_set_dst(specTask, aDstBuf);
            aDma->mSubTaskQ[curTail] = task;

            aDma->mTaskCosts[curTail] = aCost;
            aDma->mTail.store(nextTail, std::memory_order_release);
            aDma->not_empty_cv.notify_one();
            if (aDma->mPe) {
                aDma->mPe->notifyWorker();
            }

            break;
        } else {
            std::unique_lock<std::mutex> lock(aDma->mtx);
            aDma->not_full_cv.wait(lock, [&] {
                return ((aDma->mTail.load(std::memory_order_relaxed) + 1) &
                        kTaskQueueMask) !=
                       aDma->mHead.load(std::memory_order_relaxed);
            });
        }
    }

    return DOCA_SUCCESS;
}

static doca_error_t submitNextMemcpySubTask(DmaTaskMemcpy *aRawTask) {
    const u32 subtaskId = aRawTask->mNextSubtaskId++;
    const size_t offset = subtaskId * aRawTask->mSubtaskLen;
    const size_t chunkLen =
        std::min(aRawTask->mSubtaskLen, aRawTask->mDataLen - offset);
    bool isSub = aRawTask->mNbSubtasks > 1;

    doca_buf *srcBuf = nullptr;
    doca_buf *dstBuf = nullptr;
    if (isSub) {
        auto srcAddr = static_cast<u8 *>(aRawTask->mSrcBuf->mAddr) + offset;
        auto dstAddr = static_cast<u8 *>(aRawTask->mDstBuf->mAddr) + offset;
        CHECK_RETURN(original_doca_buf_inventory_buf_get_by_args(
                         aRawTask->mSrcBuf->mInv, aRawTask->mSrcBuf->mMmap,
                         srcAddr, chunkLen, srcAddr, chunkLen, &srcBuf),
                     "alloc dma sub src buf");
        CHECK_RETURN(original_doca_buf_inventory_buf_get_by_args(
                         aRawTask->mDstBuf->mInv, aRawTask->mDstBuf->mMmap,
                         dstAddr, chunkLen, dstAddr, 0, &dstBuf),
                     "alloc dma sub dst buf");
    } else {
        srcBuf = aRawTask->mSrcBuf->mBuf;
        dstBuf = aRawTask->mDstBuf->mBuf;
    }

    return submitMemcpySubTask(aRawTask->mDma, srcBuf, dstBuf, aRawTask,
                               subtaskId, isSub,
                               calMemcpyTimeCostPipeline(chunkLen));
}

doca_error_t DmaTaskMemcpy::submit() {
    mDataLen = mSrcBuf->mDataLen;
    size_t granularity = gSharedData->appDatas[gAppId].dmaGranularity;
    if (granularity == 0) {
        granularity = mDataLen == 0 ? 1 : mDataLen;
    }

    mNbSubtasks =
        hasDmaContention()
            ? static_cast<u32>((mDataLen + granularity - 1) / granularity)
            : 1;
    if (mNbSubtasks == 0) {
        mNbSubtasks = 1;
    }
    mSubtaskLen = mNbSubtasks == 1 ? mDataLen
                                   : (mDataLen + mNbSubtasks - 1) / mNbSubtasks;
    mNextSubtaskId = 0;
    mNbCompleted.store(0, std::memory_order_release);
    mHasError.store(false, std::memory_order_release);

    auto curTime = std::chrono::high_resolution_clock::now();
    mExpectTime = curTime + std::chrono::microseconds(gDmaSla);

    return submitNextMemcpySubTask(this);
}

void DmaTaskMemcpy::free() {}

static void memcpySubtaskComplete(doca_dma_task_memcpy *task,
                                  doca_data task_user_data,
                                  doca_data ctx_user_data, bool aHasError) {
    UserData *userData = static_cast<UserData *>(task_user_data.ptr);
    DmaTaskMemcpy *rawTask = static_cast<DmaTaskMemcpy *>(userData->rawTask);

    if (userData->isSub) {
        CHECK_LOG(original_doca_buf_dec_refcount(userData->srcBuf, nullptr),
                  "destroy dma sub src buf");
        CHECK_LOG(original_doca_buf_dec_refcount(userData->dstBuf, nullptr),
                  "destroy dma sub dst buf");
    }
    if (aHasError) {
        rawTask->mHasError.store(true, std::memory_order_release);
    }

    Dma *dma = rawTask->mDma;
    const bool isDone =
        aHasError || userData->isLast ||
        rawTask->mNbCompleted.fetch_add(1, std::memory_order_acq_rel) + 1 ==
            rawTask->mNbSubtasks;
    advanceDmaQueue(dma);

    if (!isDone) {
        CHECK_LOG(submitNextMemcpySubTask(rawTask), "submit next dma subtask");
        return;
    }

    const bool hasError = rawTask->mHasError.load(std::memory_order_acquire);
    if (hasError) {
        if (rawTask->mDma->mMemcpyErrCb) {
            rawTask->mDma->mMemcpyErrCb(
                reinterpret_cast<doca_dma_task_memcpy *>(rawTask),
                userData->rawData, ctx_user_data);
        }
    } else {
        rawTask->mDstBuf->mDataLen = rawTask->mDataLen;
        CHECK_LOG(original_doca_buf_set_data_len(rawTask->mDstBuf->mBuf,
                                                 rawTask->mDataLen),
                  "set raw dma dst len");

        auto curTime = std::chrono::high_resolution_clock::now();
        if (curTime > rawTask->mExpectTime) {
            LockHelper lockHelper;
            lockHelper.lock(gSharedData->appDatas[gAppId].dmaVioLock);
            gSharedData->appDatas[gAppId].dmaVioTimes++;
            lockHelper.unlock(gSharedData->appDatas[gAppId].dmaVioLock);
        }

        if (rawTask->mDma->mMemcpySuccCb) {
            rawTask->mDma->mMemcpySuccCb(
                reinterpret_cast<doca_dma_task_memcpy *>(rawTask),
                userData->rawData, ctx_user_data);
        }
    }
}

static void memcpySubtaskSuccCb(doca_dma_task_memcpy *task,
                                doca_data task_user_data,
                                doca_data ctx_user_data) {
    memcpySubtaskComplete(task, task_user_data, ctx_user_data, false);
}

static void memcpySubtaskErrCb(doca_dma_task_memcpy *task,
                               doca_data task_user_data,
                               doca_data ctx_user_data) {
    DOCA_LOG_ERR("DMA memcpy subtask failed");
    memcpySubtaskComplete(task, task_user_data, ctx_user_data, true);
}

doca_error_t doca_dma_create(doca_dev *dev, doca_dma **dma) {
    auto myDma = reinterpret_cast<Dma **>(dma);

    *myDma = new Dma;
    (*myDma)->mAccelKind = AccelKind::Dma;

    int ret = posix_memalign(&(*myDma)->mMemAddr, 64, kDmaTmpBufSize);
    if (ret) {
        DOCA_LOG_ERR("Failed to alloc dma tmp memory");
    }

    CHECK_LOG(doca_buf_inventory_create(2, &(*myDma)->mInv),
              "create dma buf inventory");
    CHECK_LOG(doca_buf_inventory_start((*myDma)->mInv),
              "start dma buf inventory");
    CHECK_LOG(doca_mmap_create(&(*myDma)->mMmap), "create dma internal mmap");
    CHECK_LOG(doca_mmap_add_dev((*myDma)->mMmap, dev),
              "add dev to dma internal mmap");
    CHECK_LOG(doca_mmap_set_memrange((*myDma)->mMmap, (*myDma)->mMemAddr,
                                     kDmaTmpBufSize),
              "set dma internal mmap memrange");
    CHECK_LOG(doca_mmap_start((*myDma)->mMmap), "start dma internal mmap");

    CHECK_LOG(original_doca_dma_create(dev, &(*myDma)->mDma), "create dma");

    return DOCA_SUCCESS;
}

doca_error_t doca_dma_destroy(doca_dma *dma) {
    auto myDma = reinterpret_cast<Dma *>(dma);

    CHECK_LOG(original_doca_dma_destroy(myDma->mDma), "destroy dma");
    CHECK_LOG(doca_mmap_destroy(myDma->mMmap), "destroy dma mmap");
    free(myDma->mMemAddr);
    CHECK_LOG(doca_buf_inventory_destroy(myDma->mInv), "destroy dma buf inv");

    delete myDma;

    return DOCA_SUCCESS;
}

doca_error_t doca_dma_task_memcpy_set_conf(
    doca_dma *dma, doca_dma_task_memcpy_completion_cb_t task_completion_cb,
    doca_dma_task_memcpy_completion_cb_t task_error_cb,
    uint32_t num_memcpy_tasks) {
    auto myDma = reinterpret_cast<Dma *>(dma);
    myDma->mMemcpySuccCb = task_completion_cb;
    myDma->mMemcpyErrCb = task_error_cb;
    return original_doca_dma_task_memcpy_set_conf(
        myDma->mDma, memcpySubtaskSuccCb, memcpySubtaskErrCb, num_memcpy_tasks);
}

doca_ctx *doca_dma_as_ctx(doca_dma *dma) {
    auto myDma = reinterpret_cast<Dma *>(dma);
    myDma->mCtx = original_doca_dma_as_ctx(myDma->mDma);
    return reinterpret_cast<doca_ctx *>(dma);
}

doca_task *doca_dma_task_memcpy_as_task(doca_dma_task_memcpy *task) {
    return reinterpret_cast<doca_task *>(task);
}

void doca_dma_task_memcpy_set_src(doca_dma_task_memcpy *task,
                                  const doca_buf *src) {
    auto myTask = reinterpret_cast<DmaTaskMemcpy *>(task);
    myTask->mSrcBuf = reinterpret_cast<const Buf *>(src);
}

const doca_buf *doca_dma_task_memcpy_get_src(const doca_dma_task_memcpy *task) {
    auto myTask = reinterpret_cast<const DmaTaskMemcpy *>(task);
    return reinterpret_cast<const doca_buf *>(myTask->mSrcBuf);
}

void doca_dma_task_memcpy_set_dst(doca_dma_task_memcpy *task, doca_buf *dst) {
    auto myTask = reinterpret_cast<DmaTaskMemcpy *>(task);
    myTask->mDstBuf = reinterpret_cast<Buf *>(dst);
}

doca_buf *doca_dma_task_memcpy_get_dst(const doca_dma_task_memcpy *task) {
    auto myTask = reinterpret_cast<const DmaTaskMemcpy *>(task);
    return reinterpret_cast<doca_buf *>(myTask->mDstBuf);
}

doca_error_t doca_dma_task_memcpy_alloc_init(doca_dma *dma, const doca_buf *src,
                                             doca_buf *dst, doca_data user_data,
                                             doca_dma_task_memcpy **task) {
    auto myDma = reinterpret_cast<Dma *>(dma);
    auto mySrcBuf = reinterpret_cast<const Buf *>(src);
    auto myDstBuf = reinterpret_cast<Buf *>(dst);
    auto myTask = reinterpret_cast<DmaTaskMemcpy **>(task);

    *myTask = new DmaTaskMemcpy{myDma, mySrcBuf, myDstBuf, user_data};

    return DOCA_SUCCESS;
}
