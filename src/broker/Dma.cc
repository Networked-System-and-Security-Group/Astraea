#include "Dma.h"

#include <dlfcn.h>
#include <doca_buf.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include "Buf.h"
#include "Pe.h"
#include "common.h"
#include "original.h"

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
////////////////////////////////////////////////////////////////////////////////
doca_error_t Dma::start() {
    CHECK_LOG(original_doca_ctx_start(mCtx), "start ctx");
    return DOCA_SUCCESS;
}

doca_error_t Dma::stop() { return original_doca_ctx_stop(mCtx); }

doca_error_t Dma::connectToPe(Pe *aPe) {
    CHECK_RETURN(original_doca_pe_connect_ctx(aPe->mPe, this->mCtx),
                 "connect pe to ctx");

    return DOCA_SUCCESS;
}

doca_error_t DmaTaskMemcpy::submit() {
    return original_doca_task_submit(
        original_doca_dma_task_memcpy_as_task(mTask));
}

void DmaTaskMemcpy::free() {
    original_doca_task_free(original_doca_dma_task_memcpy_as_task(mTask));
}

doca_error_t doca_dma_create(doca_dev *dev, doca_dma **dma) {
    auto myDma = reinterpret_cast<Dma **>(dma);

    *myDma = new Dma;

    CHECK_LOG(original_doca_dma_create(dev, &(*myDma)->mDma), "create dma");

    return DOCA_SUCCESS;
}

doca_error_t doca_dma_destroy(doca_dma *dma) {
    auto myDma = reinterpret_cast<Dma *>(dma);

    CHECK_LOG(original_doca_dma_destroy(myDma->mDma), "destroy dma");

    delete myDma;

    return DOCA_SUCCESS;
}

doca_error_t doca_dma_task_memcpy_set_conf(
    doca_dma *dma, doca_dma_task_memcpy_completion_cb_t task_completion_cb,
    doca_dma_task_memcpy_completion_cb_t task_error_cb,
    uint32_t num_memcpy_tasks) {
    auto myDma = reinterpret_cast<Dma *>(dma);
    return original_doca_dma_task_memcpy_set_conf(
        myDma->mDma, task_completion_cb, task_error_cb, num_memcpy_tasks);
}

doca_ctx *doca_dma_as_ctx(doca_dma *dma) {
    auto myDma = reinterpret_cast<Dma *>(dma);
    myDma->mCtx = original_doca_dma_as_ctx(myDma->mDma);
    return reinterpret_cast<doca_ctx *>(dma);
}

doca_task *doca_dma_task_memcpy_as_task(doca_dma_task_memcpy *task) {
    return reinterpret_cast<doca_task *>(task);
}

doca_error_t doca_dma_task_memcpy_alloc_init(doca_dma *dma, const doca_buf *src,
                                             doca_buf *dst, doca_data user_data,
                                             doca_dma_task_memcpy **task) {
    auto myDma = reinterpret_cast<Dma *>(dma);
    auto mySrcBuf = reinterpret_cast<const Buf *>(src);
    auto myDstBuf = reinterpret_cast<Buf *>(dst);
    auto myTask = reinterpret_cast<DmaTaskMemcpy **>(task);

    *myTask = new DmaTaskMemcpy;
    return original_doca_dma_task_memcpy_alloc_init(myDma->mDma, mySrcBuf->mBuf,
                                                    myDstBuf->mBuf, user_data,
                                                    &(*myTask)->mTask);
}
