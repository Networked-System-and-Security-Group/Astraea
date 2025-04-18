#include <cstddef>
#include <cstdint>
#include <cstring>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>

#include "astraea_ctx.h"
#include "astraea_ec.h"
#include "astraea_pe.h"

#include "ec_create.h"

DOCA_LOG_REGISTER(EC_CREATE::RESOURCES);

static void mock_data(void *buffer, size_t length) {
    for (size_t i = 0; i < length; i++) {
        *(static_cast<uint8_t *>(buffer) + i) = i;
    }
}

ec_create_resources::ec_create_resources() {}

ec_create_resources::~ec_create_resources() {
    /* Free tasks */
    for (astraea_ec_task_create *task : tasks)
        astraea_task_free(astraea_ec_task_create_as_task(task));

    /* Destroy ec related resources */
    if (ctx) {
        doca_error_t status = astraea_ctx_stop(ctx);
        /**
         * Make sure to finish the last inflight task
         * before destroy ec and pe
         */
        while (status == DOCA_ERROR_IN_PROGRESS) {
            (void)astraea_pe_progress(pe);
            status = astraea_ctx_stop(ctx);
        }
    }
    if (matrix)
        astraea_ec_matrix_destroy(matrix);
    if (ec)
        astraea_ec_destroy(ec);

    /* Destroy bufs, inventory and mmap */
    for (doca_buf *dst_buf : dst_bufs)
        doca_buf_dec_refcount(dst_buf, nullptr);
    if (src_buf)
        doca_buf_dec_refcount(src_buf, nullptr);
    if (buf_inventory)
        doca_buf_inventory_destroy(buf_inventory);
    if (mmap)
        doca_mmap_destroy(mmap);
    if (mmap_buffer)
        free(mmap_buffer);

    /* Destroy pe */
    if (pe)
        astraea_pe_destroy(pe);

    /* Close device */
    if (dev)
        doca_dev_close(dev);
}

doca_error_t ec_create_resources::prepare_memory(const ec_create_config &cfg) {
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

    size_t data_buf_size = cfg.nb_data_blocks * cfg.block_size;
    size_t rdnc_buf_size = cfg.nb_rdnc_blocks * cfg.block_size;
    size_t mmap_size = data_buf_size + rdnc_buf_size * cfg.nb_tasks;

    int ret = posix_memalign(&mmap_buffer, 64, mmap_size);
    if (ret) {
        DOCA_LOG_ERR("Failed to alloc memory for mmap");
        return DOCA_ERROR_NO_MEMORY;
    }

    /* Moke data on the data blocks */
    mock_data(mmap_buffer, data_buf_size);

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
        buf_inventory, mmap, mmap_buffer, data_buf_size, &src_buf);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to alloc buf for data blocks: %s",
                     doca_error_get_descr(status));
        return status;
    }
    /**
     * Set data buf's begin addr and length
     * The length will be 0 if not doing this
     */
    status = doca_buf_set_data(src_buf, mmap_buffer, data_buf_size);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set data data: %s",
                     doca_error_get_descr(status));
        return status;
    }
    /* Get rdnc bufs */
    for (uint32_t i = 0; i < cfg.nb_tasks; i++) {
        doca_buf *dst_buf;
        status = doca_buf_inventory_buf_get_by_addr(
            buf_inventory, mmap,
            static_cast<uint8_t *>(mmap_buffer) + data_buf_size +
                i * rdnc_buf_size,
            rdnc_buf_size, &dst_buf);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to alloc buf for rdnc blocks: %s",
                         doca_error_get_descr(status));
            return status;
        }
        dst_bufs.push_back(dst_buf);
    }

    return DOCA_SUCCESS;
}

doca_error_t ec_create_resources::setup_ec_ctx(
    astraea_ec_task_create_completion_cb_t success_cb,
    astraea_ec_task_create_completion_cb_t error_cb) {
    doca_error_t status;
    status = astraea_ec_create(dev, &ec);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create ec: %s", doca_error_get_descr(status));
        return status;
    }

    status = astraea_ec_task_create_set_conf(ec, success_cb, error_cb,
                                             MAX_NB_EC_TASKS);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set ec create task conf: %s",
                     doca_error_get_descr(status));
        return status;
    }

    ctx = astraea_ec_as_ctx(ec);
    if (!ctx) {
        DOCA_LOG_ERR("Failed to convert ec to ctx");
        return DOCA_ERROR_UNEXPECTED;
    }

    status = astraea_pe_connect_ctx(pe, ctx);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to connect pe to ctx: %s",
                     doca_error_get_descr(status));
        return status;
    }

    status = astraea_ctx_start(ctx);
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start ctx: %s", doca_error_get_descr(status));
        return status;
    }

    return DOCA_SUCCESS;
}

doca_error_t ec_create_resources::open_dev() {
    doca_error_t status = DOCA_SUCCESS;

    doca_devinfo **devinfo_list;
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