#include <cstddef>
#include <cstdint>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <vector>

#include "astraea_ctx.h"
#include "astraea_ec.h"
#include "astraea_pe.h"

constexpr uint32_t MAX_NB_EC_TASKS = 8192;

struct ec_create_config {
    uint32_t nb_data_blocks;
    uint32_t nb_rdnc_blocks;
    size_t block_size;
    uint32_t nb_tasks;
    uint32_t latency;
};

/* Helper class to allocate and destroy resources */
class ec_create_resources {
  public:
    doca_dev *dev = nullptr;

    astraea_ec_matrix *matrix = nullptr;
    astraea_ec *ec = nullptr;
    std::vector<astraea_ec_task_create *> tasks;
    astraea_ctx *ctx = nullptr;

    astraea_pe *pe = nullptr;

    doca_mmap *mmap = nullptr;
    void *mmap_buffer = nullptr;
    doca_buf_inventory *buf_inventory = nullptr;
    doca_buf *src_buf = nullptr;
    std::vector<doca_buf *> dst_bufs;

    ec_create_resources();
    ~ec_create_resources();

    doca_error_t prepare_memory(const ec_create_config &cfg);

    doca_error_t setup_ec_ctx(astraea_ec_task_create_completion_cb_t success_cb,
                              astraea_ec_task_create_completion_cb_t error_cb);

    doca_error_t open_dev();
};

struct ec_create_user_data {
    std::vector<std::chrono::high_resolution_clock::time_point> *begin_time_arr;
    std::vector<std::chrono::high_resolution_clock::time_point> *end_time_arr;
    ec_create_resources *rscs;
    uint32_t *nb_finished_tasks;
    uint32_t nb_total_tasks;
};

doca_error_t ec_create(const ec_create_config &cfg);
