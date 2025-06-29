#include <bits/types/sigset_t.h>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
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

    ec_create_user_data *user_data =
        static_cast<ec_create_user_data *>(task_user_data.ptr);
    user_data->end_time_arr->push_back(
        std::chrono::high_resolution_clock::now());

    uint32_t *nb_finished_tasks = user_data->nb_finished_tasks;
    (*nb_finished_tasks)++;

    if (*nb_finished_tasks == user_data->nb_total_tasks) {
        return;
    }

    ec_create_resources *rscs = user_data->rscs;
    astraea_ec_task_create *new_task;

    user_data->begin_time_arr->push_back(
        std::chrono::high_resolution_clock::now());

    doca_error_t status = astraea_ec_task_create_allocate_init(
        rscs->ec, rscs->matrix, rscs->mmap, rscs->src_buf,
        rscs->dst_bufs[*nb_finished_tasks], {.ptr = user_data}, &new_task);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to allocate and init ec task: %s",
                     doca_error_get_descr(status));
        return;
    }

    status = astraea_task_submit(astraea_ec_task_create_as_task(new_task));
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to submit task: %s", doca_error_get_descr(status));
        return;
    }
    rscs->tasks.push_back(new_task);
}
void ec_create_error_cb(astraea_ec_task_create *task, doca_data task_user_data,
                        doca_data ctx_user_data) {
    (void)task;
    (void)ctx_user_data;

    ec_create_user_data *user_data =
        static_cast<ec_create_user_data *>(task_user_data.ptr);
    uint32_t *nb_finished_tasks = user_data->nb_finished_tasks;
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
    status = astraea_ec_matrix_create(rscs.ec, ASTRAEA_EC_MATRIX_TYPE_CAUCHY,
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

    std::vector<std::chrono::high_resolution_clock::time_point> begin_time_arr;
    std::vector<std::chrono::high_resolution_clock::time_point> end_time_arr;
    uint32_t nb_finished_tasks = 0;
    ec_create_user_data user_data = {.begin_time_arr = &begin_time_arr,
                                     .end_time_arr = &end_time_arr,
                                     .rscs = &rscs,
                                     .nb_finished_tasks = &nb_finished_tasks,
                                     .nb_total_tasks = cfg.nb_tasks};

    astraea_ec_task_create *task;
    begin_time_arr.push_back(std::chrono::high_resolution_clock::now());
    status = astraea_ec_task_create_allocate_init(
        rscs.ec, rscs.matrix, rscs.mmap, rscs.src_buf, rscs.dst_bufs[0],
        {.ptr = &user_data}, &task);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to allocate and init ec task: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = astraea_task_submit(astraea_ec_task_create_as_task(task));
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to submit task: %s", doca_error_get_descr(status));
        return status;
    }
    rscs.tasks.push_back(task);

    while (nb_finished_tasks < cfg.nb_tasks) {
        (void)astraea_pe_progress(rscs.pe);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    for (uint32_t i = 0; i < cfg.nb_tasks; i++) {
        std::chrono::high_resolution_clock::time_point begin_time =
            begin_time_arr[i];
        std::chrono::high_resolution_clock::time_point end_time =
            end_time_arr[i];

        double time_cost_in_ms =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end_time -
                                                                 begin_time)
                .count() /
            (double)1000000;
        printf("%f%s", time_cost_in_ms, i == cfg.nb_tasks - 1 ? "\n" : ",");
    }

    write_to_file(static_cast<uint8_t *>(rscs.mmap_buffer) +
                      cfg.nb_data_blocks * cfg.block_size,
                  cfg.nb_rdnc_blocks * cfg.block_size, "./out/astraea");

    return DOCA_SUCCESS;
}
