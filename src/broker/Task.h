#ifndef ASTRAEA_TASK_H_
#define ASTRAEA_TASK_H_

#include <chrono>
#include <cstddef>
#include <doca_error.h>
#include <doca_types.h>

#include <doca_buf.h>

#include <doca_pe.h>

#include "common.h"

namespace astraea {
class Task;
}

namespace astraea {
struct UserData {
    bool isLast;
    bool isSub;
    doca_data rawData;
    Task *rawTask;
    doca_buf *srcBuf;
    doca_buf *dstBuf;
    u32 stripId;
};

class Task {
  public:
    doca_task *mTask = nullptr;
    std::chrono::high_resolution_clock::time_point mExpectTime;

    virtual ~Task(){};

    virtual doca_error_t submit() = 0;
    virtual void free() = 0;
};
} // namespace astraea
#endif