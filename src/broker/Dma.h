#pragma once

#include <doca_buf.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_types.h>

#include <atomic>
#include <cstddef>

#include "Ctx.h"
#include "Task.h"

namespace astraea {
class Buf;

class Dma : public Ctx {
   public:
    doca_dma *mDma = nullptr;
    doca_dma_task_memcpy_completion_cb_t mMemcpySuccCb = nullptr;
    doca_dma_task_memcpy_completion_cb_t mMemcpyErrCb = nullptr;

    doca_error_t start() override;
    doca_error_t stop() override;
    doca_error_t connectToPe(Pe *aPe) override;
};

class DmaTaskMemcpy : public Task {
   public:
    Dma *mDma = nullptr;
    const Buf *mSrcBuf = nullptr;
    Buf *mDstBuf = nullptr;
    doca_data mUserData = {};

    size_t mDataLen = 0;
    size_t mSubtaskLen = 0;
    u32 mNbSubtasks = 0;
    u32 mNextSubtaskId = 0;
    std::atomic<u32> mNbCompleted = 0;
    std::atomic<bool> mHasError = false;

    DmaTaskMemcpy(Dma *aDma, const Buf *aSrcBuf, Buf *aDstBuf,
                  doca_data aUserData)
        : mDma(aDma),
          mSrcBuf(aSrcBuf),
          mDstBuf(aDstBuf),
          mUserData(aUserData) {}

    doca_error_t submit() override;
    void free() override;
};
}  // namespace astraea
