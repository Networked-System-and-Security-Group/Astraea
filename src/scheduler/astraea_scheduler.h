#ifndef ASTRAEA_SCHEDULER_H__
#define ASTRAEA_SCHEDULER_H__

#include "resource_mgmt.h"
#include <cstdint>
#include <semaphore.h>
#include <vector>

#include <doca_error.h>

constexpr double EWMA_COEFF = 0.5;
constexpr uint32_t MAX_TOKENS_PER_MS = 120;
constexpr uint32_t AVAIL_TOKENS_PER_MS = MAX_TOKENS_PER_MS * 0.9;
constexpr uint32_t RESERVED_TOKENS_PER_MS =
    MAX_TOKENS_PER_MS - AVAIL_TOKENS_PER_MS;

/**
 * Forward declarations
 */
struct shared_resources;

class astraea_scheduler {
  private:
    /* Semaphores */
    std::vector<sem_t *> ec_token_sems;
    std::vector<sem_t *> ec_deficit_sems;
    sem_t *metadata_sem = nullptr;

    /* Shared memory */
    int shm_fd = -1;
    shared_resources *shm_data = nullptr;

    uint32_t allocated_ec_tokens[MAX_NB_APPS];

    void refresh_tokens();

  public:
    astraea_scheduler(doca_error_t *status);
    ~astraea_scheduler();

    void run();
};

#endif