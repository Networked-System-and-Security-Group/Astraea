#include "Rdma.h"

#include <dlfcn.h>
#include <doca_buf.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#include "Buf.h"
#include "Pe.h"
#include "common.h"
#include "original.h"

DOCA_LOG_REGISTER(ASTRAEA : RDMA)

using namespace astraea;

/* Original function pointers */
doca_error_t (*original_doca_rdma_create)(doca_dev *dev, doca_rdma **rdma) =
    reinterpret_cast<doca_error_t (*)(doca_dev *dev, doca_rdma **rdma)>(
        dlsym(RTLD_NEXT, "doca_rdma_create"));
doca_error_t (*original_doca_rdma_destroy)(doca_rdma *rdma) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *rdma)>(
        dlsym(RTLD_NEXT, "doca_rdma_destroy"));
doca_error_t (*original_doca_rdma_export)(
    doca_rdma *rdma, const void **local_rdma_conn_details,
    size_t *local_rdma_conn_details_size,
    doca_rdma_connection **rdma_connection) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *rdma,
                                      const void **local_rdma_conn_details,
                                      size_t *local_rdma_conn_details_size,
                                      doca_rdma_connection **rdma_connection)>(
        dlsym(RTLD_NEXT, "doca_rdma_export"));
doca_error_t (*original_doca_rdma_connect)(
    doca_rdma *rdma, const void *remote_rdma_conn_details,
    size_t remote_rdma_conn_details_size,
    doca_rdma_connection *rdma_connection) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *rdma,
                                      const void *remote_rdma_conn_details,
                                      size_t remote_rdma_conn_details_size,
                                      doca_rdma_connection *rdma_connection)>(
        dlsym(RTLD_NEXT, "doca_rdma_connect"));
doca_error_t (*original_doca_rdma_set_gid_index)(doca_rdma *rdma,
                                                 uint32_t gid_index) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *rdma, uint32_t gid_index)>(
        dlsym(RTLD_NEXT, "doca_rdma_set_gid_index"));
doca_error_t (*original_doca_rdma_set_max_num_connections)(
    doca_rdma *rdma, uint16_t max_num_connections) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *rdma,
                                      uint16_t max_num_connections)>(
        dlsym(RTLD_NEXT, "doca_rdma_set_max_num_connections"));
doca_error_t (*original_doca_rdma_set_permissions)(doca_rdma *rdma,
                                                   uint32_t permissions) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *rdma, uint32_t permissions)>(
        dlsym(RTLD_NEXT, "doca_rdma_set_permissions"));
doca_error_t (*original_doca_rdma_task_write_set_conf)(
    doca_rdma *rdma,
    doca_rdma_task_write_completion_cb_t successful_task_completion_cb,
    doca_rdma_task_write_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) =
    reinterpret_cast<doca_error_t (*)(
        doca_rdma *rdma,
        doca_rdma_task_write_completion_cb_t successful_task_completion_cb,
        doca_rdma_task_write_completion_cb_t error_task_completion_cb,
        uint32_t num_tasks)>(dlsym(RTLD_NEXT, "doca_rdma_task_write_set_conf"));

doca_error_t (*original_doca_rdma_task_write_imm_set_conf)(
    doca_rdma *rdma,
    doca_rdma_task_write_imm_completion_cb_t successful_task_completion_cb,
    doca_rdma_task_write_imm_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) =
    reinterpret_cast<
        doca_error_t (*)(doca_rdma *, doca_rdma_task_write_imm_completion_cb_t,
                         doca_rdma_task_write_imm_completion_cb_t, uint32_t)>(
        dlsym(RTLD_NEXT, "doca_rdma_task_write_imm_set_conf"));

doca_error_t (*original_doca_rdma_task_receive_set_conf)(
    doca_rdma *rdma,
    doca_rdma_task_receive_completion_cb_t successful_task_completion_cb,
    doca_rdma_task_receive_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) =
    reinterpret_cast<doca_error_t (*)(
        doca_rdma *rdma, doca_rdma_task_receive_completion_cb_t,
        doca_rdma_task_receive_completion_cb_t, uint32_t)>(
        dlsym(RTLD_NEXT, "doca_rdma_task_receive_set_conf"));

doca_ctx *(*original_doca_rdma_as_ctx)(doca_rdma *rdma) =
    reinterpret_cast<doca_ctx *(*)(doca_rdma *)>(dlsym(RTLD_NEXT,
                                                       "doca_rdma_as_ctx"));

doca_task *(*original_doca_rdma_task_write_as_task)(
    doca_rdma_task_write *task) =
    reinterpret_cast<doca_task *(*)(doca_rdma_task_write *)>(
        dlsym(RTLD_NEXT, "doca_rdma_task_write_as_task"));

