# CLAUDE.md — Astraea New Test Applications

This file guides Claude Code when developing two new test applications
for the Astraea evaluation. Submission target: USENIX ATC.

---

## Working Mode

- **Read first, then propose, then code.** Before writing any new code,
  read the existing CDN and Replica applications in full to understand
  their structure, DOCA API usage patterns, and how they integrate with
  the Astraea framework (especially AB interception via LD_PRELOAD).
- **Reuse, don't reinvent.** The new applications should follow the
  same skeleton as CDN/Replica: same threading model, same DOCA context
  setup, same buffer pool management, same shutdown logic. The only
  things that differ are the work each application does and its task
  submission pattern.
- **Stay prototype-grade.** These are evaluation prototypes, not
  production code. Do not add configuration frameworks, logging
  libraries, abstraction layers, or "extensibility hooks." Hard-code
  parameters where reasonable.
- **Commit per milestone.** Each application has 4 milestones (skeleton,
  DOCA wiring, workload generation, end-to-end test). Commit at each
  milestone with a descriptive message. Do not commit broken code.
- **Ask before adding dependencies.** Do not add new external libraries
  beyond what CDN/Replica already use (DOCA SDK, standard C/C++ libs).
  If a trace parser or similar utility seems needed, propose it to the
  human first.

---

## Background Context

The previous SIGCOMM submission was criticized for narrow evaluation:
only two applications (CDN, Replica), both using primarily the erasure
coding (EC) accelerator. Reviewers asked for more diverse applications
and multi-accelerator stress testing. The ATC revision adds two new
applications that exercise the DMA accelerator (which Astraea also
supports but did not previously demonstrate end-to-end).

Astraea fully supports two ASIC accelerators on BlueField-3 via DOCA:

- **EC (erasure coding)** — linear, stateless, fully splittable.
- **DMA** — linear, stateless, fully splittable.

The other two BF3 accelerators (AES-GCM and decompress) cannot be
algorithmically split due to DOCA API limitations (counter / sliding
window state is not exposed). They fall back to task-level rate
limiting and are out of scope for the new applications.

The existing applications and their accelerator usage:

| App | EC | DMA | Network | Workload character |
|-----|----|----|---------|--------------------|
| CDN | ✅ | ❌ | ✅ | Latency-sensitive, trace-driven (Meta CDN) |
| Replica | ✅ | ❌ | ✅ | Throughput-sensitive, trace-driven (Alibaba block) |

The new applications:

| App | EC | DMA | Network | Workload character |
|-----|----|----|---------|--------------------|
| **Memory Scanning** | ❌ | ✅ | ❌ | Periodic bursts, closed-loop |
| **Local Storage Encoding** | ✅ | ✅ | ❌ | Trace-driven (FIU), throughput-oriented |

CDN and Replica remain unchanged. Do not modify them.

---

## Application 1 — Memory Scanning (DMA-only)

### Motivation

Provider-side telemetry/security service running on the DPU. The DPU
periodically scans regions of host memory via DMA and inspects them.
This is a real deployment pattern: NVIDIA DOCA App Shield does
something analogous for host introspection and malware detection. The
inspection itself is trivial (and irrelevant to our evaluation) — what
matters is the DMA traffic pattern: periodic bursts of large DMA reads
from host memory.

### Workload Specification

- **Mode:** Closed-loop with periodic bursts. Every `T_scan_ms`
  milliseconds (default 5 ms), the application submits a burst of
  `N_pages` DMA read tasks, each fetching one "scan region" from host
  memory into DPU memory.
- **Scan region size:** Each scan region is `region_size_KB` KB
  (default 64 KB, configurable to model different scanning
  granularities). Total burst transfers `N_pages × region_size_KB`.
- **Default sizing:** Choose `N_pages` and `region_size_KB` so that
  total burst size is on the order of 4–16 MB — large enough to cause
  observable HOL blocking on the DMA accelerator if Astraea is
  disabled, demonstrating where task splitting matters.
- **Post-DMA "inspection":** Trivial. Just touch each transferred
  region (a memset or single-byte read suffices) to ensure the data
  actually arrived. Do not implement real malware detection or any
  actual inspection logic.
- **Workload source:** Pure synthesis. There is no external trace. The
  burst period and burst size are the workload parameters.

### Implementation Requirements

- Use the same DOCA DMA API pattern as elsewhere in the codebase
  (search for `doca_dma_task_memcpy` or equivalent in the existing
  source).
- Host memory: pre-allocate a single large buffer on the host side
  (matching the same pattern the existing apps use for host buffers).
  Scan regions are offsets into this buffer. Do not implement real
  page-table walks or VM introspection.
