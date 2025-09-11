#ifndef ORIGINAL_H_
#define ORIGINAL_H_

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_pe.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include <doca_erasure_coding.h>
#include <doca_rdma.h>

extern doca_error_t (*original_doca_buf_set_data_len)(doca_buf *buf,
                                                      size_t len);

extern doca_error_t (*original_doca_buf_dec_refcount)(doca_buf *buf,
                                                      uint16_t *refcount);

extern doca_error_t (*original_doca_buf_inventory_buf_get_by_args)(
    doca_buf_inventory *inventory, doca_mmap *mmap, void *addr, size_t len,
    void *data, size_t data_len, doca_buf **buf);

extern doca_error_t (*original_doca_ctx_start)(doca_ctx *ctx);
extern doca_error_t (*original_doca_ctx_stop)(doca_ctx *ctx);

extern uint8_t (*original_doca_pe_progress)(doca_pe *pe);
extern doca_error_t (*original_doca_pe_create)(doca_pe **pe);
extern doca_error_t (*original_doca_pe_destroy)(doca_pe *pe);
extern doca_error_t (*original_doca_pe_connect_ctx)(doca_pe *pe, doca_ctx *ctx);

extern doca_error_t (*original_doca_task_submit)(doca_task *task);
extern void (*original_doca_task_free)(doca_task *task);

#endif