doca_task *(*original_doca_rdma_task_write_imm_as_task)(
    doca_rdma_task_write_imm *task) =
    reinterpret_cast<doca_task *(*)(doca_rdma_task_write_imm *task)>(
        dlsym(RTLD_NEXT, "doca_rdma_task_write_imm_as_task"));

doca_task *(*original_doca_rdma_task_receive_as_task)(
    doca_rdma_task_receive *task) =
    reinterpret_cast<doca_task *(*)(doca_rdma_task_receive *)>(
        dlsym(RTLD_NEXT, "doca_rdma_task_receive_as_task"));

doca_error_t (*original_doca_rdma_task_write_allocate_init)(
    doca_rdma *, doca_rdma_connection *, const doca_buf *, doca_buf *,
    doca_data, doca_rdma_task_write **) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *, doca_rdma_connection *,
                                      const doca_buf *, doca_buf *, doca_data,
                                      doca_rdma_task_write **)>(
        dlsym(RTLD_NEXT, "doca_rdma_task_write_allocate_init"));

doca_error_t (*original_doca_rdma_task_write_imm_allocate_init)(
    struct doca_rdma *rdma, doca_rdma_connection *rdma_connection,
    const doca_buf *src_buf, doca_buf *dst_buf, doca_be32_t immediate_data,
    doca_data user_data, doca_rdma_task_write_imm **task) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *, doca_rdma_connection *,
                                      const doca_buf *, doca_buf *, doca_be32_t,
                                      doca_data, doca_rdma_task_write_imm **)>(
        dlsym(RTLD_NEXT, "doca_rdma_task_write_imm_allocate_init"));

doca_error_t (*original_doca_rdma_task_receive_allocate_init)(
    doca_rdma *rdma, doca_buf *dst_buf, doca_data user_data,
    doca_rdma_task_receive **task) =
    reinterpret_cast<doca_error_t (*)(doca_rdma *, doca_buf *, doca_data,
                                      doca_rdma_task_receive **)>(
        dlsym(RTLD_NEXT, "doca_rdma_task_receive_allocate_init"));

////////////////////////////////////////////////////////////////////////////////
doca_error_t Rdma::start() {
    CHECK_LOG(original_doca_ctx_start(mCtx), "start ctx");
    return DOCA_SUCCESS;
}

doca_error_t Rdma::stop() { return original_doca_ctx_stop(mCtx); }

doca_error_t Rdma::connectToPe(Pe *aPe) {
    CHECK_RETURN(original_doca_pe_connect_ctx(aPe->mPe, this->mCtx),
                 "connect pe to ctx");

    return DOCA_SUCCESS;
}

doca_error_t RdmaTaskWrite::submit() {
    return original_doca_task_submit(
        original_doca_rdma_task_write_as_task(mTask));
}

doca_error_t RdmaTaskWriteImm::submit() {
    return original_doca_task_submit(
        original_doca_rdma_task_write_imm_as_task(mTask));
}

doca_error_t RdmaTaskRecv::submit() {
    return original_doca_task_submit(
        original_doca_rdma_task_receive_as_task(mTask));
}

void RdmaTaskWrite::free() {
    original_doca_task_free(original_doca_rdma_task_write_as_task(mTask));
}

void RdmaTaskWriteImm::free() {
    original_doca_task_free(original_doca_rdma_task_write_imm_as_task(mTask));
}

void RdmaTaskRecv::free() {
    original_doca_task_free(original_doca_rdma_task_receive_as_task(mTask));
}

doca_error_t doca_rdma_create(doca_dev *dev, doca_rdma **rdma) {
    auto myRdma = reinterpret_cast<Rdma **>(rdma);

    *myRdma = new Rdma;

    CHECK_LOG(original_doca_rdma_create(dev, &(*myRdma)->mRdma), "create rdma");

    return DOCA_SUCCESS;
}

doca_error_t doca_rdma_destroy(doca_rdma *rdma) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);

    CHECK_LOG(original_doca_rdma_destroy(myRdma->mRdma), "destroy ec");

    delete myRdma;

    return DOCA_SUCCESS;
}

doca_error_t doca_rdma_export(doca_rdma *rdma,
                              const void **local_rdma_conn_details,
                              size_t *local_rdma_conn_details_size,
                              doca_rdma_connection **rdma_connection) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    return original_doca_rdma_export(myRdma->mRdma, local_rdma_conn_details,
                                     local_rdma_conn_details_size,
                                     rdma_connection);
}

doca_error_t doca_rdma_connect(doca_rdma *rdma,
                               const void *remote_rdma_conn_details,
                               size_t remote_rdma_conn_details_size,
                               doca_rdma_connection *rdma_connection) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    return original_doca_rdma_connect(myRdma->mRdma, remote_rdma_conn_details,
                                      remote_rdma_conn_details_size,
                                      rdma_connection);
}

