#pragma once

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_types.h>

#include <cstddef>

#include "Ctx.h"
#include "Task.h"
#include "common.h"

constexpr size_t kPerBufSize = 128 * 32768;
constexpr size_t kBufPoolSize = 128;
constexpr size_t kBufPoolMask = kBufPoolSize - 1;

/* Forward declarations */
namespace astraea {
class Buf;
}  // namespace astraea

namespace astraea {
class Ec : public Ctx {
   public:
    doca_ec *mEc = nullptr;
    doca_ec_task_create_completion_cb_t mCreateSuccCb = nullptr;
    doca_ec_task_create_completion_cb_t mCreateErrCb = nullptr;
    doca_ec_task_recover_completion_cb_t mRecoverSuccCb = nullptr;
    doca_ec_task_recover_completion_cb_t mRecoverErrCb = nullptr;

    doca_buf_inventory *mInv = nullptr;
    /* This is actually Mmap* */
    doca_mmap *mMmap = nullptr;
    void *mMemAddr = nullptr;

    /* Subtask queue related variables */

    doca_ec_task_create *mCreatePool[kTaskQueueSize] = {nullptr};
    doca_ec_task_recover *mRecoverPool[kTaskQueueSize] = {nullptr};
    doca_buf *mDstBufPool[kBufPoolSize] = {nullptr};
    u32 mCurDstBufId = 0;

    doca_buf *getPooledDstBuf() {
        doca_buf *curBuf = mDstBufPool[mCurDstBufId];
        mCurDstBufId = (mCurDstBufId + 1) & kBufPoolMask;
        return curBuf;
    }

    doca_error_t start() override;
    doca_error_t stop() override;
    doca_error_t connectToPe(Pe *aPe) override;
};

class Matrix {
   public:
    doca_ec_matrix *mMat = nullptr;
    u32 mNbDataBlks, mNbRdncBlks;

    Matrix(u32 aNbDataBlks, u32 aNbRdncBlks)
        : mNbDataBlks(aNbDataBlks), mNbRdncBlks(aNbRdncBlks) {}
};

class EcTaskCreate : public Task {
   public:
    Ec *mEc;
    const Matrix *mMat;

    const Buf *mSrcBuf;
    Buf *mDstBuf;

    doca_data mUserData;

    EcTaskCreate(Ec *aEc, const Matrix *aMat, const Buf *aSrcBuf, Buf *aDstBuf,
                 doca_data aUserData)
        : mEc(aEc),
          mMat(aMat),
          mSrcBuf(aSrcBuf),
          mDstBuf(aDstBuf),
          mUserData(aUserData) {}

    doca_error_t submit() override;
    void free() override;
};

class EcTaskRecover : public Task {
   public:
    Ec *mEc;
    const Matrix *mMat;

    const Buf *mSrcBuf;
    Buf *mDstBuf;

    doca_data mUserData;

    EcTaskRecover(Ec *aEc, const Matrix *aMat, const Buf *aSrcBuf, Buf *aDstBuf,
                  doca_data aUserData)
        : mEc(aEc),
          mMat(aMat),
          mSrcBuf(aSrcBuf),
          mDstBuf(aDstBuf),
          mUserData(aUserData) {}
    doca_error_t submit() override;
    void free() override;
};

}  // namespace astraea