#include <chrono>
#include <cstdint>
#include <mutex>
#include <semaphore.h>

#include <doca_error.h>
#include <doca_pe.h>
#include <utility>
#include <vector>

#include "astraea_ctx.h"
#include "astraea_ec.h"
#include "astraea_pe.h"

#include "doca_buf.h"
#include "doca_log.h"
#include "doca_mmap.h"
#include "resource_mgmt.h"

DOCA_LOG_REGISTER(ASTRAEA : PE);

extern sem_t *metadata_sem;
extern shared_resources *shm_data;
extern uint32_t app_id;
extern std::chrono::microseconds latency_sla;

auto last_expect_time = std::chrono::high_resolution_clock::now();

bool has_finished_task = false;

doca_error_t astraea_pe_create(astraea_pe **pe) {
    *pe = new astraea_pe;

    doca_error_t status = doca_pe_create(&(*pe)->pe);

    if (status != DOCA_SUCCESS) {
        delete *pe;
        *pe = nullptr;
    }

    return status;
}

doca_error_t astraea_pe_destroy(astraea_pe *pe) {
    for (astraea_ctx *ctx : pe->ctxs) {
        delete ctx;
    }

    doca_error_t status = doca_pe_destroy(pe->pe);

    delete pe;

    return status;
}

uint8_t astraea_pe_progress(astraea_pe *pe) {
    /* Lock all ctx */
    for (astraea_ctx *ctx : pe->ctxs) {
        ctx->ctx_lock.lock();
    }

    doca_pe_progress(pe->pe);

    /* Unlock all ctx */
    for (astraea_ctx *ctx : pe->ctxs) {
        ctx->ctx_lock.unlock();
    }

    if (has_finished_task) {
        has_finished_task = false;
        return 1;
    }
    return 0;
}

doca_error_t astraea_task_submit(astraea_task *task) {
    if (task->type == EC_CREATE) {

        auto cur_time = std::chrono::high_resolution_clock::now();

        last_expect_time =
            latency_sla +
            (last_expect_time > cur_time ? last_expect_time : cur_time);
        task->ec_task_create->expected_time = last_expect_time;

        uint32_t nb_sub_tasks = task->ec_task_create->cur_subtask_pos;
        astraea_ec *ec = task->ec_task_create->ec;
        for (uint32_t i = 0; i < nb_sub_tasks; i++) {
            ec->subtask_queue[ec->prod_pos] =
                task->ec_task_create->subtask_pool[i]->task;
            /* Unlock subtask for consumer */
            ec->subtask_locks[ec->prod_pos].unlock();
            ec->prod_pos++;
        }
    }
    return DOCA_SUCCESS;
}

void astraea_task_free(astraea_task *task) { delete task; }

doca_error_t astraea_pe_connect_ctx(astraea_pe *pe, astraea_ctx *ctx) {
    /**
     * We don't need to lock ctx here
     * As this always happen before task submitting
     */
    doca_error_t status = doca_pe_connect_ctx(pe->pe, ctx->ctx);
    if (status == DOCA_SUCCESS) {
        pe->ctxs.push_back(ctx);
    }
    return status;
}