doca_error_t doca_rdma_set_gid_index(doca_rdma *rdma, uint32_t gid_index) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    return original_doca_rdma_set_gid_index(myRdma->mRdma, gid_index);
}

doca_error_t doca_rdma_set_max_num_connections(doca_rdma *rdma,
                                               uint16_t max_num_connections) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    return original_doca_rdma_set_max_num_connections(myRdma->mRdma,
                                                      max_num_connections);
}

doca_error_t doca_rdma_set_permissions(doca_rdma *rdma, uint32_t permissions) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    return original_doca_rdma_set_permissions(myRdma->mRdma, permissions);
}

doca_error_t doca_rdma_task_write_set_conf(
    doca_rdma *rdma,
    doca_rdma_task_write_completion_cb_t successful_task_completion_cb,
    doca_rdma_task_write_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    return original_doca_rdma_task_write_set_conf(
        myRdma->mRdma, successful_task_completion_cb, error_task_completion_cb,
        num_tasks);
}

doca_error_t doca_rdma_task_write_imm_set_conf(
    doca_rdma *rdma,
    doca_rdma_task_write_imm_completion_cb_t successful_task_completion_cb,
    doca_rdma_task_write_imm_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    return original_doca_rdma_task_write_imm_set_conf(
        myRdma->mRdma, successful_task_completion_cb, error_task_completion_cb,
        num_tasks);
}

doca_error_t doca_rdma_task_receive_set_conf(
    doca_rdma *rdma,
    doca_rdma_task_receive_completion_cb_t successful_task_completion_cb,
    doca_rdma_task_receive_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    return original_doca_rdma_task_receive_set_conf(
        myRdma->mRdma, successful_task_completion_cb, error_task_completion_cb,
        num_tasks);
}

doca_ctx *doca_rdma_as_ctx(doca_rdma *rdma) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    myRdma->mCtx = original_doca_rdma_as_ctx(myRdma->mRdma);
    return reinterpret_cast<doca_ctx *>(rdma);
}

doca_task *doca_rdma_task_write_as_task(doca_rdma_task_write *task) {
    return reinterpret_cast<doca_task *>(task);
}

doca_task *doca_rdma_task_receive_as_task(doca_rdma_task_receive *task) {
    return reinterpret_cast<doca_task *>(task);
}

doca_task *doca_rdma_task_write_imm_as_task(doca_rdma_task_write_imm *task) {
    return reinterpret_cast<doca_task *>(task);
}

doca_error_t doca_rdma_task_write_allocate_init(
    doca_rdma *rdma, doca_rdma_connection *rdma_connection,
    const doca_buf *src_buf, doca_buf *dst_buf, doca_data user_data,
    doca_rdma_task_write **task) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    auto mySrcBuf = reinterpret_cast<const Buf *>(src_buf);
    auto myDstBuf = reinterpret_cast<Buf *>(dst_buf);
    auto myTask = reinterpret_cast<RdmaTaskWrite **>(task);

    *myTask = new RdmaTaskWrite;

    return original_doca_rdma_task_write_allocate_init(
        myRdma->mRdma, rdma_connection, mySrcBuf->mBuf, myDstBuf->mBuf,
        user_data, &(*myTask)->mTask);
}

doca_error_t doca_rdma_task_write_imm_allocate_init(
    doca_rdma *rdma, doca_rdma_connection *rdma_connection,
    const doca_buf *src_buf, doca_buf *dst_buf, doca_be32_t immediate_data,
    doca_data user_data, doca_rdma_task_write_imm **task) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    auto mySrcBuf = reinterpret_cast<const Buf *>(src_buf);
    auto myDstBuf = reinterpret_cast<Buf *>(dst_buf);
    auto myTask = reinterpret_cast<RdmaTaskWriteImm **>(task);

    *myTask = new RdmaTaskWriteImm;
    return original_doca_rdma_task_write_imm_allocate_init(
        myRdma->mRdma, rdma_connection, mySrcBuf->mBuf, myDstBuf->mBuf,
        immediate_data, user_data, &(*myTask)->mTask);
}

doca_error_t doca_rdma_task_receive_allocate_init(
    doca_rdma *rdma, doca_buf *dst_buf, doca_data user_data,
    doca_rdma_task_receive **task) {
    auto myRdma = reinterpret_cast<Rdma *>(rdma);
    // dst_buf can be nullptr
    auto myDstBuf = reinterpret_cast<Buf *>(dst_buf);
    auto myTask = reinterpret_cast<RdmaTaskRecv **>(task);

    *myTask = new RdmaTaskRecv;

    return original_doca_rdma_task_receive_allocate_init(
        myRdma->mRdma, myDstBuf ? myDstBuf->mBuf : nullptr, user_data,
        &(*myTask)->mTask);
}