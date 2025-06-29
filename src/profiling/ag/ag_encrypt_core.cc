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

#include <doca_aes_gcm.h>

#include "ag_encrypt.h"

DOCA_LOG_REGISTER(EC_CREATE : CORE);

/* Tasks will be free in the destructor */
void
ag_encrypt_success_cb(doca_aes_gcm_task_encrypt* task,
                      doca_data task_user_data,
                      doca_data ctx_user_data)
{
    (void)task;
    (void)ctx_user_data;
    uint32_t* nb_finished_tasks = static_cast<uint32_t*>(task_user_data.ptr);
    (*nb_finished_tasks)++;
}
void
ag_encrypt_error_cb(doca_aes_gcm_task_encrypt* task,
                    doca_data task_user_data,
                    doca_data ctx_user_data)
{
    (void)task;
    (void)ctx_user_data;
    uint32_t* nb_finished_tasks = static_cast<uint32_t*>(task_user_data.ptr);
    (*nb_finished_tasks)++;
    DOCA_LOG_ERR("AG encrypt task failed");
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
    status = rscs.setup_ag_ctx(ag_encrypt_success_cb, ag_encrypt_error_cb);
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
    status = doca_aes_gcm_key_create(
      rscs.ag, DEFAULT_KEY, DOCA_AES_GCM_KEY_128, &rscs.key);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create ag key: %s",
                     doca_error_get_descr(status));
        return status;
    }

    uint32_t nb_finished_tasks = 0;
    for (uint32_t i = 0; i < cfg.nb_tasks; i++) {
        doca_aes_gcm_task_encrypt* task;
        status =
          doca_aes_gcm_task_encrypt_alloc_init(rscs.ag,
                                               rscs.src_buf,
                                               rscs.dst_bufs[i],
                                               rscs.key,
                                               DEFAULT_IV,
                                               DEFAULT_IV_SIZE_IN_BYTES,
                                               DEFAULT_TAG_SIZE_IN_BYTES,
                                               cfg.aad_size,
                                               { .ptr = &nb_finished_tasks },
                                               &task);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to allocate and init ag task: %s",
                         doca_error_get_descr(status));
            return status;
        }

        rscs.tasks.push_back(task);

        status = doca_task_submit_ex(doca_aes_gcm_task_encrypt_as_task(task),
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
    DOCA_LOG_INFO("All tasks finished, nb_tasks = %u, all_task_time = %fus, "
                  "plaintext_size = %u, aad_size = "
                  "%u, per_task_time = %fus",
                  cfg.nb_tasks,
                  time_cost_in_us,
                  cfg.plaintext_size,
                  cfg.aad_size,
                  time_cost_in_us / cfg.nb_tasks);

    return DOCA_SUCCESS;
}
