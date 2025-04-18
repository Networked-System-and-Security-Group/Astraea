#ifndef RESOURCE_MGMT_H__
#define RESOURCE_MGMT_H__

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

#include <doca_error.h>

/**
 * Forward declarations
 */
struct astraea_ec_task_create;

/**
 * An ec create task takes 25us
 * So the ec ctx can run 40 1024B tasks per ms
 */

static constexpr uint32_t MAX_SEM_NAME_LEN = 256;
constexpr uint32_t MAX_NB_APPS = 2;

/* Guard nb_apps and pids*/
constexpr char METADATA_SEM_NAME[] = "/metadata_sem";

/* Guard ec_tokens */
constexpr char EC_TOKEN_SEM_NAMES[MAX_NB_APPS][MAX_SEM_NAME_LEN] = {
    "/ec_token_sem1", "/ec_token_sem2"};

/* Guard deficits */
constexpr char EC_DEFICIT_SEM_NAMES[MAX_NB_APPS][MAX_SEM_NAME_LEN] = {
    "/ec_deficit_sem1", "/ec_deficit_sem2"};

constexpr char SHM_NAME[] = "/shm";

/* This locates on shared memory */
struct shared_resources {
    uint32_t nb_apps;
    /* Available ec tokens for rate limiting */
    uint32_t ec_tokens[MAX_NB_APPS];
    /* Deficits for scheduling */
    uint32_t deficits[MAX_NB_APPS];
    pid_t pids[MAX_NB_APPS];
};

constexpr size_t SHM_SIZE = sizeof(shared_resources);

/**
 * A RAII class to register app
 * And pre-allocate global vars(shared memory and semaphore)
 */
class astraea_authenticator {
  public:
    astraea_authenticator(uint32_t latency, doca_error_t *status);
    ~astraea_authenticator();
};

#endif