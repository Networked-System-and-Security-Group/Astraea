#ifndef ASTRAEA_PE_H__
#define ASTRAEA_PE_H__

#include <cstdint>
#include <vector>

#include <doca_error.h>
#include <doca_pe.h>

/**
 * Forward declarations
 */
struct astraea_ec_task_create;
struct astraea_ctx;

struct astraea_pe {
    doca_pe *pe;
    std::vector<astraea_ctx *> ctxs;
};

enum task_type { EC_CREATE };

struct astraea_task {
    task_type type;
    union {
        astraea_ec_task_create *ec_task_create;
    };
};

doca_error_t astraea_pe_create(astraea_pe **pe);

doca_error_t astraea_pe_destroy(astraea_pe *pe);

uint8_t astraea_pe_progress(astraea_pe *pe);

doca_error_t astraea_task_submit(astraea_task *task);

void astraea_task_free(astraea_task *task);

doca_error_t astraea_pe_connect_ctx(astraea_pe *pe, astraea_ctx *ctx);

#endif