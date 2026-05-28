#ifndef MEMORY_H_
#define MEMORY_H_

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_mmap.h>

doca_error_t initMemory(size_t aMaxNbBufs, doca_dev *aDev, size_t aMmapSize,
                        void *&aMemAddr, doca_mmap *&aMmap,
                        doca_buf_inventory *&aBufInv);

void destroyMemory(void *aMemAddr, doca_mmap *aMmap,
                   doca_buf_inventory *aBufInv);

#endif