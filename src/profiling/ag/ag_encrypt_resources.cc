#include <cstddef>
#include <cstdint>
#include <cstring>

#include <doca_error.h>
#include <doca_log.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include <doca_aes_gcm.h>

#include "ag_encrypt.h"

DOCA_LOG_REGISTER(AG_ENCRYPT::RESOURCES);

static void
mock_data(void* buffer, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        *(static_cast<uint8_t*>(buffer) + i) = i;
    }
}

lz4_decomp_resources::lz4_decomp_resources() {}

lz4_decomp_resources::~lz4_decomp_resources()
{
    /* Free tasks */
    for (doca_aes_gcm_task_encrypt* task : tasks)
        doca_task_free(doca_aes_gcm_task_encrypt_as_task(task));

    /* Destroy bufs, inventory and mmap */
    for (doca_buf* dst_buf : dst_bufs)
        doca_buf_dec_refcount(dst_buf, nullptr);
    if (src_buf)
        doca_buf_dec_refcount(src_buf, nullptr);
    if (buf_inventory)
        doca_buf_inventory_destroy(buf_inventory);
    if (mmap)
        doca_mmap_destroy(mmap);
    if (mmap_buffer)
        free(mmap_buffer);

    /* Destroy ec related resources */
    if (ctx) {
        doca_error_t status = doca_ctx_stop(ctx);
        /**
         * Make sure to finish the last inflight task
         * before destroy ec and pe
         */
        while (status == DOCA_ERROR_IN_PROGRESS) {
            while (doca_pe_progress(pe) == 0)
                ;
            status = doca_ctx_stop(ctx);
        }
    }

    if (key)
        doca_aes_gcm_key_destroy(key);

    if (ag)
        doca_aes_gcm_destroy(ag);

    /* Destroy pe */
    if (pe)
        doca_pe_destroy(pe);

    /* Close device */
    if (dev)
        doca_dev_close(dev);
}

doca_error_t
lz4_decomp_resources::prepare_memory(const lz4_decomp_config& cfg)
{
    doca_error_t status;

    status = doca_mmap_create(&mmap);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create mmap: %s", doca_error_get_descr(status));
        return status;
    }

    status = doca_mmap_add_dev(mmap, dev);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to add dev: %s", doca_error_get_descr(status));
        return status;
    }

    size_t input_buf_size = cfg.plaintext_size + cfg.aad_size;
    size_t output_buf_size = input_buf_size + DEFAULT_TAG_SIZE_IN_BYTES;
    size_t mmap_size = input_buf_size + output_buf_size * cfg.nb_tasks;

    int ret = posix_memalign(&mmap_buffer, 64, mmap_size);
    if (ret) {
        DOCA_LOG_ERR("Failed to alloc memory for mmap");
        return DOCA_ERROR_NO_MEMORY;
    }

    /* Moke data on the data blocks */
    mock_data(mmap_buffer, input_buf_size);

    status = doca_mmap_set_memrange(mmap, mmap_buffer, mmap_size);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set memrange: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = doca_mmap_start(mmap);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_descr(status));
        return status;
    }

    const uint32_t nb_bufs = 1 + cfg.nb_tasks;
    status = doca_buf_inventory_create(nb_bufs, &buf_inventory);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create buf inventory: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = doca_buf_inventory_start(buf_inventory);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start buf inventory: %s",
                     doca_error_get_descr(status));
        return status;
    }
    /* Get data buf */
    status = doca_buf_inventory_buf_get_by_addr(
      buf_inventory, mmap, mmap_buffer, input_buf_size, &src_buf);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to alloc buf for data blocks: %s",
                     doca_error_get_descr(status));
        return status;
    }
    /**
     * Set data buf's begin addr and length
     * The length will be 0 if not doing this
     */
    status = doca_buf_set_data(src_buf, mmap_buffer, input_buf_size);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set data data: %s",
                     doca_error_get_descr(status));
        return status;
    }
    /* Get rdnc bufs */
    for (uint32_t i = 0; i < cfg.nb_tasks; i++) {
        doca_buf* dst_buf;
        status = doca_buf_inventory_buf_get_by_addr(
          buf_inventory,
          mmap,
          static_cast<uint8_t*>(mmap_buffer) + input_buf_size +
            i * output_buf_size,
          output_buf_size,
          &dst_buf);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to alloc buf for rdnc blocks: %s",
                         doca_error_get_descr(status));
            return status;
        }
        dst_bufs.push_back(dst_buf);
    }

    return DOCA_SUCCESS;
}

doca_error_t
lz4_decomp_resources::setup_ag_ctx(
  doca_aes_gcm_task_encrypt_completion_cb_t success_cb,
  doca_aes_gcm_task_encrypt_completion_cb_t error_cb)
{
    doca_error_t status;
    status = doca_aes_gcm_create(dev, &ag);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create ag: %s", doca_error_get_descr(status));
        return status;
    }

    status = doca_aes_gcm_task_encrypt_set_conf(
      ag, success_cb, error_cb, MAX_NB_AG_TASKS);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set ag encrypt task conf: %s",
                     doca_error_get_descr(status));
        return status;
    }

    ctx = doca_aes_gcm_as_ctx(ag);
    if (!ctx) {
        DOCA_LOG_ERR("Failed to convert ag to ctx");
        return DOCA_ERROR_UNEXPECTED;
    }

    status = doca_pe_connect_ctx(pe, ctx);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to connect pe to ctx: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = doca_ctx_start(ctx);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start ctx: %s", doca_error_get_descr(status));
        return status;
    }

    return DOCA_SUCCESS;
}

doca_error_t
lz4_decomp_resources::open_dev()
{
    doca_error_t status = DOCA_SUCCESS;

    doca_devinfo** devinfo_list;
    uint32_t nb_devs;

    status = doca_devinfo_create_list(&devinfo_list, &nb_devs);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create devinfo list: %s",
                     doca_error_get_descr(status));
        return status;
    }

    /* Simply choose the first device, that should work */
    status = doca_dev_open(devinfo_list[0], &dev);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to open dev: %s", doca_error_get_descr(status));
    }

    doca_devinfo_destroy_list(devinfo_list);
    return status;
}