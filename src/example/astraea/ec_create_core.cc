#include <bits/types/sigset_t.h>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_types.h>

#include "astraea_ctx.h"
#include "astraea_ec.h"
#include "astraea_pe.h"

#include "ec_create.h"

DOCA_LOG_REGISTER(EC_CREATE : CORE);

static void write_to_file(void *data, size_t size, const char *file_name) {
    namespace fs = std::filesystem;

    // 创建父目录（如果不存在）
    const fs::path file_path(file_name);
    fs::create_directories(file_path.parent_path());

    // 以二进制模式打开文件，清空现有内容或创建新文件
    std::ofstream file(file_name, std::ios::binary | std::ios::trunc);

    // 检查文件流状态
    if (!file) {
        throw std::runtime_error("Failed to open file: " +
                                 std::string(file_name));
    }

    // 写入二进制数据
    file.write(reinterpret_cast<const char *>(data),
               static_cast<std::streamsize>(size));

    // 显式关闭文件（RAII机制会保证关闭，但显式调用更明确）
    file.close();

    // 验证写入完整性
    if (file.bad()) {
        throw std::runtime_error("Failed to write complete data to file");
    }
}

/* Tasks will be free in the destructor */
void ec_create_success_cb(astraea_ec_task_create *task,
                          doca_data task_user_data, doca_data ctx_user_data) {
    (void)task;
    (void)ctx_user_data;
    uint32_t *nb_finished_tasks = static_cast<uint32_t *>(task_user_data.ptr);
    (*nb_finished_tasks)++;
}
void ec_create_error_cb(astraea_ec_task_create *task, doca_data task_user_data,
                        doca_data ctx_user_data) {
    (void)task;
    (void)ctx_user_data;
    uint32_t *nb_finished_tasks = static_cast<uint32_t *>(task_user_data.ptr);
    (*nb_finished_tasks)++;
    DOCA_LOG_ERR("EC create task failed");
}

doca_error_t ec_create(const ec_create_config &cfg) {
    doca_error_t status;

    ec_create_resources rscs;

    /* Open device */
    status = rscs.open_dev();
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to open device");
        return status;
    }

    /* Create pe */
    status = astraea_pe_create(&rscs.pe);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create pe: %s", doca_error_get_descr(status));
        return status;
    }

    /* Create and config ec ctx */
    status = rscs.setup_ec_ctx(ec_create_success_cb, ec_create_error_cb);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to setup ec ctx");
        return status;
    }

    /* Setup mmap, buf inventory and bufs */
    status = rscs.prepare_memory(cfg);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to prepare bufs");
        return status;
    }

    /* Create and submit task */
    status = astraea_ec_matrix_create(rscs.ec, DOCA_EC_MATRIX_TYPE_CAUCHY,
                                      cfg.nb_data_blocks, cfg.nb_rdnc_blocks,
                                      &rscs.matrix);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create ec matrix: %s",
                     doca_error_get_descr(status));
        return status;
    }

    /* Wait for the signal to submit task */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int sig;

    DOCA_LOG_INFO("Wait for signal SIGUSR1 to continue");
    sigwait(&mask, &sig);

    auto begin_time = std::chrono::high_resolution_clock::now();

    uint32_t nb_finished_tasks = 0;
    for (uint32_t i = 0; i < cfg.nb_tasks; i++) {
        astraea_ec_task_create *task;
        status = astraea_ec_task_create_allocate_init(
            rscs.ec, rscs.matrix, rscs.mmap, rscs.src_buf, rscs.dst_bufs[i],
            {.ptr = &nb_finished_tasks}, &task);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to allocate and init ec task: %s",
                         doca_error_get_descr(status));
            return status;
        }

        status = astraea_task_submit(astraea_ec_task_create_as_task(task));
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to submit task: %s",
                         doca_error_get_descr(status));
            return status;
        }

        rscs.tasks.push_back(task);
    }

    while (nb_finished_tasks < cfg.nb_tasks)
        (void)astraea_pe_progress(rscs.pe);

    auto end_time = std::chrono::high_resolution_clock::now();

    double time_cost_in_ms =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time -
                                                             begin_time)
            .count() /
        (double)1000000;
    DOCA_LOG_INFO("All tasks finished, taking %fms", time_cost_in_ms);
    write_to_file(static_cast<uint8_t *>(rscs.mmap_buffer) +
                      cfg.nb_data_blocks * cfg.block_size,
                  cfg.nb_rdnc_blocks * cfg.block_size, "./out/astraea");
    return DOCA_SUCCESS;
}
