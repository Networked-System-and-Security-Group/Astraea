#include <arpa/inet.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <thread>

#include "cdn_client.h"
#include "common.h"
#include "doca_ctx.h"
#include "memory.h"
#include "rdma.h"
#include "socket.h"

DOCA_LOG_REGISTER(CDN:CLIENT : CORE);

extern bool gForceQuit;

#include <sstream>
#include <string>

std::vector<Request> load_requests_text(const std::string &path) {
    std::ifstream ifs{path};
    if (!ifs) throw std::runtime_error("open for read failed: " + path);

    std::vector<Request> events;
    std::string line;

    // 跳过注释行（以 # 开头）
    auto read_next_data_line = [&]() -> bool {
        while (std::getline(ifs, line)) {
            if (!line.empty() && line[0] == '#') continue;
            if (line.empty()) continue;
            return true;
        }
        return false;
    };

    // 读条数（如果你不想写条数，也可以改为“读到 EOF 为止”）
    if (!read_next_data_line()) return events;
    size_t n = 0;
    {
        std::istringstream iss(line);
        if (!(iss >> n)) throw std::runtime_error("bad count line");
    }
    events.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        if (!read_next_data_line()) throw std::runtime_error("unexpected EOF");
        std::istringstream iss(line);
        uint64_t ts = 0, sz = 0;
        if (!(iss >> ts >> sz))
            throw std::runtime_error("bad data line: " + line);
        events.emplace_back(ts, sz);
    }
    return events;
}

void writeSuccCb(doca_rdma_task_write_imm *task, doca_data task_user_data,
                 doca_data ctx_user_data) {
    if (gForceQuit) {
        doca_task_free(doca_rdma_task_write_imm_as_task(task));
    }
}

void writeErrCb(doca_rdma_task_write_imm *task, doca_data task_user_data,
                doca_data ctx_user_data) {
    // doca_task_free(doca_rdma_task_write_imm_as_task(task));
    doca_error_t status =
        doca_task_get_status(doca_rdma_task_write_imm_as_task(task));
    DOCA_LOG_INFO("%s", doca_error_get_descr(status));
    doca_task_free(doca_rdma_task_write_imm_as_task(task));
    DOCA_LOG_INFO("Write failed fuck");
}

static doca_error_t initTasks(const CdnClientCfg &aCfg, CdnClientRscs &aRscs) {
    CHECK_RETURN(doca_rdma_task_write_imm_allocate_init(
                     aRscs.rdma, aRscs.conn, nullptr, nullptr, 114514,
                     {.u64 = 0}, &aRscs.writeTask),
                 "alloc and init write imm task");
    return DOCA_SUCCESS;
}

doca_error_t init(const CdnClientCfg &aCfg, CdnClientRscs &aRscs) {
    aRscs.requests = load_requests_text("./output.txt");
    CHECK_RETURN(openDev(aCfg.ibdevName, aRscs.dev), "open device");
    /* Server only need pe to built connection */
    CHECK_RETURN(doca_pe_create(&aRscs.pe), "create pe");

    CHECK_RETURN(initMemory(8192, aRscs.dev, aCfg.mmapSize, aRscs.memAddr,
                            aRscs.mmap, aRscs.bufInv),
                 "init memory");

    CHECK_RETURN(initRdma(aCfg.gidIdx, aRscs.dev, aRscs.pe, nullptr, nullptr,
                          nullptr, nullptr, aRscs.rdma, aRscs.ctx),
                 "init rdma");
    CHECK_RETURN(doca_ctx_stop(aRscs.ctx), "stop rdma ctx");
    CHECK_RETURN(doca_rdma_task_write_imm_set_conf(aRscs.rdma, writeSuccCb,
                                                   writeErrCb, 8192),
                 "set write imm conf");
    CHECK_RETURN(doca_ctx_start(aRscs.ctx), "start rdma ctx");

    const void *localConnDesc;
    size_t localConnDescSize;
    CHECK_RETURN(doca_rdma_export(aRscs.rdma, &localConnDesc,
                                  &localConnDescSize, &aRscs.conn),
                 "export connection");

    /* Send connection info using socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{AF_INET, htons(aRscs.port), {INADDR_ANY}};
    int ret = bind(sockfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (ret == -1) {
        DOCA_LOG_ERR("Failed to bind socket");
        close(sockfd);
        return DOCA_ERROR_IO_FAILED;
    }

    DOCA_LOG_INFO("Server %u listening, port is %u", aRscs.threadId,
                  aRscs.port);
    listen(sockfd, 1);

    int c = accept(sockfd, nullptr, nullptr);

    /* 1. receive remote connection info */
    char remoteConnDesc[1024];
    size_t remoteConnDescSize;
    remoteConnDescSize = recvMsg(c, remoteConnDesc);

    /* 2. send local connection info to remote */
    sendMsg(c, static_cast<const char *>(localConnDesc), localConnDescSize);

    /* 3. send local mmap info to remote */
    const void *localMmapDesc;
    size_t localMmapDescSize;
    CHECK_RETURN(doca_mmap_export_rdma(aRscs.mmap, aRscs.dev, &localMmapDesc,
                                       &localMmapDescSize),
                 "export mmap through rdma");
    sendMsg(c, static_cast<const char *>(localMmapDesc), localMmapDescSize);

    close(c);
    close(sockfd);

    CHECK_RETURN(doca_rdma_connect(aRscs.rdma, remoteConnDesc,
                                   remoteConnDescSize, aRscs.conn),
                 "connect to remote");
    DOCA_LOG_INFO("Thread %u connection established", aRscs.threadId);

    CHECK_RETURN(initTasks(aCfg, aRscs), "init tasks");

    return DOCA_SUCCESS;
}

