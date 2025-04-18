#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <doca_error.h>
#include <doca_log.h>

#include "astraea_scheduler.h"
#include "doca_error.h"
#include "resource_mgmt.h"

DOCA_LOG_REGISTER(ASTRAEA:SCHEDULER : CORE);

astraea_scheduler::astraea_scheduler(doca_error_t *status) {
    /* Init semaphores */
    for (uint32_t i = 0; i < MAX_NB_APPS; i++) {
        sem_t *sem = sem_open(EC_TOKEN_SEM_NAMES[i], O_CREAT, 0666, 1);
        if (sem == SEM_FAILED) {
            DOCA_LOG_ERR("Failed to create token_sems[%u]", i);
            *status = DOCA_ERROR_OPERATING_SYSTEM;
            return;
        }
        ec_token_sems.push_back(sem);

        sem = sem_open(EC_DEFICIT_SEM_NAMES[i], O_CREAT, 0666, 1);
        if (sem == SEM_FAILED) {
            DOCA_LOG_ERR("Failed to create deficit_sems[%u]", i);
            *status = DOCA_ERROR_OPERATING_SYSTEM;
            return;
        }
        ec_deficit_sems.push_back(sem);
    }

    metadata_sem = sem_open(METADATA_SEM_NAME, O_CREAT, 0666, 1);
    if (metadata_sem == SEM_FAILED) {
        DOCA_LOG_ERR("Failed to create nb_apps_sem");
        *status = DOCA_ERROR_OPERATING_SYSTEM;
        return;
    }

    /* Init shared memory */
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        DOCA_LOG_ERR("Failed to create shared memory fd");
        *status = DOCA_ERROR_OPERATING_SYSTEM;
        return;
    }

    if (ftruncate(shm_fd, SHM_SIZE) == -1) {
        DOCA_LOG_ERR("Failed to set shm size");
        *status = DOCA_ERROR_OPERATING_SYSTEM;
        return;
    }

    void *shm_addr =
        mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_addr == MAP_FAILED) {
        DOCA_LOG_ERR("Failed to map shared memory");
        *status = DOCA_ERROR_OPERATING_SYSTEM;
        return;
    }

    /**
     * The scheduler must start before user apps
     * So we don't use semphore to lock shm here
     */
    shm_data = static_cast<shared_resources *>(shm_addr);
    shm_data->nb_apps = 0;
    for (uint32_t i = 0; i < MAX_NB_APPS; i++) {
        shm_data->ec_tokens[i] = 0;
        shm_data->pids[i] = -1;
    }

    memset(allocated_ec_tokens, 0, sizeof(allocated_ec_tokens));

    *status = DOCA_SUCCESS;
}

astraea_scheduler::~astraea_scheduler() {
    /* Release shared memory resources */
    if (shm_data) {
        munmap(shm_data, SHM_SIZE);
        shm_data = nullptr;
    }

    if (shm_fd != -1) {
        close(shm_fd);
        shm_unlink(SHM_NAME);
        shm_fd = -1;
    }

    /* Release semaphores */
    for (uint32_t i = 0; i < ec_token_sems.size(); i++) {
        sem_close(ec_token_sems[i]);
        sem_unlink(EC_TOKEN_SEM_NAMES[i]);
        sem_close(ec_deficit_sems[i]);
        sem_unlink(EC_DEFICIT_SEM_NAMES[i]);
    }
    ec_token_sems.clear();

    if (metadata_sem) {
        sem_close(metadata_sem);
        sem_unlink(METADATA_SEM_NAME);
        metadata_sem = nullptr;
    }
}

bool scheduler_force_quit = false;

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        DOCA_LOG_INFO("\n\nSignal %d received, preparing to exit...\n", signum);
        scheduler_force_quit = true;
    }
}

uint32_t pred_tokens[MAX_NB_APPS];
void astraea_scheduler::refresh_tokens() {
    /**
     * This operation should always finish without any wait
     * As there won't be so many new apps
     */
    if (sem_wait(metadata_sem) == -1) {
        DOCA_LOG_ERR("Failed to access metadata_sem");
        return;
    }

    for (uint32_t i = 0; i < shm_data->nb_apps; i++) {
        if (sem_wait(ec_token_sems[i]) == -1) {
            DOCA_LOG_ERR("Failed to access token_sems[%d]", i);
            continue;
        }
        if (sem_wait(ec_deficit_sems[i])) {
            DOCA_LOG_ERR("Failed to get ec_token_sem");
            continue;
        }
    }

    double pred_sum = 0;
    double deficit_sum = 0;
    for (uint32_t i = 0; i < shm_data->nb_apps; i++) {
        uint32_t nb_used_tokens =
            allocated_ec_tokens[i] - shm_data->ec_tokens[i];
        pred_tokens[i] = EWMA_COEFF * nb_used_tokens +
                         (1 - EWMA_COEFF) * allocated_ec_tokens[i];
        pred_sum += pred_tokens[i];
        deficit_sum += shm_data->deficits[i];
        /* Clear deficits */
        shm_data->deficits[i] = 0;
    }

    for (uint32_t i = 0; i < shm_data->nb_apps; i++) {
        uint32_t nb_allocated_tokens =
            deficit_sum == 0 ? pred_tokens[i] / pred_sum * MAX_TOKENS_PER_MS
                             : pred_tokens[i] / pred_sum * AVAIL_TOKENS_PER_MS +
                                   shm_data->deficits[i] / deficit_sum *
                                       RESERVED_TOKENS_PER_MS;
        /* Deal with initial state */
        if (nb_allocated_tokens == 0) {
            nb_allocated_tokens = MAX_TOKENS_PER_MS / shm_data->nb_apps;
        }
        allocated_ec_tokens[i] = nb_allocated_tokens;
        shm_data->ec_tokens[i] = nb_allocated_tokens;
    }

    for (uint32_t i = 0; i < shm_data->nb_apps; i++) {
        if (sem_post(ec_token_sems[i]) == -1) {
            DOCA_LOG_ERR("Failed to release token_sems[%d]", i);
        }
        if (sem_post(ec_deficit_sems[i])) {
            DOCA_LOG_ERR("Failed to post ec_token_sem");
        }
    }

    if (sem_post(metadata_sem) == -1) {
        DOCA_LOG_ERR("Failed to release metadata_sem");
    }
}

void astraea_scheduler::run() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    do {
        refresh_tokens();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!scheduler_force_quit);
}