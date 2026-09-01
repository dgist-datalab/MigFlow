# MigFlow

## What is MigFlow?

**MigFlow** is a data placement technique for heterogeneous multi-tiered memory
systems (H-NUMA), i.e. multi-socket machines whose sockets combine DRAM with a
slower memory device such as persistent memory or CXL-attached memory. Its
policies are derived from **MigOpt**, a near-optimal offline placement
algorithm formulated as a minimum-cost maximum-flow problem.
To realize these policies efficiently in a practical system, MigFlow employs three lightweight mechanisms:

* **Lowest-latency-first allocation** places new pages in the fastest tier that
  has free space, regardless of socket locality.
* **Quick demotion** moves pages that cooled down right after their burst of
  accesses following allocation directly to the slowest tier.
* **Cost-benefit promotion** promotes a page only when the latency benefit of
  the faster tier outweighs the migration cost.

MigFlow is implemented as a small kernel patch, a kernel module, and a userspace
daemon on a real four-tier machine (DRAM and Intel Optane persistent memory on
two sockets), which enables us to measure execution time, throughput, and
migration traffic of real applications.

The paper that introduces MigOpt and MigFlow is currently under submission to
[EuroSys 2027](https://2027.eurosys.org/).

## Implementation Overview

MigFlow is implemented on top of the [SK hynix HMSDK v2.0](https://github.com/skhynix/hmsdk)
kernel (Linux 6.6.0) and consists of three parts:

* **Kernel patch** (`kernel/`): adds the `MPOL_TOP_DOWN` memory policy for
  lowest-latency-first allocation, adds in-kernel variants of
  `perf_event_open` (taken from [MEMTIS](https://github.com/cosmoss-jigu/memtis))
  and exports the page-table and page-walk helpers the module needs, adds a
  migration rate limiter for the Tiered-AutoNUMA baseline, and resets the
  reclaim-failure state of kswapd when demotion is enabled.
* **Kernel module** (`module/`, `migflow.ko`): `ktrackd` tracks every page the
  target process faults in through a burst queue and a grace queue (the 2Q
  allocation tracker) and reports whether the page cooled down; `kpebsd`
  samples memory accesses of the target with PEBS.
* **Daemon** (`daemon/`, `umigratord`): launches the application, keeps a
  per-page hotness histogram from the module's records, and runs quick
  demotion, cost-benefit promotion, and LFU demotion under an 800 MB / 10 s
  migration budget using `move_pages(2)`.

For evaluation we use **Silo** running **TPC-C** as the workload, and
**Tiered-AutoNUMA** (the memory tiering of the Linux kernel: NUMA balancing
mode 2 with demotion) as the baseline. Tiered-AutoNUMA is the best-performing
baseline on this workload in the paper, so reproducing MigFlow's gain over it
validates the paper's main result on a real system. Both can be run with the
same script.

## Hardware Prerequisites

The hardware requirements for executing MigFlow are as follows.

* **CPU**: An Intel Xeon with **PEBS** (Processor Event-Based Sampling). Our
  evaluation platform uses two Xeon Gold 6252 (Cascade Lake, 24 cores per
  socket); the PEBS event codes in `module/pebs.h` are for this generation.
  PEBS is not available inside virtual machines on this CPU generation, so the
  experiments must run on the bare-metal machine.
* **Memory**: Two sockets, each with DRAM and a slower memory device exposed as
  a NUMA node, forming **exactly four** NUMA nodes. Our platform has **32 GB
  DRAM + 128 GB Intel Optane DC persistent memory** (PMEM) per socket
  (T0 = local DRAM, T1 = remote DRAM, T2 = local PMEM, T3 = remote PMEM).
  The TPC-C workload occupies about **270 GB** at its peak (the database itself
  is 66 GB, but Silo keeps allocating during the run), so it fills all four
  tiers.

## Software Prerequisites

- **OS**: Linux **6.6.0** from HMSDK v2.0 with the MigFlow kernel patch
  (tested on **Ubuntu 22.04**, gcc 11.4)
- **numactl**: HMSDK numactl with the MigFlow `--top-down` patch
- **ndctl / daxctl**: to expose persistent memory as NUMA nodes
- **Silo** (TPC-C) with the MigFlow patch, and its dependencies
  (`libnuma-dev`, `libjemalloc-dev`, `libdb++-dev`)

For artifact evaluation you are given a **user account with sudo** on our
evaluation machine, where the kernel, numactl, and the memory topology are
already set up. In that case, skip to **step 4** of the installation below.
The full setup is described for completeness and for reproducing the
environment on another machine.

## Installation & Compilation

Since the experiments replace the kernel, change the memory topology, and load
a kernel module, a dedicated machine is required. There are five steps to
install and run MigFlow:

1. Build and install the **MigFlow kernel**
2. Build and install **numactl** with `--top-down`
3. Configure the **memory topology**
4. Build the **MigFlow module and daemon**
5. Build the **workload** (Silo TPC-C)

### 0. Install Dependencies

```bash
sudo apt install git build-essential bc bison flex libssl-dev libelf-dev \
     autoconf automake libtool pkg-config ndctl daxctl time \
     libnuma-dev libjemalloc-dev libdb++-dev
```

Clone this repository and remember its location:

```bash
git clone https://github.com/dgist-datalab/MigFlow.git
cd MigFlow
export MIGFLOW=$PWD
```

### 1. Build the MigFlow Kernel

#### 1-1. Clone the HMSDK v2.0 kernel

```bash
cd ~
git clone https://github.com/skhynix/linux.git migflow-linux
cd migflow-linux
git checkout d0a2dd36d6efc565a52c3950e586f5f29acabcb4
git log --oneline -1
```

You should see the commit `d0a2dd3` of the hmsdk-v2.0 kernel.

#### 1-2. Apply the MigFlow patch and build

```bash
git apply $MIGFLOW/kernel/migflow-hmsdk-v2.0.patch
cp $MIGFLOW/kernel/config-6.6.0-migflow .config
make olddefconfig
grep CONFIG_TOP_DOWN .config
make -j$(nproc)
sudo make modules_install install
```

`grep` should print `CONFIG_TOP_DOWN=y`.

#### 1-3. Boot parameter and reboot

Add `memhp_default_state=offline` to `GRUB_CMDLINE_LINUX` in
`/etc/default/grub` so that persistent memory is not onlined as movable memory
automatically, then:

```bash
sudo update-grub
sudo reboot
```

After the reboot, `uname -r` should print `6.6.0-hmsdk2.0` (with a trailing
`+` when the kernel was built inside a git checkout; every later step uses
`uname -r`, so both are fine).

### 2. Build numactl with `--top-down`

```bash
cd ~
git clone https://github.com/skhynix/numactl.git
cd numactl
git checkout 829d172f2206b160e33e2fb1e57ab5c3884902fa
git apply $MIGFLOW/numactl/numactl-top-down.patch
./autogen.sh
./configure --prefix=/usr/local
make -j$(nproc)
sudo make install
sudo ldconfig
/usr/local/bin/numactl 2>&1 | grep top-down
```

The last command should print the usage line `[--top-down= | -K <nodes>]`.

### 3. Configure the Memory Topology

#### 3-1. Create persistent memory namespaces (once)

Create one devdax namespace of 128 GB on the persistent memory region of each
socket:

```bash
sudo ndctl create-namespace --mode=devdax --region=region0 --size=128G
sudo ndctl create-namespace --mode=devdax --region=region1 --size=128G
```

#### 3-2. Set up the tiers (after every boot)

```bash
sudo bash $MIGFLOW/scripts/setup_topology.sh
```

The script exposes the namespaces as NUMA nodes 2 and 3 and trims the DRAM
nodes 0 and 1 to 32 GB. It ends by printing the topology, which should look
like this:

```
available: 4 nodes (0-3)
node 0 size: 31275 MB
node 1 size: 31722 MB
node 2 size: 129024 MB
node 3 size: 129024 MB
```

### 4. Build the MigFlow Module and Daemon

> **On our evaluation server, start here.** The kernel, numactl, and the
> memory topology (steps 1-3) are already set up; clone the repository as in
> step 0 and continue from this step.

```bash
cd $MIGFLOW/module
make
cd $MIGFLOW/daemon
make
```

This produces `module/migflow.ko` and `daemon/umigratord`. The module builds
against the running kernel (`/lib/modules/$(uname -r)/build`).

### 5. Build the Workload (Silo TPC-C)

```bash
cd $MIGFLOW/workloads
git clone https://github.com/stephentu/silo.git
cd silo
git checkout cc11ca1
git apply $MIGFLOW/workloads/silo-migflow.patch
git submodule update --init
make -j$(nproc) MODE=perf dbtest
```

If compilation succeeds, the TPC-C binary is created at
`workloads/silo/out-perf.masstree/benchmarks/dbtest`.

## Execution

To run TPC-C under MigFlow, execute the following from the repository root:

```bash
cd $MIGFLOW
sudo bash scripts/run_tpcc.sh
```

To run the same workload under the **Tiered-AutoNUMA** baseline (the memory
tiering of the Linux kernel):

```bash
sudo bash scripts/run_tpcc.sh tiered
```

The script configures the kernel for the chosen scheme, runs Silo TPC-C
(scale factor 28, 25 M transactions per worker, 24 threads on socket 0),
prints a summary at the end, and restores the kernel settings it changed.

* For **MigFlow** it disables NUMA balancing and demotion, loads `migflow.ko`
  (sampling the CPUs the workload runs on) and starts the workload under
  `umigratord` with `numactl --top-down=0,1,2,3`.
* For **Tiered-AutoNUMA** it enables NUMA balancing mode 2 with demotion
  (`numa_balancing=2`, `demotion_enabled=1`, `zone_reclaim_mode=15`, MGLRU on,
  promotion rate limit 20 MB/s), limits promotions plus demotions to 80 MB/s
  in total, and starts the workload without the daemon.

The full log is written to `results/tpcc-<scheme>-<timestamp>.log`.

In our test environment, one run completes in about **22 minutes** (MigFlow)
or **25 minutes** (Tiered-AutoNUMA). The workload can be scaled with
environment variables, which must be passed through `sudo -E`:

| Variable         | Default    | Meaning                                                    |
|------------------|------------|------------------------------------------------------------|
| `SCALE_FACTOR`   | `28`       | TPC-C warehouses (28: 66 GB database, ~270 GB peak footprint) |
| `OPS_PER_WORKER` | `25000000` | transactions per worker thread                             |
| `NTHREADS`       | `24`       | worker threads, pinned to CPUs `0..NTHREADS-1` of socket 0 |

For example, `OPS_PER_WORKER=5000000 sudo -E bash scripts/run_tpcc.sh` runs
a fifth of the transactions. Note that a short run finishes before the
footprint grows beyond the DRAM tiers, so its numbers are not representative;
use it only to check that everything works. Since the experiments use the
whole machine, run one experiment at a time (the script refuses to start
while another MigFlow run is in progress).

## Code Structure

MigFlow's implementation spans the `module` and `daemon` directories. Below is
a compact overview of key components.

**Kernel module (`migflow.ko`)**

* **`module/tracker.c`** – `ktrackd`: hooks page faults of the target with
  kprobes, keeps the burst and grace queues of the 2Q allocation tracker,
  clears and tests the access bit of each page, adapts the two periods to the
  burst-access pressure, and hands records to the daemon through ring buffers.
* **`module/sampler.c`** – `kpebsd`: opens PEBS events for loads and stores of
  the target on the sampled CPUs and queues the samples for `ktrackd`.
* **`module/pebs.h`** – PEBS event codes (platform specific).
* **`module/perf_internal.h`** – layout of the kernel's perf ring buffer.
* **`include/migflow_rb.h`** – ring buffer records and ioctls shared by the
  module and the daemon.

**Daemon (`umigratord`)**

* **`daemon/profile.cc`** – consumes allocation and sample records, maintains
  the per-page profiles and the access-count histogram, and cools the
  histogram periodically.
* **`daemon/migration.cc`** – quick demotion of cooled pages, the gain-rank
  table and cost-benefit promotion (with the dynamic penalty factor), LFU
  demotion, the per-phase migration budget, and the main loop.
* **`daemon/umigratord.cc`** – command line handling; launches the application
  as a child process.

**Kernel, tools and scripts**

* **`kernel/migflow-hmsdk-v2.0.patch`** – `MPOL_TOP_DOWN` (`mm/mempolicy.c`),
  in-kernel `perf_event_open` for the module (`kernel/events/core.c`), symbol
  exports for the module, the migration rate limiter (`mm/migrate.c`,
  `mm/vmscan.c`) and the kswapd reset (`mm/memory-tiers.c`).
* **`numactl/numactl-top-down.patch`** – `--top-down` option for numactl.
* **`workloads/silo-migflow.patch`** – Silo adaptations for the multi-tier
  setup: no transparent huge pages, no per-thread NUMA pinning, a throughput
  line every 5 seconds.
* **`scripts/run.sh`** – generic runner (MigFlow / Tiered-AutoNUMA) for any
  program; **`scripts/run_tpcc.sh`** – TPC-C wrapper;
  **`scripts/summarize.sh`** – result summary;
  **`scripts/setup_topology.sh`** – topology setup.

## Results

During the experiment, you can observe how MigFlow places new pages in the
fast tiers, demotes cooled pages and promotes hot pages, and at the end you
get the execution time, the TPC-C throughput, and the migration volume.

Everything a run prints on the terminal is also written to a log file in the
**`results/`** directory of the repository, which is created on the first run:

```
results/tpcc-migflow-<timestamp>.log   MigFlow run of run_tpcc.sh
results/tpcc-tiered-<timestamp>.log    Tiered-AutoNUMA run of run_tpcc.sh
results/run-<scheme>-<timestamp>.log   runs of run.sh with other applications
results/jemalloc.stats                 allocator statistics written by Silo
```

The summary is the last block of each log; it can be printed again at any
time with `bash scripts/summarize.sh results/<log file>`.

The following result is from an example run of `sudo bash scripts/run_tpcc.sh`:

* **Setup**: the script records the scheme and the command, loads the module,
  and the daemon reports the application it launched and the initial PEBS
  sampling periods:

```
== scheme: migflow ==
== TPC-C: scale factor 28, 25000000 ops/worker, 24 threads ==
== command: /home/eurosys-ae/MigFlow/workloads/silo/out-perf.masstree/benchmarks/dbtest --verbose --bench tpcc --num-threads 24 --scale-factor 28 --ops-per-worker=25000000 ==
== loading migflow.ko (sample_cpus=0-23) ==
== running ==
umigratord pid 5159, application pid 5160
[Sampling period] loads 701, stores 100003
```

* **Migration phases**: every 10 seconds the daemon prints two lines. `[Alloc]`
  is what the allocation tracker reported in the phase: the number of pages
  that finished the burst and grace periods, how many of them cooled down
  (`cold`), how many cooled pages were accessed again before being demoted
  (`re-accessed`), and the size of the quick-demotion queue of each tier.
  `[Phase]` is how the 800 MB budget was spent: quick demotion before and after
  the promotions, promotions (with the running total), LFU demotion, and the
  current penalty factor `alpha` (4 × tier-0 hit ratio, at least 1.0).

```
[Alloc] pages 646020, cold 531550 (82.3%), warm 114425, re-accessed 60 (0.0%), overflow 526641 | queued T0:800 MB T1:800 MB T2:800 MB
[Phase 1624ms] quick demotion 399 MB, promotion 95 MB (total 3.6 GB), LFU demotion 0 MB, quick demotion 304 MB | 799 of 800 MB used, alpha 3.78
[Alloc] pages 615802, cold 506585 (82.3%), warm 109160, re-accessed 81 (0.0%), overflow 501607 | queued T0:800 MB T1:799 MB T2:800 MB
[Phase 1999ms] quick demotion 399 MB, promotion 95 MB (total 3.7 GB), LFU demotion 0 MB, quick demotion 304 MB | 799 of 800 MB used, alpha 3.78
```

* **Statistics**: when the application exits, the daemon prints the totals of
  the allocation tracker, the profiler and each migration type, including a
  tier-to-tier matrix. Quick demotion sends cooled pages from T0/T1 directly to
  T3; promotions bring hot pages from T2/T3 into T0.

```
=== umigratord statistics ===
allocation tracker: 64001773 pages, cold 48714487, warm 15282898, re-accessed cold 59780
  allocated on tier 0..3: 15910001 13692140 34395243 1
profiler: 57966471 samples, 63538066 profiled pages at peak, 8288753 samples of untracked pages
promotion: iters 330, tried 2688186, moved 2433081, succeeded 2432978 pages
  pages moved from tier (row) to tier (column):
           0         0         0         0
       96044         0         0         0
     1944623         0         0         0
      392311         0         0         0
quick demotion: iters 1104, tried 21027550, moved 21027532, succeeded 21026761 pages
  pages moved from tier (row) to tier (column):
           0         0         0  11183227
           0         0         0   9639193
           0         0         0    204341
           0         0         0         0
LFU demotion: iters 0, tried 0, moved 0, succeeded 0 pages
  pages moved from tier (row) to tier (column):
           0         0         0         0
           0         0         0         0
           0         0         0         0
           0         0         0         0
```

  While a destination tier is completely full, `move_pages(2)` fails for
  the pages headed there and the kernel logs a rate-limited `page allocation
  failure` warning in `dmesg`; the daemon counts those pages as not moved and
  selects them again in a later phase.

* **Workload output**: whatever the application prints stays in the log.
  Silo reports its TPC-C throughput at the end of the run:

```
agg_throughput: 687282 ops/sec
```

* **Summary**: the last lines are the same for every workload and scheme:
  the execution time and the migration volume. (The daemon's exit statistics
  right above them also show where pages were first placed: over the run Silo
  allocated 244 GB, which landed on T0 and T1 first and then on T2 as the DRAM
  tiers filled up — the peak footprint is about 270 GB across the four tiers.)

```
== summary (migflow) ==
execution time         1285.71 s
promotion              9.28 GB (2,432,978 pages)
quick demotion         80.21 GB
LFU demotion           0.00 GB
migration total        89.49 GB
```

The following is the output of `sudo bash scripts/run_tpcc.sh tiered` on the
same machine. For Tiered-AutoNUMA the migration volume comes from the kernel's
counters (`/proc/vmstat`): promotion by NUMA hinting faults and demotion by
reclaim (kswapd), with a budget of 80 MB/s in total.

```
agg_throughput: 518701 ops/sec
== summary (tiered) ==
execution time         1504.05 s
promotion              0.01 GB (NUMA-fault promotion)
demotion               68.10 GB (reclaim demotion)
migration total        68.11 GB
```

MigFlow runs TPC-C about 15 % faster than Tiered-AutoNUMA, the strongest
baseline on this workload, on our platform (1,286 s vs 1,504 s in these runs).
Its migration volume is of the same order as the baseline's (about 89 GB,
almost all of it quick demotion of cooled pages, vs. 68 GB of reclaim
demotion).
Absolute numbers on another machine will differ; run-to-run variation of the
execution time on the same machine is within about 3 %.

## Appendix A: Running Other Applications

`run_tpcc.sh` is a thin wrapper around the generic runner, which runs **any
program** under MigFlow or Tiered-AutoNUMA and produces the same summary:

```bash
sudo bash scripts/run.sh migflow -- <program> [args...]
sudo bash scripts/run.sh tiered  -- <program> [args...]
```

The program is pinned to the cores of socket 0 (`CPUS`, default `0-23`) and,
for MigFlow, started under `umigratord` with `numactl --top-down=0,1,2,3`; the
daemon attaches to the process before it starts, so every allocation is
tracked. Use an application whose memory footprint exceeds the DRAM tiers
(64 GB on our platform) to see migration: pages are placed on T0/T1 first,
cooled pages are demoted, and hot pages are promoted. The summary is the same
for every program — execution time and migration volume — while
workload-specific metrics such as throughput are printed by the program itself
and kept in the log (`results/run-<scheme>-*.log`).

Environment variables of `run.sh` (pass them with `sudo -E`):

| Variable          | Default            | Meaning                                                        |
|-------------------|--------------------|----------------------------------------------------------------|
| `CPUS`            | `0-23`             | CPU list the program is pinned to and MigFlow samples with PEBS |
| `RATE_LIMIT_MBPS` | `80`               | migration budget of Tiered-AutoNUMA (promotion + demotion), MB/s |
| `RESULT_DIR`      | `<repo>/results`   | directory for the logs                                          |
| `LOG_NAME`        | `run-<scheme>`     | prefix of the log file name                                     |
| `LOG_TITLE`       | (none)             | description line written at the top of the log                  |

The module and the daemon can also be used directly:

```bash
sudo insmod module/migflow.ko sample_cpus=0-23
sudo daemon/umigratord -- /usr/local/bin/numactl --cpunodebind=0 --physcpubind=0-23 --top-down=0,1,2,3 -- <program> [args...]
sudo rmmod migflow
```

`umigratord` is used once per `insmod`: it takes over the module when it
starts and releases it when the application exits.

## Appendix B: Parameters

**`umigratord` options** (`daemon/umigratord --help`)

| Option                    | Default | Meaning                                                              |
|---------------------------|---------|----------------------------------------------------------------------|
| `-q`, `--quick-demotion`  | `1`     | quick demotion of pages that cooled down after allocation (0 = off)  |
| `-a`, `--dynamic-alpha`   | `1`     | scale the migration cost of promotions with the tier-0 hit ratio     |
| `-l`, `--alpha-min`       | `1.0`   | lower bound of the dynamic penalty factor alpha                      |
| `-i`, `--print-interval`  | (exit)  | print the statistics every N seconds (also on `SIGUSR1`)             |
| `-v`, `--verbose`         | `0`     | 0 = summary and one line per phase, 1 = every migration step, 2 = debug |

The defaults are the settings used in the paper.

**`setup_topology.sh` variables**

| Variable      | Default         | Meaning                                  |
|---------------|-----------------|------------------------------------------|
| `DRAM_GB`     | `32`            | size each DRAM node is trimmed to (GiB)  |
| `DAX_DEVICES` | `dax0.0 dax1.0` | devdax devices of the PMEM namespaces    |
| `PMEM_NODES`  | `2 3`           | NUMA nodes of the PMEM devices           |
| `DRAM_NODES`  | `0 1`           | NUMA nodes of the DRAM                   |
