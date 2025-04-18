# Astraea

The DPU performance isolation framework, built with DOCA.

## Prerequisites

1. BlueField 3 DPU/SuperNIC
2. `DOCA` >= 2.9.1 LTS
3. C++ compiler that supports C++20 standard
4. `meson` and `ninja`

## Build and Run

1. execute `./scripts/build.sh` to build the library and executables
2. execute `./scripts/profile.sh` to run the profiling program
3. execute `./scripts/run.sh d s`, `./scripts/run.sh d b` and test them with `./scripts/test.sh d`
4. execute `./scripts/run.sh a s`, `./scripts/run.sh a b` after `./build/src/scheduler/astraea_scheduler` and test them with `./scripts/test.sh a`
