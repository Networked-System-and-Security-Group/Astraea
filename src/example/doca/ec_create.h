
#include <cstddef>
#include <cstdint>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <vector>

constexpr uint32_t MAX_NB_EC_TASKS = 8192;

struct ec_create_config {
    uint32_t nb_data_blocks;
    uint32_t nb_rdnc_blocks;
    size_t block_size;
    uint32_t nb_tasks;
};

/* Helper class to allocate and destroy resources */
class ec_create_resources {
  public:
    doca_dev *dev = nullptr;

    doca_ec_matrix *matrix = nullptr;
    doca_ec *ec = nullptr;
    std::vector<doca_ec_task_create *> tasks;
    doca_ctx *ctx = nullptr;

    doca_pe *pe = nullptr;

    doca_mmap *mmap = nullptr;
    void *mmap_buffer = nullptr;
    doca_buf_inventory *buf_inventory = nullptr;
    doca_buf *src_buf = nullptr;
    std::vector<doca_buf *> dst_bufs;

    ec_create_resources();
    ~ec_create_resources();

    doca_error_t prepare_memory(const ec_create_config &cfg);

    doca_error_t setup_ec_ctx(doca_ec_task_create_completion_cb_t success_cb,
                              doca_ec_task_create_completion_cb_t error_cb);

    doca_error_t open_dev();
};

doca_error_t ec_create(const ec_create_config &cfg);
