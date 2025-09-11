#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <vector>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

#include "common.h"
#include "doca_types.h"
#include "memory.h"

DOCA_LOG_REGISTER(COMMON : MEMORY);

/* Create and start mmap and buf inventory */
doca_error_t initMemory(size_t aMaxNbBufs, doca_dev *aDev, size_t aMmapSize,
                        void *&aMemAddr, doca_mmap *&aMmap,
                        doca_buf_inventory *&aBufInv) {
    CHECK_RETURN(doca_buf_inventory_create(aMaxNbBufs, &aBufInv),
                 "create buf inventory");
    CHECK_RETURN(doca_buf_inventory_start(aBufInv), "start buf inventory");

    int ret = posix_memalign(&aMemAddr, 64, aMmapSize);
    if (ret) {
        DOCA_LOG_ERR("Failed to alloc memory for mmap");
        return DOCA_ERROR_NO_MEMORY;
    }

    CHECK_RETURN(doca_mmap_create(&aMmap), "create mmap");
    CHECK_RETURN(doca_mmap_set_memrange(aMmap, aMemAddr, aMmapSize),
                 "set mmap memrange");
    CHECK_RETURN(
        doca_mmap_set_permissions(aMmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE |
                                             DOCA_ACCESS_FLAG_RDMA_READ |
                                             DOCA_ACCESS_FLAG_RDMA_WRITE |
                                             DOCA_ACCESS_FLAG_PCI_READ_WRITE),
        "set map permission");
    CHECK_RETURN(doca_mmap_add_dev(aMmap, aDev), "add dev to mmap");
    CHECK_RETURN(doca_mmap_start(aMmap), "start mmap");

    return DOCA_SUCCESS;
}

void destroyMemory(void *aMemAddr, doca_mmap *aMmap,
                   doca_buf_inventory *aBufInv) {
    CHECK_LOG(doca_buf_inventory_destroy(aBufInv), "destroy buf inventory");
    CHECK_LOG(doca_mmap_destroy(aMmap), "destroy mmap");
    free(aMemAddr);
}