- DPU memory: pre-allocate a buffer pool to receive scanned data.
  Reuse buffers across bursts to avoid alloc/free in the hot path.
- Threading: single submission thread plus the DOCA completion
  callback model, matching the existing app skeleton.
- Configurable parameters via command-line flags or environment
  variables (match whichever pattern existing apps use):
  - `T_scan_ms` — burst period
  - `N_pages` — number of regions per burst
  - `region_size_KB` — size of each region
  - Total runtime / target burst count

### Milestones

1. **Skeleton:** Copy the existing app structure. Set up DOCA context,
   buffer pools, signal handlers, shutdown logic. No actual work yet.
   Commit: `memscan: initial skeleton from existing app template`.
2. **DMA wiring:** Submit a single hard-coded DMA task and verify it
   completes. Verify integration with Astraea via LD_PRELOAD works
   (i.e., AB intercepts the call). Commit: `memscan: single DMA task
   round-trip verified`.
3. **Workload generation:** Implement the periodic burst submission
   loop with configurable parameters. Commit: `memscan: periodic burst
   workload`.
4. **End-to-end test:** Run alongside CDN or Replica with Astraea
   enabled. Verify the application produces meaningful throughput
   numbers and that AB scheduling decisions visibly affect timing.
   Commit: `memscan: end-to-end with Astraea verified`.

---

## Application 2 — Local Storage Encoding (EC + DMA)

### Motivation

A provider-side data protection service. The DPU offloads erasure
coding for locally-stored data: it reads data from host memory via
DMA, computes EC redundancy on the DPU, then writes the encoded
output back to host memory via DMA. No network. This models scenarios
like ZFS/Ceph local EC protection, or backup/snapshot encoding,
offloaded to the DPU to reduce host CPU cost.

This application is the first in the evaluation that uses **both EC
and DMA accelerators within a single application's pipeline**. It is
the key vehicle for demonstrating Astraea's ability to manage
multi-accelerator contention inside one application as well as across
applications.

### Workload Specification

- **Mode:** Trace-driven. Each trace request becomes a pipeline of
  three stages: DMA read → EC encode → DMA write.
- **Trace source:** FIU traces (from FIU SyLab, mirrored at the URL
  the user provided — CAMELab archive). Use one of the write-heavy
  traces (e.g., the `casa`, `madmax`, or `webmail` series) since EC
  encoding is meaningful primarily on writes. Pick one trace file at
  setup time; do not switch mid-run.
- **Trace handling — important:** The application uses a single large
  pre-allocated host buffer. Trace request `offset` fields are
  ignored; only `size` (and `timestamp` for arrival timing) are used.
  Read/write operations always target offset 0 of the pre-allocated
  buffer, sized to fit the largest request in the trace.
- **Inter-arrival timing must be preserved.** Use the trace's
  timestamps to control task submission rate. Do not collapse into a
  closed-loop submitter — that would destroy the realism of the
  workload and the point of using a trace.
- **EC parameters:** Use the same EC encoding parameters as Replica
  uses (data block count, redundancy block count, block size). The
  goal is to make the EC workload directly comparable to Replica's,
  differing only in the surrounding pipeline (DMA-bracketed local
  encoding vs. EC + network transmission).
- **Replica trace separation:** Replica uses Alibaba block traces.
  Local Storage Encoding uses FIU traces. They are different trace
  sources, so there is no risk of double-using the same data.

### Implementation Requirements

- Pipeline structure: model after the existing CDN's multi-stage
  pipeline (CDN already chains accelerator operations in DOCA's
  callback model). The pattern is:
  1. Trace driver submits a request at the right time.
  2. DMA read task submitted; on completion, callback enqueues EC
     encode task.
  3. EC encode task submitted; on completion, callback enqueues DMA
     write task.
  4. DMA write completes; request is done.
- Host memory: single large pre-allocated buffer; size it to comfortably
  fit the largest request in the chosen FIU trace.
- DPU memory: buffer pools for both the staging data and the encoded
  output. Reuse buffers across requests.
- Trace parser: write a minimal parser for the FIU trace format. Read
  the entire trace into memory at startup. Do not implement streaming
  parsing — FIU traces are small enough to fit comfortably.
- Configurable parameters:
  - Path to FIU trace file
  - EC data/redundancy block counts and block size (default to match
    Replica)
  - Optional time scaling factor on the trace (lets us replay faster
    than real time if needed for stress)
  - Total runtime

### Milestones

1. **Skeleton:** Copy the existing app structure (especially CDN's
   multi-stage pipeline pattern). DOCA context, buffer pools,
   shutdown logic. Commit: `localec: initial skeleton from CDN
   template`.