void runTasks(const CdnClientCfg &aCfg, CdnClientRscs &aRscs) {
    DOCA_LOG_INFO("Nb requests is %lu", aRscs.requests.size());
    const uint64_t t0 = aRscs.requests[0].ts_ms;
    aRscs.beginTime = std::chrono::high_resolution_clock::now();
    for (uint32_t i = aRscs.threadId; i < aRscs.requests.size(); ++i) {
        DOCA_LOG_INFO("i is %u", i);
        uint64_t rel_ms = (aRscs.requests[i].ts_ms >= t0)
                              ? (aRscs.requests[i].ts_ms - t0)
                              : 0;
        auto target = aRscs.beginTime + std::chrono::milliseconds(rel_ms);
        std::this_thread::sleep_until(target);

        doca_ctx_states state;
        doca_ctx_get_state(aRscs.ctx, &state);
        bool flag = false;
        while (state == DOCA_CTX_STATE_STOPPING) {
            if (!flag) {
                doca_task_free(
                    doca_rdma_task_write_imm_as_task(aRscs.writeTask));
                flag = true;
            }
            doca_error_t status = doca_ctx_stop(aRscs.ctx);
            if (status != DOCA_SUCCESS) {
                DOCA_LOG_INFO("Failed to stop ctx: %s",
                              doca_error_get_descr(status));
            }
            doca_pe_progress(aRscs.pe);
            DOCA_LOG_INFO("ctx is stopping");
            doca_ctx_get_state(aRscs.ctx, &state);
        }

        if (state == DOCA_CTX_STATE_IDLE) {
            doca_ctx_start(aRscs.ctx);
        }

        doca_ctx_get_state(aRscs.ctx, &state);
        while (state == DOCA_CTX_STATE_STARTING) {
            DOCA_LOG_INFO("ctx is starting");
            doca_pe_progress(aRscs.pe);
        }

        doca_rdma_task_write_imm_set_immediate_data(
            aRscs.writeTask, htonl(aRscs.requests[i].size));

        CHECK_LOG(
            doca_task_submit(doca_rdma_task_write_imm_as_task(aRscs.writeTask)),
            "submit write imm task");
        while (!doca_pe_progress(aRscs.pe)) {
        }

        if (gForceQuit) {
            break;
        }
    }

    while (!gForceQuit) {
        doca_pe_progress(aRscs.pe);
        // std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    while (!aRscs.isFreeded) {
        doca_pe_progress(aRscs.pe);
    }
}

void destroy(CdnClientRscs &aRscs) {
    destroyRdma(aRscs.rdma, aRscs.ctx);
    destroyMemory(aRscs.memAddr, aRscs.mmap, aRscs.bufInv);
    CHECK_LOG(doca_pe_destroy(aRscs.pe), "destroy pe");
    CHECK_LOG(doca_dev_close(aRscs.dev), "close dev");
}
