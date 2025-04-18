#ifndef ASTRAEA_CTX_H__
#define ASTRAEA_CTX_H__

#include <mutex>
#include <stop_token>
#include <thread>

#include <doca_ctx.h>
#include <doca_error.h>

/**
 * Forward declarations
 */
struct astraea_ec;

enum ctx_type { EC };

struct astraea_ctx {
    doca_ctx *ctx;
    std::jthread *submitter;
    ctx_type type;
    union {
        astraea_ec *ec;
    };
    std::mutex ctx_lock;
};

doca_error_t astraea_ctx_start(astraea_ctx *ctx);

doca_error_t astraea_ctx_stop(astraea_ctx *ctx);

#endif