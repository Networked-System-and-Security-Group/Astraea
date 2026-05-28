#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <vector>

#include "common.h"
#include "doca_buf.h"
#include "doca_buf_inventory.h"
#include "doca_erasure_coding.h"
#include "doca_pe.h"
#include "ec.h"
#include "memory.h"

DOCA_LOG_REGISTER(MICROBENCHMARK);

constexpr uint32_t kNbReuse = 10000;
size_t gDataSize = 128 * 65536;
uint32_t gNbFinishedTasks = 0;
uint32_t gNbFreedTasks = 0;

std::vector<std::chrono::high_resolution_clock::time_point> gBeginTimes;
std::vector<std::chrono::high_resolution_clock::time_point> gEndTimes;

struct UserData {
    doca_buf *dstBuf;
    uint32_t reuseTimes;
};

// static void setData(void *addr, size_t len) {
//     for (uint32_t i = 0; i < len; i++) {
//         static_cast<uint8_t *>(addr)[i] = i & (sizeof(uint8_t) - 1);
//     }
// }

static void scb(doca_ec_task_create *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    gEndTimes.push_back(std::chrono::high_resolution_clock::now());
    gNbFinishedTasks++;

    UserData *userData = static_cast<UserData *>(task_user_data.ptr);
    userData->reuseTimes++;

    if (userData->reuseTimes < kNbReuse) {
        doca_buf_set_data_len(userData->dstBuf, 0);
        gBeginTimes.push_back(std::chrono::high_resolution_clock::now());
        doca_task_submit(doca_ec_task_create_as_task(task));
    } else {
        doca_task_free(doca_ec_task_create_as_task(task));
        gNbFreedTasks++;
    }
}
static void ecb(doca_ec_task_create *task, doca_data task_user_data,
                doca_data ctx_user_data) {}

int main(int argc, char **argv) {
    CHECK_RETURN(registerLogger(DOCA_LOG_LEVEL_WARNING), "register logger");
    u32 blkSize = atoi(argv[1]);
    gDataSize = blkSize * 128;
    doca_dev *dev;
    doca_pe *pe;
    doca_ctx *ctx;

    void *memAddr;
    doca_mmap *mmap;
    doca_buf_inventory *inv;
    doca_buf *srcBuf;
    doca_buf *dstBuf;

    doca_ec *ec;
    doca_ec_matrix *matrix, *dummyMatrix;

    CHECK_RETURN(openDev("mlx5_0", dev), "open device");

    CHECK_RETURN(initMemory(8192, dev, gDataSize * 2, memAddr, mmap, inv),
                 "init memory");

    CHECK_RETURN(
        doca_buf_inventory_buf_get_by_data(
            inv, mmap, static_cast<char *>(memAddr), gDataSize, &srcBuf),
        "get src buf");
    CHECK_RETURN(doca_buf_inventory_buf_get_by_addr(
                     inv, mmap, static_cast<char *>(memAddr) + gDataSize,
                     gDataSize, &dstBuf),
                 "get dst buf");

    CHECK_RETURN(doca_pe_create(&pe), "create pe");

    CHECK_RETURN(initEc(dev, pe, 128, 32, nullptr, 0, scb, ecb, nullptr,
                        nullptr, ec, matrix, dummyMatrix, ctx),
                 "init ec");

    doca_ec_task_create *task;

    UserData userData = {.dstBuf = dstBuf, .reuseTimes = 0};

    CHECK_LOG(doca_ec_task_create_allocate_init(ec, matrix, srcBuf, dstBuf,
                                                {.ptr = &userData}, &task),
              "alloc ec create task");

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    int sig;

    DOCA_LOG_INFO("Wait for signal SIGUSR1 to continue");
    sigwait(&mask, &sig);

    gBeginTimes.push_back(std::chrono::high_resolution_clock::now());
    CHECK_LOG(doca_task_submit(doca_ec_task_create_as_task(task)),
              "submit ec create task");

    while (gNbFinishedTasks < kNbReuse) {
        doca_pe_progress(pe);
    }

    CHECK_LOG(doca_buf_dec_refcount(srcBuf, nullptr), "dec src buf ref count");
    CHECK_LOG(doca_buf_dec_refcount(dstBuf, nullptr), "dec dst buf ref count");

    destroyEc(matrix, nullptr, ec, ctx);
    destroyMemory(memAddr, mmap, inv);
    CHECK_LOG(doca_pe_destroy(pe), "destroy pe");
    CHECK_LOG(doca_dev_close(dev), "close device");

    double timeCostSum = 0;
    std::vector<double> timeCosts;
    for (uint32_t i = 0; i < gNbFinishedTasks; i++) {
        double timeCostInUs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(gEndTimes[i] -
                                                                 gBeginTimes[i])
                .count() /
            1000.0;
        timeCostSum += timeCostInUs;
        timeCosts.push_back(timeCostInUs);
        // printf("%f%s", timeCostInUs, i == gNbFinishedTasks - 1 ? "\n" : ",
        // ");
    }

    std::sort(timeCosts.begin(), timeCosts.end());
    DOCA_LOG_INFO("p99 time cost is %f",
                  timeCosts[(gNbFinishedTasks * 99) / 100]);
    DOCA_LOG_INFO("p95 time cost is %f",
                  timeCosts[(gNbFinishedTasks * 95) / 100]);
    DOCA_LOG_INFO("p90 time cost is %f",
                  timeCosts[(gNbFinishedTasks * 90) / 100]);

    DOCA_LOG_INFO("Average time cost is %f", timeCostSum / kNbReuse);
    DOCA_LOG_INFO("blkSize is %u", blkSize);
    if (blkSize == 8192) {
        double sla = 400;
        u32 vioCount = 0;
        for (auto t : timeCosts) {
            if (t > sla) {
                vioCount++;
            }
        }
        DOCA_LOG_INFO("SLA violation rate is %f%%",
                      vioCount * 100.0 / kNbReuse);
    }

    return 0;
}