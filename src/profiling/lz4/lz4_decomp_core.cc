#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_types.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include <doca_compress.h>

#include "lz4_decomp.h"

DOCA_LOG_REGISTER(EC_CREATE : CORE);

/* Tasks will be free in the destructor */
void
lz4_decomp_success_cb(doca_compress_task_decompress_lz4_block* task,
                      doca_data task_user_data,
                      doca_data ctx_user_data)
{
    (void)task;
    (void)ctx_user_data;
    uint32_t* nb_finished_tasks = static_cast<uint32_t*>(task_user_data.ptr);
    (*nb_finished_tasks)++;
}
void
lz4_decomp_error_cb(doca_compress_task_decompress_lz4_block* task,
                    doca_data task_user_data,
                    doca_data ctx_user_data)
{
    (void)task;
    (void)ctx_user_data;
    uint32_t* nb_finished_tasks = static_cast<uint32_t*>(task_user_data.ptr);
    (*nb_finished_tasks)++;
    DOCA_LOG_ERR("LZ4 decomp task failed");
}

doca_error_t
lz4_decomp(const lz4_decomp_config& cfg)
{
    doca_error_t status;

    lz4_decomp_resources rscs;

    /* Open device */
    status = rscs.open_dev();
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to open device");
        return status;
    }

    /* Create pe */
    status = doca_pe_create(&rscs.pe);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create pe: %s", doca_error_get_descr(status));
        return status;
    }

    /* Create and config ec ctx */
    status = rscs.setup_comp_ctx(lz4_decomp_success_cb, lz4_decomp_error_cb);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to setup ag ctx");
        return status;
    }

    /* Setup mmap, buf inventory and bufs */
    status = rscs.prepare_memory(cfg);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to prepare bufs");
        return status;
    }

    /* Create and submit task */
    uint32_t nb_finished_tasks = 0;
    for (uint32_t i = 0; i < cfg.nb_tasks; i++) {
        doca_compress_task_decompress_lz4_block* task;
        status = doca_compress_task_decompress_lz4_block_alloc_init(
          rscs.comp,
          rscs.src_buf,
          rscs.dst_bufs[i],
          { .ptr = &nb_finished_tasks },
          &task);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to allocate and init decomp task: %s",
                         doca_error_get_descr(status));
            return status;
        }

        rscs.tasks.push_back(task);

        status = doca_task_submit_ex(
          doca_compress_task_decompress_lz4_block_as_task(task),
          DOCA_TASK_SUBMIT_FLAG_NONE);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to submit task: %s",
                         doca_error_get_descr(status));
            return status;
        }
    }

    auto begin_time = std::chrono::high_resolution_clock::now();
    doca_ctx_flush_tasks(rscs.ctx);

    while (nb_finished_tasks < cfg.nb_tasks)
        (void)doca_pe_progress(rscs.pe);

    auto end_time = std::chrono::high_resolution_clock::now();

    double time_cost_in_us =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end_time -
                                                           begin_time)
        .count() /
      (double)1000;
    DOCA_LOG_INFO("All tasks finished, input_size = %lu, per_task_time = %fus",
                  cfg.input_size,
                  time_cost_in_us / cfg.nb_tasks);

    return DOCA_SUCCESS;
}
