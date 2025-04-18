#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_types.h>

#include "astraea_ctx.h"
#include "astraea_ec.h"
#include "astraea_pe.h"
#include "resource_mgmt.h"

DOCA_LOG_REGISTER(ASTRAEA : EC);

extern sem_t *ec_deficit_sem;
extern shared_resources *shm_data;
extern uint32_t app_id;
extern sem_t *ec_token_sem;

extern bool has_finished_task;

void subtask_success_cb(doca_ec_task_create *task, doca_data task_user_data,
                        doca_data ctx_user_data) {
    (void)ctx_user_data;
    const _astraea_ec_subtask_create_user_data *user_data =
        static_cast<_astraea_ec_subtask_create_user_data *>(task_user_data.ptr);

    if (user_data->is_sub) {
        const doca_buf *sub_dst_buf = doca_ec_task_create_get_rdnc_blocks(task);
        uint8_t *dst_data;
        doca_buf_get_data(sub_dst_buf, (void **)&dst_data);

        const size_t origin_block_size =
            user_data->origin_task->origin_block_size;
        const size_t sub_block_size = user_data->origin_task->sub_block_size;
        const uint32_t nb_rdnc_blocks =
            user_data->origin_task->matrix->nb_rdnc_blocks;
        uint8_t *dst_base_addr =
            static_cast<uint8_t *>(user_data->origin_task->dst_base_addr);

        for (uint32_t i = 0; i < nb_rdnc_blocks; i++) {
            memcpy(dst_base_addr + i * origin_block_size +
                       user_data->strip_id * sub_block_size,
                   dst_data + i * sub_block_size, sub_block_size);
        }
    }

    if (user_data->is_last) {
        auto cur_time = std::chrono::high_resolution_clock::now();
        if (cur_time > user_data->origin_task->expected_time) {
            if (sem_wait(ec_deficit_sem)) {
                DOCA_LOG_ERR("Failed to get ec_token_sem");
                return;
            }

            shm_data->deficits[app_id]++;

            if (sem_post(ec_deficit_sem)) {
                DOCA_LOG_ERR("Failed to post ec_token_sem");
                return;
            }
        }

        user_data->origin_task->ec->success_cb(
            user_data->origin_task, user_data->origin_task->user_data,
            {.u64 = 0});
        has_finished_task = true;
    }
}

void subtask_error_cb(doca_ec_task_create *task, doca_data task_user_data,
                      doca_data ctx_user_data) {
    (void)ctx_user_data;
    _astraea_ec_subtask_create_user_data *user_data =
        static_cast<_astraea_ec_subtask_create_user_data *>(task_user_data.ptr);

    if (user_data->is_last) {
        user_data->origin_task->ec->error_cb(user_data->origin_task,
                                             user_data->origin_task->user_data,
                                             {.u64 = 0});
        has_finished_task = true;
    }
}