2. **Trace parser:** Implement FIU trace parsing as a standalone unit
   with a small test driver. Verify it produces a sensible event
   stream. Commit: `localec: FIU trace parser`.
3. **Pipeline wiring:** Implement the DMA→EC→DMA pipeline with
   hard-coded requests (not yet trace-driven). Verify single-request
   round-trip through all three stages and that AB intercepts each
   accelerator call. Commit: `localec: three-stage pipeline
   round-trip verified`.
4. **End-to-end:** Wire the trace parser to the pipeline submission
   loop with timestamp-based pacing. Verify the application produces
   meaningful throughput when run alongside CDN or Replica with
   Astraea. Commit: `localec: end-to-end with FIU trace and Astraea
   verified`.

---

## Implementation Conventions (Both Applications)

- **File layout:** Place each new application in its own directory
  parallel to the existing CDN and Replica directories. Match
  whichever build system layout (Makefile / CMake / Meson) the
  existing apps use.
- **Naming:** Use clear, short directory names: `memscan/` and
  `localec/` (or whatever convention matches existing apps best —
  inspect first and conform).
- **Headers and code style:** Match the existing code's style exactly
  (brace placement, naming convention, indentation, header comments).
  Do not introduce a new style.
- **Buffer pools and SG lists:** The existing Astraea integration uses
  scatter/gather lists and reusable buffer pools heavily. New apps
  must use the same patterns so that AB's interception works
  correctly. Do not bypass these.
- **Logging:** Match the existing app's logging level and format. No
  new logging frameworks.
- **Configuration:** Match the existing app's parameter-passing
  convention (CLI flags, env vars, or config file). Do not add a new
  configuration mechanism.

---

## Integration with Astraea

Both new applications must work transparently with Astraea via
LD_PRELOAD interception:

- The applications must call DOCA APIs through the normal vendor SDK
  entry points (no direct driver access, no symbol bypass).
- AB must be able to intercept DMA submission and EC submission calls
  for these applications without any application-side cooperation.
- SLA requirements are passed via environment variables at startup
  (same as existing apps).
- Verify integration by running with `LD_PRELOAD=<AB-library>` and
  confirming that AB logs show interception of the new applications'
  task submissions.

If AB interception fails for any new DOCA API the new applications
use, **stop and flag this to the human** rather than working around
it. AB may need to be extended to cover an API path it does not
currently intercept.

---

## Out of Scope

- Any modification to CDN or Replica.
- Any modification to AS, AB, or the Astraea framework itself unless
  explicitly approved (see Integration section above).
- Code polishing for line-count inflation. Real code from the two new
  applications is sufficient.
- Real malware detection logic in Memory Scanning.
- Real filesystem semantics (offset handling, write barriers, etc.) in
  Local Storage Encoding.
- Performance tuning of the new applications beyond what is needed for
  them to produce meaningful workload pressure. The point is to stress
  Astraea, not to make the apps fast in isolation.
- Trace format conversion utilities beyond the minimum needed.
- Visualization / dashboards / monitoring tooling for the new apps.

---

## Experiment Design Reference

The new applications will be used in the following experiments (for
reference — the human will set up and run these, not Claude Code):

1. **Solo-run baselines:** Each new app alone, on native DOCA and on
   Astraea. Measures overhead.
2. **Cross-accelerator independence:** CDN + Memory Scanning (EC vs.
   DMA on separate accelerators). Validates Finding #2 under real
   contention.
3. **Same-accelerator contention:** CDN + Local Storage Encoding (both
   use EC). Companion to existing CDN + Replica.
4. **Multi-app stress:** CDN + Replica + Memory Scanning + Local
   Storage Encoding all co-located. Responds to reviewer request for
   multi-application stress testing.
5. **DMA profiling:** Microbenchmark on the DMA accelerator analogous
   to the EC/AES-GCM profiling already in Appendix A. Feeds into the
   resource model.

Claude Code's job is to make sure the new applications can support
all five experiments — i.e., the apps must be controllable enough
(via parameters) to produce the workload conditions each experiment
needs.

---

## Workflow Summary

For each new application:

1. Read the existing CDN (and Replica) source in full first.
2. Propose the directory layout and file structure before writing.
3. Implement milestone by milestone. Pause and verify after each one.
4. After each milestone, commit and report status to the human.
5. Do not start the next application until the current one is at
   milestone 4 (end-to-end verified) and approved.

If something in this CLAUDE.md is ambiguous or conflicts with what
the existing codebase actually does, **trust the existing codebase**
and flag the discrepancy to the human.