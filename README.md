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

## Citation

If you use Astraea for your research, please cite our [paper](https://dl.acm.org/doi/10.1145/3744969.3748397):
```
@inproceedings{peng2025astraea,
    author = {Peng, Qiyang and Zhang, Menghao and Wang, Feiyang and Li, Guanyu and Hu, Chunming},
    title = {Astraea: Enforcing DPU Performance Isolation in Public Clouds},
    year = {2025},
    publisher = {Association for Computing Machinery},
    booktitle = {Proceedings of the ACM SIGCOMM 2025 Posters and Demos},
}
```