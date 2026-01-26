#include "Ctx.h"

#include <dlfcn.h>
#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>

using namespace astraea;
DOCA_LOG_REGISTER(ASTRAEA : CTX)

/* Original function pointers */
doca_error_t (*original_doca_ctx_start)(doca_ctx *ctx) =
    reinterpret_cast<doca_error_t (*)(doca_ctx *ctx)>(dlsym(RTLD_NEXT,
                                                            "doca_ctx_start"));
doca_error_t (*original_doca_ctx_stop)(doca_ctx *ctx) =
    reinterpret_cast<doca_error_t (*)(doca_ctx *ctx)>(dlsym(RTLD_NEXT,
                                                            "doca_ctx_stop"));
////////////////////////////////////////////////////////////////////////////////

doca_error_t doca_ctx_start(doca_ctx *ctx) {
    auto myCtx = reinterpret_cast<Ctx *>(ctx);
    return myCtx->start();
}

doca_error_t doca_ctx_stop(doca_ctx *ctx) {
    auto myCtx = reinterpret_cast<Ctx *>(ctx);
    return myCtx->stop();
}