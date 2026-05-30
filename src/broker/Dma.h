#pragma once

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_dma.h>

#include "Ctx.h"
#include "Task.h"

namespace astraea {
class Dma : public Ctx {
   public:
    doca_dma *mDma = nullptr;

    doca_error_t start() override;
    doca_error_t stop() override;
    doca_error_t connectToPe(Pe *aPe) override;
};

class DmaTaskMemcpy : public Task {
   public:
    doca_dma_task_memcpy *mTask;

    doca_error_t submit() override;
    void free() override;
};
}  // namespace astraea
