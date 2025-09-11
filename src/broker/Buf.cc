#include <cstdint>
#include <doca_error.h>
#include <doca_log.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>

#include <dlfcn.h>

#include "Buf.h"

DOCA_LOG_REGISTER(ASTRAEA : BUF)

using namespace astraea;

/* Original function pointers */
doca_error_t (*original_doca_buf_set_data_len)(doca_buf *buf, size_t len) =
    reinterpret_cast<doca_error_t (*)(doca_buf *, size_t)>(
        dlsym(RTLD_NEXT, "doca_buf_set_data_len"));

doca_error_t (*original_doca_buf_dec_refcount)(doca_buf *buf,
                                               uint16_t *refcount) =
    reinterpret_cast<doca_error_t (*)(doca_buf *, uint16_t *)>(
        dlsym(RTLD_NEXT, "doca_buf_dec_refcount"));

doca_error_t (*original_doca_buf_inventory_buf_get_by_args)(
    doca_buf_inventory *inventory, doca_mmap *mmap, void *addr, size_t len,
    void *data, size_t data_len, doca_buf **buf) =
    reinterpret_cast<doca_error_t (*)(doca_buf_inventory *, doca_mmap *, void *,
                                      size_t, void *, size_t, doca_buf **)>(
        dlsym(RTLD_NEXT, "doca_buf_inventory_buf_get_by_args"));
////////////////////////////////////////////////////////////////////////////////

/* Re-implementations */
doca_error_t doca_buf_set_data_len(doca_buf *buf, size_t data_len) {
    auto myBuf = reinterpret_cast<Buf *>(buf);
    return original_doca_buf_set_data_len(myBuf->mBuf, data_len);
}

doca_error_t doca_buf_dec_refcount(doca_buf *buf, uint16_t *refcount) {
    auto myBuf = reinterpret_cast<Buf *>(buf);
    uint16_t cnt = 0;

    doca_error_t status = original_doca_buf_dec_refcount(myBuf->mBuf, &cnt);

    if (cnt == 0) {
        delete myBuf;
    }

    if (refcount) {
        *refcount = cnt;
    }

    return status;
}

doca_error_t doca_buf_inventory_buf_get_by_args(doca_buf_inventory *inventory,
                                                doca_mmap *mmap, void *addr,
                                                size_t len, void *data,
                                                size_t data_len,
                                                doca_buf **buf) {
    Buf **myBuf = reinterpret_cast<Buf **>(buf);

    *myBuf = new Buf;

    (*myBuf)->mInv = inventory;
    (*myBuf)->mMmap = mmap;
    (*myBuf)->mAddr = addr;
    (*myBuf)->mBufLen = len;
    (*myBuf)->mDataLen = data_len;

    return original_doca_buf_inventory_buf_get_by_args(
        inventory, mmap, addr, len, data, data_len, &(*myBuf)->mBuf);
}