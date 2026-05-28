# Astraea

Astraea is a fine-grained and lightweight DPU performance isolation framework built on NVIDIA DOCA SDK. It enables performance isolation for multi-tenant applications on DPU accelerators through offline performance modeling, adaptive task splitting, and workload-guided scheduling—without modifications to the hardware or vendor SDK.

## Prerequisites

1. **BlueField-3 DPU/SuperNIC**
2. **DOCA 2.9.x LTS** (only 2.9.x versions are supported; 3.x versions are incompatible)
3. C++ compiler that supports C++20 standard
4. `cmake` (≥ 3.20) and `ninja` (or `make`)

## Build and Run

### Build

```bash
cmake -B build -G Ninja
cmake --build build
```

All binaries and the shared library are placed directly in `build/`:

```
build/
├── libastraea_broker.so   # Astraea Broker (LD_PRELOAD target)
├── scheduler              # Astraea Scheduler daemon
├── cdn_dpu / cdn_client
├── replica_dpu / replica_client
├── microbenchmark
└── ec_prof
```

### Run

`scripts/alias.sh` defines commonly used command aliases for testing:

```bash
# Load aliases
source scripts/alias.sh

# Run the scheduler (Astraea Scheduler)
sc  # taskset -c 7 ./build/scheduler

# Run CDN application with Astraea isolation
ac  # taskset -c 1-3 env LD_PRELOAD=./build/libastraea_broker.so SLA=269 ./build/cdn_dpu -r 50000

# Run Replica application with Astraea isolation
ar  # taskset -c 4-6 env LD_PRELOAD=./build/libastraea_broker.so SLA=1250 ./build/replica_dpu

# Run native DOCA CDN application (no isolation)
dc  # taskset -c 1-3 ./build/cdn_dpu -r 50000

# Run native DOCA Replica application (no isolation)
dr  # taskset -c 4-6 ./build/replica_dpu

# Microbenchmark
ds  # Small tasks (8KB block)
db  # Large tasks (64KB block)
as  # Small tasks with Astraea
ab  # Large tasks with Astraea
```

## Project Structure

```
Astraea/
├── CMakeLists.txt              # Project build configuration
├── scripts/
│   ├── alias.sh                # Common command aliases
│   └── kill.sh                 # Process termination script
├── src/
│   ├── broker/                 # Astraea Broker (AB)
│   │   │                       # Dynamic library injected via LD_PRELOAD
│   │   ├── initialize.cc       # Initialization logic (shared memory, app registration)
│   │   ├── shm.h               # Shared memory data structure definitions
│   │   ├── Ctx.cc/h            # DOCA Context interception and management
│   │   ├── Pe.cc/h             # Progress Engine interception
│   │   ├── Ec.cc/h             # Erasure Coding task interception and splitting
│   │   ├── Task.cc/h           # Task queue and submission management
│   │   ├── Buf.cc/h            # Buffer management and zero-copy optimization
│   │   └── Rdma.cc/h           # RDMA task handling
│   │
│   └── scheduler/              # Astraea Scheduler (AS)
│       │                       # Global scheduling daemon
│       ├── main.cc             # Scheduler entry point
│       └── Scheduler.cc/h      # Scheduling algorithm (EWMA prediction, resource allocation, SLA compensation)
│
└── test/
    ├── common/                 # Common utility library (shared by all test apps)
    │   ├── arg.cc              # Command-line argument handling
    │   ├── dev.cc              # DOCA device management
    │   ├── ec.cc/h             # Erasure Coding accelerator wrapper
    │   ├── memory.cc/h         # Memory management (mmap, buffer inventory)
    │   ├── rdma.cc/h           # RDMA communication wrapper
    │   └── socket.cc/h         # Socket communication utilities
    │
    ├── cdn_client/             # CDN client (latency-sensitive application)
    ├── cdn_dpu/                # CDN DPU-side handler
    ├── replica_client/         # Replica client (throughput-intensive application)
    ├── replica_dpu/            # Replica DPU-side handler
    ├── microbenchmark/         # EC accelerator latency microbenchmark
    └── profiling/
        └── ec/                 # Offline EC accelerator performance modeling
```

### Core Components

- **Astraea Broker (AB)**: A lightweight dynamic library transparently injected into applications via `LD_PRELOAD`. It intercepts DOCA API calls, performs adaptive task splitting, and manages task submission based on scheduler policies.

- **Astraea Scheduler (AS)**: A global scheduling daemon that periodically (default: every millisecond) makes resource allocation decisions. It uses EWMA to predict workload demands, dynamically adjusts task splitting granularity, and ensures long-term fairness through SLA violation compensation.

- **Shared Memory**: Enables efficient communication between AS and AB, conveying resource quotas, usage statistics, and SLA violation information.