doca_error_t astraea_ec_create(doca_dev *dev, astraea_ec **ec) {
    astraea_ec *new_ec = new astraea_ec;
    *ec = nullptr;

    new_ec->dev = dev;

    doca_error_t status = doca_ec_create(dev, &new_ec->ec);
    if (status != DOCA_SUCCESS) {
        delete new_ec;
        return status;
    }

    int ret =
        posix_memalign(&new_ec->tmp_rdnc_buffer, 64, TMP_RDNC_BUFFER_SIZE);
    if (ret) {
        DOCA_LOG_ERR("Failed to alloc memory");
        doca_ec_destroy(new_ec->ec);
        delete new_ec;
        return status;
    }
    status = doca_mmap_create(&new_ec->dst_mmap);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create dst mmap: %s",
                     doca_error_get_descr(status));
        doca_ec_destroy(new_ec->ec);
        delete new_ec;
        return status;
    }

    status = doca_mmap_add_dev(new_ec->dst_mmap, dev);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to add dev to dst mmap: %s",
                     doca_error_get_descr(status));
        doca_mmap_destroy(new_ec->dst_mmap);
        doca_ec_destroy(new_ec->ec);
        delete new_ec;
        return status;
    }

    status = doca_mmap_set_memrange(new_ec->dst_mmap, new_ec->tmp_rdnc_buffer,
                                    TMP_RDNC_BUFFER_SIZE);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set mmap memrange: %s",
                     doca_error_get_descr(status));
        doca_mmap_destroy(new_ec->dst_mmap);
        doca_ec_destroy(new_ec->ec);
        delete new_ec;
        return status;
    }

    status = doca_mmap_start(new_ec->dst_mmap);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start dst mmap: %s",
                     doca_error_get_descr(status));
        doca_mmap_destroy(new_ec->dst_mmap);
        doca_ec_destroy(new_ec->ec);
        delete new_ec;
        return status;
    }

    status = doca_buf_inventory_create(MAX_NB_CTX_BUFS, &new_ec->buf_inventory);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create buf inventory: %s",
                     doca_error_get_descr(status));
        doca_mmap_destroy(new_ec->dst_mmap);
        doca_ec_destroy(new_ec->ec);
        delete new_ec;
        return status;
    }

    status = doca_buf_inventory_start(new_ec->buf_inventory);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start buf inventory: %s",
                     doca_error_get_descr(status));
        doca_buf_inventory_destroy(new_ec->buf_inventory);
        doca_mmap_destroy(new_ec->dst_mmap);
        doca_ec_destroy(new_ec->ec);
        delete new_ec;
        return status;
    }

    new_ec->cur_task_pos = 0;
    for (uint32_t i = 0; i < MAX_NB_INFLIGHT_EC_TASKS; i++) {
        new_ec->task_pool[i] = new astraea_ec_task_create;
        new_ec->task_pool[i]->cur_subtask_pos = 0;
        new_ec->task_pool[i]->is_free = false;
        for (uint32_t j = 0; j < MAX_NB_SUBTASKS_PER_TASK; j++) {
            new_ec->task_pool[i]->subtask_pool[j] =
                new _astraea_ec_subtask_create;
            new_ec->task_pool[i]->subtask_pool[j]->user_data =
                new _astraea_ec_subtask_create_user_data;
            new_ec->task_pool[i]->subtask_pool[j]->task = nullptr;
        }
    }

    *ec = new_ec;

    return DOCA_SUCCESS;
}

doca_error_t astraea_ec_destroy(astraea_ec *ec) {
    for (uint32_t i = 0; i < MAX_NB_INFLIGHT_EC_TASKS; i++) {
        astraea_ec_task_create *task = ec->task_pool[i];
        for (uint32_t j = 0; j < MAX_NB_SUBTASKS_PER_TASK; j++) {
            _astraea_ec_subtask_create *subtask = task->subtask_pool[j];
            /* DOCA tasks are free in astraea_ctx_stop */
            delete subtask->user_data;
            delete subtask;
        }
        for (std::pair<doca_buf *, doca_buf *> sub_buf_pair :
             task->sub_buf_pairs) {
            doca_buf_dec_refcount(sub_buf_pair.first, nullptr);
            doca_buf_dec_refcount(sub_buf_pair.second, nullptr);
        }

        delete task;
    }
    doca_error_t status;
    status = doca_ec_destroy(ec->ec);
    status = doca_buf_inventory_destroy(ec->buf_inventory);
    status = doca_mmap_destroy(ec->dst_mmap);
    free(ec->tmp_rdnc_buffer);

    delete ec;

    return status;
}

astraea_ctx *astraea_ec_as_ctx(astraea_ec *ec) {
    astraea_ctx *ctx = new astraea_ctx;

    ctx->ctx = doca_ec_as_ctx(ec->ec);
    if (ctx->ctx == nullptr) {
        delete ctx;
        return nullptr;
    }
    ctx->type = EC;
    ctx->ec = ec;
    ctx->submitter = nullptr;

    return ctx;
}

doca_error_t astraea_ec_task_create_set_conf(
    astraea_ec *ec,
    astraea_ec_task_create_completion_cb_t successful_task_completion_cb,
    astraea_ec_task_create_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks) {
    (void)num_tasks;
    ec->success_cb = successful_task_completion_cb;
    ec->error_cb = error_task_completion_cb;
    return doca_ec_task_create_set_conf(
        ec->ec, subtask_success_cb, subtask_error_cb, MAX_NB_INFLIGHT_EC_TASKS);
}

static inline uint32_t calc_token_cost(uint32_t nb_data_blocks,
                                       uint32_t nb_rdnc_blocks,
                                       size_t block_size) {
    if (nb_data_blocks == 128 && nb_rdnc_blocks == 32 && block_size == 1024) {
        return 1;
    }

    if (nb_data_blocks == 128 && nb_rdnc_blocks == 32 &&
        block_size == 1024 * 1024) {
        return 1024;
    }
    return 1;
}

/**
 * This function should be modify later!
 * The granularity should be calculated according to real time metadata
 * Which must incurs semaphore operations
 */
