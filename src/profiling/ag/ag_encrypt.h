
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

#include <doca_aes_gcm.h>

constexpr uint32_t MAX_NB_AG_TASKS = 65536;
constexpr uint32_t DEFAULT_TAG_SIZE_IN_BYTES = 96 / 8;
constexpr uint32_t DEFAULT_KEY_SIZE_IN_BYTES = 128 / 8;
constexpr uint32_t DEFAULT_IV_SIZE_IN_BYTES = 12;
constexpr char DEFAULT_KEY[DEFAULT_KEY_SIZE_IN_BYTES] = { 0 };
constexpr uint8_t DEFAULT_IV[DEFAULT_IV_SIZE_IN_BYTES] = { 0 };

struct lz4_decomp_config
{
    uint32_t plaintext_size;
    uint32_t aad_size;
    uint32_t nb_tasks;
};

/* Helper class to allocate and destroy resources */
class lz4_decomp_resources
{
  public:
    doca_dev* dev = nullptr;

    doca_aes_gcm_key* key = nullptr;
    doca_aes_gcm* ag = nullptr;
    std::vector<doca_aes_gcm_task_encrypt*> tasks;
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

    doca_error_t setup_ag_ctx(
      doca_aes_gcm_task_encrypt_completion_cb_t success_cb,
      doca_aes_gcm_task_encrypt_completion_cb_t error_cb);

    doca_error_t open_dev();
};

doca_error_t
lz4_decomp(const lz4_decomp_config& cfg);
