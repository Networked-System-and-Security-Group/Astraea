# Astraea 待修改事项

本文档记录代码审查中发现的所有问题，以及当前修复状态。

---

## 已完成修复

| # | 问题 | 涉及文件 |
|---|------|---------|
| 1 | `kMaxNbApps` 从 2 改为 4 | `src/broker/shm.h:7` |
| 2 | 调度器 `i ^ 1` 改为遍历所有其他 app 的 vioTimes | `src/scheduler/Scheduler.cc:137` |
| 6 | `Ec::connectToPe` 悬空指针：`mIsStopped` 改为 `mPe` + `mIsStoppedIdx` | `src/broker/Ctx.h`, `src/broker/Ec.cc:168,186` |
| 7 | `getenv("SLA")` 无 null 检查改为 Initializer 内检查，失败则 exit | `src/broker/initialize.cc`, `src/broker/common.h:15` |
| 21 | DMA profiling 工具已存在并可构建，原待办过期 | `test/profiling/dma/main.cc`, `CMakeLists.txt` |
| 22 | DMA broker wrapper 从空实现补齐为可拆分、可调度实现，并加入 per-accelerator 配额 | `src/broker/Dma.*`, `src/broker/Pe.cc`, `src/broker/shm.h`, `src/scheduler/Scheduler.*`, `CMakeLists.txt` |
| 23 | EC create 拆分未限制 dst buffer pool 且未写入 `stripId`，可能导致 copy-back 错误 | `src/broker/Ec.cc` |

---

## 待修复：实现层面

### #8 `gForceQuit` 非原子变量，被信号处理器写入（数据竞争）

- **文件**：`src/scheduler/Scheduler.cc:52`
- **当前**：`bool gForceQuit = false;`
- **修改**：改为 `std::atomic<bool> gForceQuit = false;`（需 include `<atomic>`，文件里已有）
- **注意**：同样的问题存在于 `src/test/cdn_dpu/main.cc` 和 `src/test/replica_dpu/main.cc` 中的 `extern bool gForceQuit`，相关文件也需要同步修改声明和读取方式

### #9 `shm_open` / `ftruncate` 返回值未检查

- **文件**：`src/scheduler/Scheduler.cc:18-19`
- **当前**：
  ```cpp
  mShmFd = shm_open(kShmName, O_CREAT | O_RDWR, 0666);
  ftruncate(mShmFd, sizeof(SharedData));
  ```
- **修改**：加检查，`shm_open` 返回 -1 时打印错误并 return；`ftruncate` 返回非零时同理

### #10 App 退出后 `nbApps` 不减，重启时 AppId 越界

- **文件**：`src/broker/initialize.cc`
- **问题**：`Initializer::~Initializer()` 不减 `nbApps`，重启单个 app 而不重启 Scheduler 会导致 `gAppId >= kMaxNbApps`
- **修改**：在析构函数里加：
  ```cpp
  LockHelper lockHelper;
  lockHelper.lock(gSharedData->nbAppsLock);
  gSharedData->nbApps--;
  lockHelper.unlock(gSharedData->nbAppsLock);
  ```
- **注意**：这只能保证干净退出的情况；crash 后仍需重启 Scheduler

### #11 调试字符串遗留在生产代码

- **文件**：`src/test/replica_dpu/core.cc:162`
- **当前**：`DOCA_LOG_INFO("Fuck");`
- **修改**：改为有意义的日志，例如：
  ```cpp
  DOCA_LOG_WARN("request size %zu exceeds kMaxDataSize, clamping", rounded);
  ```

### #12 Trace 文件路径硬编码

- **文件**：`src/test/replica_dpu/core.cc:293`
- **当前**：`aRscs.requests = load_requests_text("./ali.txt");`
- **修改**：将 trace 文件路径加入 `ReplicaCfg` 结构体，通过 CLI 参数传入（参考 `ReplicaCfg.ibdevName` 的处理方式，在 `src/test/replica_dpu/main.cc` 中用 `doca_argp` 注册该参数）

### #13 Microbenchmark 设备名硬编码

- **文件**：`src/microbenchmark/main.cc:82`
- **当前**：`openDev("mlx5_0", dev)`
- **修改**：通过 `argv[2]` 或 `doca_argp` 传入设备名，与其他工具保持一致

### #14 Task.cc 多余分号

- **文件**：`src/broker/Task.cc:17`
- **当前**：`dlsym(RTLD_NEXT, "doca_task_submit")); ;`（行末多一个分号）
- **修改**：删掉多余的 `;`

---

## 待修复：工程化层面

### #15 `CHECK_RETURN` / `CHECK_LOG` 宏重复定义

- **文件**：`src/common/common.h:14-34` 和 `src/broker/common.h:21-40`
- **问题**：两份定义完全相同
- **修改方案**：将宏提取到公共头文件（如 `src/common/common.h`），broker 层的 `src/broker/common.h` 改为 include 它，删除 broker 侧的重复定义
- **注意**：broker 的 `common.h` 还有 `extern gSla` 等声明，不能直接合并，需要拆开

### #16 `u8`/`u16`/`u32`/`u64` 类型别名在 5+ 处重复定义

- **涉及文件**：
  - `src/scheduler/Scheduler.h:9`（只有 u32）
  - `src/broker/common.h:10-13`
  - `src/common/common.h:8-10`（u8/u16/u32）
  - `src/test/cdn_dpu/cdn_dpu.h:14`（只有 u32）
  - `src/test/replica_dpu/replica_dpu.h:15`（只有 u32）
