#ifndef ASTRAEA_EC_H__
#define ASTRAEA_EC_H__

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_types.h>

constexpr uint32_t MAX_NB_SUBTASKS_PER_TASK = 1024;
constexpr uint32_t MAX_NB_INFLIGHT_EC_TASKS = 8192;
constexpr uint32_t MAX_NB_CTX_BUFS = 1024 * 1024;
constexpr size_t TMP_RDNC_BUFFER_SIZE = 32 * 1024 * 1024 * 32;
constexpr uint32_t MAX_NB_DATA_BLOCKS = 128;
constexpr uint32_t MAX_NB_RDNC_BLOCKS = 32;

/**
 * Forward declarations
 */
struct astraea_task;
struct astraea_ctx;

/* Forward declaration for structs in this file */
struct astraea_ec;
struct astraea_ec_task_create;
///////////////////////

typedef void (*astraea_ec_task_create_completion_cb_t)(
    astraea_ec_task_create *task, doca_data task_user_data,
    doca_data ctx_user_data);

struct _astraea_ec_subtask_create_user_data {
    bool is_sub;
    bool is_last;
    uint32_t strip_id;
    astraea_ec_task_create *origin_task;
};

struct _astraea_ec_subtask_create {
    doca_ec_task_create *task;
    _astraea_ec_subtask_create_user_data *user_data;
};

struct astraea_ec_matrix {
    doca_ec_matrix *matrix;
    uint32_t nb_data_blocks;
    uint32_t nb_rdnc_blocks;
};

struct astraea_ec_task_create {
    /* Resources managed by task itself */
    std::vector<_astraea_ec_subtask_create *> subtasks;
    std::vector<std::pair<doca_buf *, doca_buf *>> sub_buf_pairs;
    _astraea_ec_subtask_create *subtask_pool[MAX_NB_SUBTASKS_PER_TASK];
    uint32_t cur_subtask_pos;

    /* Metadatas that we only want to set once */
    size_t origin_block_size;
    size_t sub_block_size;
    uint8_t *dst_base_addr;

    /* Resources managed by other objects */
    doca_data user_data;
    doca_buf *original_data_blocks;
    doca_buf *rdnc_blocks;
    astraea_ec *ec;
    astraea_ec_matrix *matrix;
    std::chrono::high_resolution_clock::time_point expected_time;
    bool is_free;
};

struct astraea_ec {
    doca_ec *ec;
    astraea_ec_task_create_completion_cb_t success_cb;
    astraea_ec_task_create_completion_cb_t error_cb;
    std::queue<doca_ec_task_create *> ec_create_tasks; /* Sub tasks */
    std::mutex ec_create_tasks_lock;
    doca_dev *dev;

    void *tmp_rdnc_buffer;
    doca_mmap *dst_mmap;
    doca_buf_inventory *buf_inventory;

    astraea_ec_task_create *task_pool[MAX_NB_INFLIGHT_EC_TASKS];
    uint32_t cur_task_pos;
};

doca_error_t astraea_ec_create(doca_dev *dev, astraea_ec **ec);

doca_error_t astraea_ec_destroy(astraea_ec *ec);

astraea_ctx *astraea_ec_as_ctx(astraea_ec *ec);

doca_error_t astraea_ec_task_create_set_conf(
    astraea_ec *ec,
    astraea_ec_task_create_completion_cb_t successful_task_completion_cb,
    astraea_ec_task_create_completion_cb_t error_task_completion_cb,
    uint32_t num_tasks);

doca_error_t astraea_ec_task_create_allocate_init(
    astraea_ec *ec, astraea_ec_matrix *coding_matrix, doca_mmap *src_mmap,
    doca_buf *original_data_blocks, doca_buf *rdnc_blocks, doca_data user_data,
    astraea_ec_task_create **task);

astraea_task *astraea_ec_task_create_as_task(astraea_ec_task_create *task);

doca_error_t astraea_ec_matrix_create(astraea_ec *ec, doca_ec_matrix_type type,
                                      size_t data_block_count,
                                      size_t rdnc_block_count,
                                      astraea_ec_matrix **matrix);

doca_error_t astraea_ec_matrix_destroy(astraea_ec_matrix *matrix);

#endif