static size_t calc_granularity(astraea_ec_task_create *task) {
    size_t granularity = 0;

    uint32_t token_cost =
        calc_token_cost(task->matrix->nb_data_blocks,
                        task->matrix->nb_rdnc_blocks, task->origin_block_size);

    if (sem_wait(ec_token_sem)) {
        DOCA_LOG_ERR("Failed to get ec_token_sem");
        return 1024;
    }

    const uint32_t nb_avail_tokens = shm_data->ec_tokens[app_id];

    if (token_cost < nb_avail_tokens) {
        granularity = task->origin_block_size;
    } else {
        granularity = nb_avail_tokens < 2      ? 1 * 1024
                      : nb_avail_tokens < 4    ? 2 * 1024
                      : nb_avail_tokens < 8    ? 4 * 1024
                      : nb_avail_tokens < 16   ? 8 * 1024
                      : nb_avail_tokens < 32   ? 16 * 1024
                      : nb_avail_tokens < 64   ? 32 * 1024
                      : nb_avail_tokens < 128  ? 64 * 1024
                      : nb_avail_tokens < 256  ? 128 * 1024
                      : nb_avail_tokens < 512  ? 256 * 1024
                      : nb_avail_tokens < 1024 ? 512 * 1024
                                               : 1024 * 1024;
    }

    if (sem_post(ec_token_sem)) {
        DOCA_LOG_ERR("Failed to post ec_token_sem");
        return 1024;
    }

    return granularity;
}

/* Only use to reduce function parameter */
struct subtask_create_ctx {
    doca_buf *sub_src_buf;
    doca_buf *sub_dst_buf;
    uint32_t strip_id;
    bool is_last;
    bool is_sub;
    astraea_ec_task_create *origin_task;
};

static inline doca_error_t
create_subtask(const subtask_create_ctx &stsk_ctx,
               _astraea_ec_subtask_create **subtask) {
    *subtask = nullptr;
    _astraea_ec_subtask_create *new_subtask =
        stsk_ctx.origin_task
            ->subtask_pool[stsk_ctx.origin_task->cur_subtask_pos++];

    new_subtask->user_data->is_sub = stsk_ctx.is_sub;
    new_subtask->user_data->is_last = stsk_ctx.is_last;
    new_subtask->user_data->strip_id = stsk_ctx.strip_id;
    new_subtask->user_data->origin_task = stsk_ctx.origin_task;

    doca_error_t status = doca_ec_task_create_allocate_init(
        stsk_ctx.origin_task->ec->ec, stsk_ctx.origin_task->matrix->matrix,
        stsk_ctx.sub_src_buf, stsk_ctx.sub_dst_buf,
        {.ptr = new_subtask->user_data}, &new_subtask->task);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to allocate and init ec create task: %s",
                     doca_error_get_descr(status));
        return status;
    }

    *subtask = new_subtask;
    return DOCA_SUCCESS;
}

