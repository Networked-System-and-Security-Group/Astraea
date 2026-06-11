# Astraea

Astraea is a fine-grained, lightweight performance isolation framework for
Data Processing Units (DPUs). Built on NVIDIA DOCA, it isolates and schedules
hardware accelerator resources, such as erasure coding (EC) and DMA, among
co-located applications without requiring modifications to the DPU hardware or
vendor SDK.

Astraea provides three core mechanisms:

- Offline profiling to model the relationship between task parameters and
  accelerator execution time.
- Adaptive task splitting and result reassembly for fine-grained scheduling.
- Workload-guided resource allocation that balances application SLAs,
  fairness, and accelerator utilization.

## Requirements

- NVIDIA BlueField-3 DPU/SuperNIC
- NVIDIA DOCA 3.3
- A compiler with C++20 support
- CMake 3.20 or later
- Ninja or Make

The build system uses `pkg-config` to locate the following DOCA components:
`doca-common`, `doca-argp`, `doca-rdma`, `doca-dma`, and
`doca-erasure-coding`.

## Build

The recommended build generator is Ninja:

```bash
cmake -B build -G Ninja
cmake --build build
```

Make can also be used:

```bash
cmake -B build
cmake --build build
```

All executables and `libastraea_broker.so` are generated directly under
`build/`.

## Project Structure

```text
Astraea/
├── CMakeLists.txt                       # CMake build configuration
├── src/
│   ├── broker/                          # Astraea Broker (AB)
│   │   ├── initialize.cc                # Initialization, SLA parsing, and application registration
│   │   ├── shm.h                        # Shared-memory structures used by AS and AB
│   │   ├── Ctx.cc/.h                    # DOCA context interception and management
│   │   ├── Pe.cc/.h                     # DOCA progress engine interception
│   │   ├── Task.cc/.h                   # Virtual task queues and task submission
│   │   ├── Ec.cc/.h                     # EC task splitting, submission, and result reassembly
│   │   ├── Dma.cc/.h                    # DMA task splitting and submission
│   │   ├── Rdma.cc/.h                   # RDMA call handling
│   │   └── Buf.cc/.h                    # DOCA buffer management
│   └── scheduler/                       # Astraea Scheduler (AS)
│       ├── main.cc                      # AS entry point
│       └── Scheduler.cc/.h              # Resource allocation and granularity adjustment
└── test/
    ├── common/                          # Shared DOCA utilities for test applications
    ├── profiling/
    │   ├── ec/                          # Offline profiling for the EC accelerator
    │   └── dma/                         # Offline profiling for the DMA accelerator
    ├── microbenchmark/                  # Accelerator microbenchmarks
    ├── cdn_client/                      # CDN client
    ├── cdn_dpu/                         # Latency-sensitive CDN DPU application
    ├── replica_client/                  # Replication client
    ├── replica_dpu/                     # Throughput-sensitive replication DPU application
    ├── memscan_host/                    # Memory Scan host component
    ├── memscan_dpu/                     # DMA-based memory scanning DPU application
    ├── localec_host/                    # Local EC host component
    └── localec_dpu/                     # Local encoding DPU application using DMA and EC
```

## Core Components

### Astraea Scheduler (AS)

AS is a global scheduling daemon. It periodically reads resource usage and SLA
violation statistics from each application, computes inter-application
resource allocations independently for accelerators such as EC and DMA, and
dynamically adjusts each application's task-splitting granularity. The current
implementation uses a default scheduling period of 1 ms.

### Astraea Broker (AB)

AB is built as `libastraea_broker.so` and injected into each application
through `LD_PRELOAD`. It intercepts DOCA API calls, maintains per-application
virtual task queues, and performs task splitting, subtask submission, and
result reassembly according to the allocations and granularities selected by
AS. This design preserves DOCA's asynchronous programming model.

### AS-AB Coordination

AS and the AB instance in each application communicate through shared memory.
AS publishes resource allocations and task-splitting granularities. AB enforces
these decisions and reports resource usage and SLA violation statistics back
to AS, forming a continuous scheduling feedback loop.
