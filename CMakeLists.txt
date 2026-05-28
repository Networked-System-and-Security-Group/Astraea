cmake_minimum_required(VERSION 3.20)
project(Astraea VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All outputs land directly in build/, not in nested subdirectories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_compile_definitions(DOCA_ALLOW_EXPERIMENTAL_API)

find_package(PkgConfig REQUIRED)
pkg_check_modules(doca_common REQUIRED IMPORTED_TARGET doca-common)
pkg_check_modules(doca_argp   REQUIRED IMPORTED_TARGET doca-argp)
pkg_check_modules(doca_rdma   REQUIRED IMPORTED_TARGET doca-rdma)
pkg_check_modules(doca_dma    REQUIRED IMPORTED_TARGET doca-dma)
pkg_check_modules(doca_ec     REQUIRED IMPORTED_TARGET doca-erasure-coding)

# ---- Common utility library (shared by test apps, profiling, microbenchmark) ----
add_library(common_lib STATIC
    test/common/arg.cc
    test/common/dev.cc
    test/common/log.cc
    test/common/rdma.cc
    test/common/memory.cc
    test/common/socket.cc
    test/common/ec.cc
)
target_include_directories(common_lib PUBLIC test/common)
target_link_libraries(common_lib PUBLIC
    PkgConfig::doca_common
    PkgConfig::doca_argp
    PkgConfig::doca_rdma
    PkgConfig::doca_dma
    PkgConfig::doca_ec
)

# ---- Astraea Broker (libastraea_broker.so, injected via LD_PRELOAD) ----
add_library(astraea_broker SHARED
    src/broker/Buf.cc
    src/broker/Ctx.cc
    src/broker/Ec.cc
    src/broker/Rdma.cc
    src/broker/Pe.cc
    src/broker/Task.cc
    src/broker/initialize.cc
)
target_include_directories(astraea_broker PRIVATE src/broker)
target_link_libraries(astraea_broker PRIVATE
    PkgConfig::doca_common
    PkgConfig::doca_ec
    PkgConfig::doca_rdma
    dl
)

# ---- Astraea Scheduler ----
add_executable(scheduler
    src/scheduler/Scheduler.cc
    src/scheduler/main.cc
)
target_include_directories(scheduler PRIVATE src/scheduler)

# ---- Microbenchmark ----
add_executable(microbenchmark test/microbenchmark/main.cc)
target_link_libraries(microbenchmark PRIVATE common_lib)

# ---- EC profiling ----
add_executable(ec_prof test/profiling/ec/main.cc)
target_link_libraries(ec_prof PRIVATE common_lib)

# ---- DMA profiling ----
add_executable(dma_prof test/profiling/dma/main.cc)
target_link_libraries(dma_prof PRIVATE common_lib)

# ---- CDN DPU ----
add_executable(cdn_dpu
    test/cdn_dpu/main.cc
    test/cdn_dpu/core.cc
)
target_include_directories(cdn_dpu PRIVATE test/cdn_dpu)
target_link_libraries(cdn_dpu PRIVATE common_lib)

# ---- CDN Client ----
add_executable(cdn_client
    test/cdn_client/main.cc
    test/cdn_client/core.cc
)
target_include_directories(cdn_client PRIVATE test/cdn_client)
target_link_libraries(cdn_client PRIVATE common_lib)

# ---- Replica DPU ----
add_executable(replica_dpu
    test/replica_dpu/main.cc
    test/replica_dpu/core.cc
)
target_include_directories(replica_dpu PRIVATE test/replica_dpu)
target_link_libraries(replica_dpu PRIVATE common_lib)

# ---- Replica Client ----
add_executable(replica_client
    test/replica_client/main.cc
    test/replica_client/core.cc
)
target_include_directories(replica_client PRIVATE test/replica_client)
target_link_libraries(replica_client PRIVATE common_lib)
