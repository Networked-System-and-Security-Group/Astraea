#include <cstddef>
#include <cstdint>
#include <vector>

#include <doca_error.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include <doca_compress.h>

constexpr uint32_t MAX_NB_AG_TASKS = 65536;
constexpr uint32_t MAX_BUF_SIZE = 2 * 1024 * 1024;

struct lz4_decomp_config
{
    size_t input_size;
    uint8_t* data;
    uint32_t nb_tasks;
};

/* Helper class to allocate and destroy resources */
class lz4_decomp_resources
{
  public:
    doca_dev* dev = nullptr;

    doca_compress* comp;
    std::vector<doca_compress_task_decompress_lz4_block*> tasks;
    doca_ctx* ctx = nullptr;

    doca_pe* pe = nullptr;

    doca_mmap* mmap = nullptr;
    void* mmap_buffer = nullptr;
    doca_buf_inventory* buf_inventory = nullptr;
    doca_buf* src_buf = nullptr;
    std::vector<doca_buf*> dst_bufs;

    lz4_decomp_resources();
    ~lz4_decomp_resources();

    doca_error_t prepare_memory(const lz4_decomp_config& cfg);

    doca_error_t setup_comp_ctx(
      doca_compress_task_decompress_lz4_block_completion_cb_t success_cb,
      doca_compress_task_decompress_lz4_block_completion_cb_t error_cb);

    doca_error_t open_dev();
};

doca_error_t
lz4_decomp(const lz4_decomp_config& cfg);
