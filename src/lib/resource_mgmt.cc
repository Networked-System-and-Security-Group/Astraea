#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>

#include <doca_log.h>

#include "doca_error.h"
#include "resource_mgmt.h"

DOCA_LOG_REGISTER(RESOURCE_MGMT);

/* Per app global variables */
sem_t *metadata_sem = nullptr;
sem_t *ec_token_sem = nullptr;
sem_t *ec_deficit_sem = nullptr;
int shm_fd = -1;
shared_resources *shm_data = nullptr;
uint32_t app_id = -1;
std::chrono::microseconds latency_sla;

/**
 * This function not only register this app in shared memory
 * But also set output parameters for the app to check status
 */
astraea_authenticator::astraea_authenticator(uint32_t latency,
                                             doca_error_t *status) {
    latency_sla = std::chrono::microseconds(latency);
    *status = DOCA_SUCCESS;

    pid_t pid = getpid();

    metadata_sem = sem_open(METADATA_SEM_NAME, 0);
    if (metadata_sem == SEM_FAILED) {
        DOCA_LOG_ERR("Failed to open metadata_sem");
        *status = DOCA_ERROR_IO_FAILED;
        return;
    }

    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        DOCA_LOG_ERR("Failed to open shared memory");
        *status = DOCA_ERROR_IO_FAILED;
        return;
    }

    shm_data = static_cast<shared_resources *>(
        mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (shm_data == MAP_FAILED) {
        DOCA_LOG_ERR("Failed to map shared memory");
        *status = DOCA_ERROR_IO_FAILED;
        return;
    }

    if (sem_wait(metadata_sem) == -1) {
        DOCA_LOG_ERR("Failed to access metadata_sem");
        *status = DOCA_ERROR_IO_FAILED;
        return;
    }

    ec_token_sem = sem_open(EC_TOKEN_SEM_NAMES[shm_data->nb_apps], 0);
    if (ec_token_sem == SEM_FAILED) {
        DOCA_LOG_ERR("Failed to open corresbonding token sem");
        sem_post(metadata_sem);
        *status = DOCA_ERROR_IO_FAILED;
        return;
    }

    ec_deficit_sem = sem_open(EC_DEFICIT_SEM_NAMES[shm_data->nb_apps], 0);
    if (ec_deficit_sem == SEM_FAILED) {
        DOCA_LOG_ERR("Failed to open corresbonding deficit sem");
        sem_post(metadata_sem);
        *status = DOCA_ERROR_IO_FAILED;
        return;
    }

    app_id = shm_data->nb_apps;
    shm_data->pids[shm_data->nb_apps++] = pid;

    if (sem_post(metadata_sem) == -1) {
        DOCA_LOG_ERR("Failed to release metadata_sem");
        *status = DOCA_ERROR_IO_FAILED;
        return;
    }
}

astraea_authenticator::~astraea_authenticator() {
    if (shm_data) {
        munmap(shm_data, SHM_SIZE);
        shm_data = nullptr;
    }

    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
    }

    if (ec_token_sem) {
        sem_close(ec_token_sem);
        ec_token_sem = nullptr;
    }

    if (metadata_sem) {
        sem_close(metadata_sem);
        metadata_sem = nullptr;
    }
}