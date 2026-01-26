#ifndef ASTRAEA_BUF_H_
#define ASTRAEA_BUF_H_

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_error.h>
#include <doca_mmap.h>

#include <cstddef>

namespace astraea {
class Buf {
   public:
    doca_buf *mBuf = nullptr;
    doca_buf_inventory *mInv = nullptr;
    doca_mmap *mMmap = nullptr;
    void *mAddr = nullptr;
    size_t mBufLen = 0;
    size_t mDataLen = 0;
};
}  // namespace astraea
#endif