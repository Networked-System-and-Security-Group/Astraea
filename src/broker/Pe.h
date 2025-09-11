#ifndef ASTRAEA_PE_H_
#define ASTRAEA_PE_H_

#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <doca_error.h>

#include <doca_ctx.h>
#include <doca_pe.h>

#include "common.h"

/* Forward declarations */
namespace astraea {
class Ctx;
}
////////////////////////////////

namespace astraea {

class Pe {
  public:
    doca_pe *mPe;
    std::vector<Ctx *> mCtxs;
    std::vector<std::mutex *> mLocks;
    /* We have to use uint8_t instead of bool
    as bool* is hard to acquire from std::vector<bool> */
    std::vector<u8> mIsStoppeds;
    std::jthread *mWorker = nullptr;
};
} // namespace astraea
#endif