#ifndef __ASTRAEA_RDMA_H__
#define __ASTRAEA_RDMA_H__

#include <doca_dev.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <doca_rdma.h>

#include "Ctx.h"

namespace astraea {
class Rdma : public Ctx {
  public:
    doca_rdma *mRdma = nullptr;

    doca_error_t start() override;
    doca_error_t stop() override;
    doca_error_t connectToPe(Pe *aPe) override;
};

class RdmaTaskWrite : public Task {
  public:
    doca_rdma_task_write *mTask;

    doca_error_t submit() override;
    void free() override;
};
} // namespace astraea
#endif