- **修改方案**：在 `src/common/common.h` 中统一定义 u8/u16/u32/u64，其他文件通过 include 获取，删除各自的重复定义

### #17 `LockHelper` 不是 RAII，名字误导

- **文件**：`src/broker/shm.h:26-33`
- **问题**：有 `lock()` 和 `unlock()` 两个方法，但析构时不自动解锁，不符合 RAII 语义
- **修改方案**：改为真正的 RAII guard：
  ```cpp
  class ScopedSpinLock {
      std::atomic_flag &mFlag;
  public:
      explicit ScopedSpinLock(std::atomic_flag &f) : mFlag(f) {
          while (mFlag.test_and_set(std::memory_order_acquire));
      }
      ~ScopedSpinLock() { mFlag.clear(std::memory_order_release); }
  };
  ```
  然后把所有 `lockHelper.lock(...); ... lockHelper.unlock(...)` 替换为 `ScopedSpinLock lk(...)` 作用域块
- **涉及文件**：`src/broker/shm.h`、`src/broker/Ec.cc`（createSubtaskSuccCb、recoverSubtaskSuccCb）、`src/broker/Pe.cc`（worker）、`src/broker/initialize.cc`、`src/scheduler/Scheduler.cc`

### #18 中文注释混杂在英文代码库中

- **文件**：`src/test/replica_dpu/core.cc:36-47, 63-93`
- **修改**：将注释翻译为英文或直接删除（逻辑本身不复杂，无需注释）

### #19 Scheduler 使用 `printf`，其余代码使用 `DOCA_LOG_*`

- **文件**：`src/scheduler/Scheduler.cc:56-57`，`src/scheduler/main.cc:11,13`
- **修改**：改用 `printf` → `DOCA_LOG_INFO`（需在 Scheduler.cc 添加 `DOCA_LOG_REGISTER(ASTRAEA:SCHED)`，main.cc 中调用 `registerLogger`）

### #20 `scripts/alias.sh` 中 SLA 值硬编码

- **文件**：`scripts/alias.sh`
- **修改**：改为从环境变量读取，提供默认值：
  ```bash
  SLA_CDN=${SLA_CDN:-269}
  SLA_REPLICA=${SLA_REPLICA:-1250}
  alias ac="taskset -c 1-3 env LD_PRELOAD=./build/src/broker/libastraea_broker.so SLA=$SLA_CDN ./build/src/test/cdn_dpu/cdn_dpu -r 50000"
  alias ar="taskset -c 4-6 env LD_PRELOAD=./build/src/broker/libastraea_broker.so SLA=$SLA_REPLICA ./build/src/test/replica_dpu/replica_dpu"
  ```

### #24 `Buf` wrapper 缺少 data pointer 元数据

- **文件**：`src/broker/Buf.h`, `src/broker/Buf.cc`
- **问题**：wrapper 只记录 `mAddr` 和 `mDataLen`，没有记录 DOCA buf 的 data pointer。当前 DMA 拆分假设测试代码里的 data 与 head 对齐；如果未来应用使用 `doca_buf_set_data()` 或 `buf_get_by_args()` 让 data != head，DMA 子任务切片会从错误地址开始。
- **修改方向**：在 `Buf` 中增加 `mData`，在 `doca_buf_inventory_buf_get_by_args()`、`doca_buf_set_data()`、`doca_buf_set_data_len()` wrapper 中同步维护。

### #25 EC 子任务完成回调用 `isLast` 判断原任务完成，可能受 completion 乱序影响

- **文件**：`src/broker/Ec.cc:createSubtaskSuccCb`, `src/broker/Ec.cc:recoverSubtaskSuccCb`
- **问题**：当前最后提交的子任务完成时就触发用户 callback。如果 DOCA completion 顺序不严格等于提交顺序，可能在其他子任务尚未完成时提前触发原任务完成。
- **修改方向**：仿照 DMA 新实现，在 `EcTaskCreate` / `EcTaskRecover` 中维护完成计数，计数达到 `nbStrips` 时再触发用户 callback。

---

## 设计层面（需与论文对齐，暂不改代码）

### #3 EWMA 预测的对象是上期配额而非实际需求

- **文件**：`src/scheduler/Scheduler.cc:125-126`
- **问题**：`predictions[i] = kEwmaCoef * usage + (1 - kEwmaCoef) * mAllocations[i]`，`mAllocations[i]` 是上期配额，被限流的 app 实际需求会被持续低估
- **可能方向**：broker 额外上报 "队列积压长度" 或 "被阻塞次数" 作为需求代理；或在 SharedData 中增加 `demand` 字段，由 broker 在任务提交时更新

### #4 EC cost model 双负因子编码，物理含义反直觉

- **文件**：`src/broker/Ec.cc:203-215`
- **问题**：`calCreateTimeCostPipeline` 和 `calRecoverTimeCostPipeline` 通过两个负因子相乘得正，数值能用但无法向论文读者直接解释
- **建议**：在论文附录补充回归方法说明；或重新用对数线性模型拟合，保证所有系数为正

### #5 `gLastExpectTime` 全局变量跨 Create/Recover 任务共享，存在竞争

- **文件**：`src/broker/Ec.cc:33, 272, 380`
- **问题**：`EcTaskCreate::submit()` 和 `EcTaskRecover::submit()` 都读写同一个 `gLastExpectTime`，无锁保护
- **修改方向**：改为 `Ec` 类的成员变量（每个 Ec 实例独立的 DDL 水线），或用 `std::atomic` + CAS 更新