doca_error_t astraea_ec_task_create_allocate_init(
    astraea_ec *ec, astraea_ec_matrix *coding_matrix, doca_mmap *src_mmap,
    doca_buf *original_data_blocks, doca_buf *rdnc_blocks, doca_data user_data,
    astraea_ec_task_create **task) {
    *task = nullptr;
    astraea_ec_task_create *new_task = ec->task_pool[ec->cur_task_pos++];

    size_t src_buf_size;
    doca_error_t status =
        doca_buf_get_data_len(original_data_blocks, &src_buf_size);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get block size: %s",
                     doca_error_get_descr(status));
        return status;
    }

    const size_t origin_block_size =
        src_buf_size / coding_matrix->nb_data_blocks;

    new_task->origin_block_size = origin_block_size;
    new_task->user_data = user_data;
    new_task->original_data_blocks = original_data_blocks;
    new_task->rdnc_blocks = rdnc_blocks;
    new_task->ec = ec;
    new_task->matrix = coding_matrix;

    const size_t sub_block_size = calc_granularity(new_task);

    new_task->sub_block_size = sub_block_size;

    if (origin_block_size > sub_block_size) {
        void *dst_base_addr = nullptr;
        status = doca_buf_get_data(rdnc_blocks, &dst_base_addr);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to get rdnc buf addr: %s",
                         doca_error_get_descr(status));
            return status;
        }

        new_task->dst_base_addr = static_cast<uint8_t *>(dst_base_addr);

        void *src_base_addr = nullptr;
        status = doca_buf_get_data(original_data_blocks, &src_base_addr);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to get data buf addr: %s",
                         doca_error_get_descr(status));
            return status;
        }

        const uint32_t nb_strips = origin_block_size / sub_block_size;
        for (uint32_t i = 0; i < nb_strips; i++) {
            doca_buf *sub_src_buf, *sub_dst_buf;

            doca_buf *tmp_bufs[MAX_NB_DATA_BLOCKS];
            // create scatter gather list
            for (uint32_t j = 0; j < coding_matrix->nb_data_blocks; j++) {
                uint8_t *addr = static_cast<uint8_t *>(src_base_addr) +
                                j * origin_block_size + i * sub_block_size;

                status = doca_buf_inventory_buf_get_by_addr(
                    ec->buf_inventory, src_mmap, addr, sub_block_size,
                    &tmp_bufs[j]);
                if (status != DOCA_SUCCESS) {
                    DOCA_LOG_ERR("Failed to alloc buf for data blocks: %s",
                                 doca_error_get_descr(status));
                    return status;
                }

                status = doca_buf_set_data(tmp_bufs[j], addr, sub_block_size);
                if (status != DOCA_SUCCESS) {
                    DOCA_LOG_ERR("Failed to set buf data for data bufs: %s",
                                 doca_error_get_descr(status));
                    return status;
                }

                if (j == 0) {
                    sub_src_buf = tmp_bufs[j];
                } else {
                    status = doca_buf_chain_list(sub_src_buf, tmp_bufs[j]);
                    if (status != DOCA_SUCCESS) {
                        DOCA_LOG_ERR("Failed to chain list: %s",
                                     doca_error_get_descr(status));
                        return status;
                    }
                }
            }

            status = doca_buf_inventory_buf_get_by_addr(
                ec->buf_inventory, ec->dst_mmap,
                static_cast<uint8_t *>(ec->tmp_rdnc_buffer) +
                    i * sub_block_size * coding_matrix->nb_rdnc_blocks,
                sub_block_size * coding_matrix->nb_rdnc_blocks, &sub_dst_buf);
            if (status != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to alloc buf for rdnc blocks: %s",
                             doca_error_get_descr(status));
                return status;
            }

            const subtask_create_ctx stsk_ctx = {
                .sub_src_buf = sub_src_buf,
                .sub_dst_buf = sub_dst_buf,
                .strip_id = i,
                .is_last = i == nb_strips - 1 ? true : false,
                .is_sub = true,
                .origin_task = new_task};

            _astraea_ec_subtask_create *subtask = nullptr;
            status = create_subtask(stsk_ctx, &subtask);
            if (status != DOCA_SUCCESS) {
                DOCA_LOG_ERR("Failed to create sub task");
                return status;
            }
            new_task->subtasks.push_back(subtask);
            new_task->sub_buf_pairs.push_back(
                std::make_pair(stsk_ctx.sub_src_buf, stsk_ctx.sub_dst_buf));
        }
    } else {
        const subtask_create_ctx stsk_ctx = {.sub_src_buf =
                                                 original_data_blocks,
                                             .sub_dst_buf = rdnc_blocks,
                                             .strip_id = 0,
                                             .is_last = true,
                                             .is_sub = false,
                                             .origin_task = new_task};
        _astraea_ec_subtask_create *subtask = nullptr;
        status = create_subtask(stsk_ctx, &subtask);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to create sub task");
            return status;
        }

        new_task->subtasks.push_back(subtask);
    }
    *task = new_task;
    return DOCA_SUCCESS;
}

astraea_task *astraea_ec_task_create_as_task(astraea_ec_task_create *task) {
    astraea_task *general_task = new astraea_task;
    general_task->type = EC_CREATE;
    general_task->ec_task_create = task;
    return general_task;
}

doca_error_t astraea_ec_matrix_create(astraea_ec *ec, doca_ec_matrix_type type,
                                      size_t data_block_count,
                                      size_t rdnc_block_count,
                                      astraea_ec_matrix **matrix) {
    *matrix = new astraea_ec_matrix;
    (*matrix)->nb_data_blocks = data_block_count;
    (*matrix)->nb_rdnc_blocks = rdnc_block_count;

    doca_error_t status = doca_ec_matrix_create(
        ec->ec, type, data_block_count, rdnc_block_count, &(*matrix)->matrix);
    if (status != DOCA_SUCCESS) {
        delete *matrix;
        *matrix = nullptr;
    }

    return status;
}

doca_error_t astraea_ec_matrix_destroy(astraea_ec_matrix *matrix) {
    doca_error_t status = doca_ec_matrix_destroy(matrix->matrix);

    delete matrix;

    return status;
}
