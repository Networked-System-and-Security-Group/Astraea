#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <semaphore.h>
#include <stop_token>
#include <thread>

#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include "astraea_ctx.h"
#include "astraea_ec.h"
#include "doca_erasure_coding.h"
#include "resource_mgmt.h"

DOCA_LOG_REGISTER(ASTRAEA : CTX);

extern sem_t *ec_token_sem;
extern shared_resources *shm_data;
extern uint32_t app_id;

static void worker(std::stop_token stoken, astraea_ctx *ctx) {
    while (!stoken.stop_requested()) {
        switch (ctx->type) {
        case EC:
            astraea_ec *ec = ctx->ec;
            uint32_t nb_submitted_tasks = 0;
            uint32_t nb_avail_tokens = 0;

            /* Get avail tokens */
            if (sem_wait(ec_token_sem)) {
                DOCA_LOG_ERR("Failed to get ec_token_sem");
                break;
            }

            nb_avail_tokens = shm_data->ec_tokens[app_id];

            if (sem_post(ec_token_sem)) {
                DOCA_LOG_ERR("Failed to post ec_token_sem");
                break;
            }

            ec->subtask_locks[ec->cons_pos].lock();
            ec->subtask_locks[ec->cons_pos].unlock();

            if (nb_avail_tokens > 0 && ec->cons_pos < ec->prod_pos) {

                ctx->ctx_lock.lock();
                doca_error_t status =
                    doca_task_submit(doca_ec_task_create_as_task(
                        ctx->ec->subtask_queue[ec->cons_pos]));

                ctx->ctx_lock.unlock();
                if (status == DOCA_SUCCESS) {
                    ec->cons_pos++;
                    nb_avail_tokens--;
                    nb_submitted_tasks++;
                } else {
                    DOCA_LOG_ERR("Failed to submit sub task: %s",
                                 doca_error_get_descr(status));
                }
            }

            if (sem_wait(ec_token_sem)) {
                DOCA_LOG_ERR("Failed to get ec_token_sem");
                break;
            }

            shm_data->ec_tokens[app_id] -= nb_submitted_tasks;

            if (sem_post(ec_token_sem)) {
                DOCA_LOG_ERR("Failed to post ec_token_sem");
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
}

doca_error_t astraea_ctx_start(astraea_ctx *ctx) {
    doca_error_t status = doca_ctx_start(ctx->ctx);
    if (status != DOCA_SUCCESS) {
        return status;
    }
    ctx->submitter = new std::jthread{worker, ctx};
    return status;
}

doca_error_t astraea_ctx_stop(astraea_ctx *ctx) {
    /**
     * It is mandatory to check whether submitter is null
     * As user may call this function many times to ensure doca_ctx fully
     * stopped
     */
    if (ctx->submitter) {
        ctx->submitter->request_stop();
        if (ctx->type == EC) {
            for (uint32_t i = ctx->ec->prod_pos; i < MAX_NB_INFLIGHT_EC_TASKS;
                 i++) {
                ctx->ec->subtask_locks[i].unlock();
            }
        }
        delete ctx->submitter;
        ctx->submitter = nullptr;
    }

    if (ctx->type == EC) {
        for (uint32_t i = 0; i < MAX_NB_INFLIGHT_EC_TASKS; i++) {
            astraea_ec_task_create *task = ctx->ec->task_pool[i];
            for (uint32_t j = 0; j < MAX_NB_SUBTASKS_PER_TASK; j++) {
                _astraea_ec_subtask_create *subtask = task->subtask_pool[j];
                if (subtask->task) {
                    doca_task_free(doca_ec_task_create_as_task(subtask->task));
                }
            }
        }
    }

    /* Astraea will release astraea_ctx's memory in astraea_pe_progress */
    return doca_ctx_stop(ctx->ctx);
}