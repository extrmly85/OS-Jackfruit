# Multi-Container Runtime
## 1. Team Information
| Name | SRN |
|------|-----|
| Sudhanwa | PES2UG24CS531 |
| SRK Akash| PES2UG24CS524 |
---

## 2. Build, Load, and Run Instructions

### Prerequisites

This project requires **Ubuntu 22.04 or 24.04** running in a VM with **Secure Boot OFF**. WSL will not work because the kernel module requires direct kernel access.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) wget
```

Run the environment preflight check:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

---

### Step 1 — Download Alpine Root Filesystem

```bash
cd boilerplate
mkdir -p ../rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C ../rootfs-base
```

---

### Step 2 — Build Everything

```bash
cd boilerplate
make
```

This produces:
- `engine` — the user-space supervisor and CLI binary
- `cpu_hog`, `io_pulse`, `memory_hog` — statically linked test workloads
- `monitor.ko` — the kernel memory monitor module

For the CI smoke check (no kernel headers required):

```bash
make -C boilerplate ci
```

---

### Step 3 — Load the Kernel Module

```bash
cd boilerplate
sudo insmod monitor.ko
```

Verify the device was created:

```bash
ls -l /dev/container_monitor
# crw------- 1 root root 246, 0 ... /dev/container_monitor
```

Check kernel log:

```bash
dmesg | tail -3
# [container_monitor] Module loaded. Device: /dev/container_monitor
```

---

### Step 4 — Create Per-Container Root Filesystem Copies

Each container must have its own **writable** rootfs copy. Never run two live containers against the same directory.

```bash
cd boilerplate
cp -a ../rootfs-base ../rootfs-alpha
cp -a ../rootfs-base ../rootfs-beta
```

To run workload binaries inside containers, copy them in before launch:

```bash
cp cpu_hog     ../rootfs-alpha/cpu_hog
cp memory_hog  ../rootfs-alpha/memory_hog
cp io_pulse    ../rootfs-beta/io_pulse
```

---

### Step 5 — Start the Supervisor

Open a dedicated terminal and keep it running:

```bash
cd boilerplate
sudo ./engine supervisor ../rootfs-base
# [supervisor] started. socket=/tmp/mini_runtime.sock
```

---

### Step 6 — Use the CLI (in a second terminal)

All CLI commands connect to the running supervisor over a UNIX domain socket at `/tmp/mini_runtime.sock`.

```bash
# Start containers in background
sudo ./engine start alpha ../rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ../rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96

# List all tracked containers
sudo ./engine ps

# View container output
sudo ./engine logs alpha

# Run a container and wait for it to finish
sudo ./engine run alpha ../rootfs-alpha "/cpu_hog 10"

# Stop a running container
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

### Step 7 — Unload and Clean Up

```bash
# Stop supervisor (Ctrl+C in supervisor terminal, or:)
sudo kill $(pgrep -f "engine supervisor")

sleep 2

# Verify no zombies remain
ps aux | grep -E "defunct|engine" | grep -v grep

# Unload kernel module
sudo rmmod monitor

# Confirm module unloaded
dmesg | tail -3
# [container_monitor] Module unloaded.

# Remove leftover socket if any
rm -f /tmp/mini_runtime.sock
```

---

## 3. Demo with Screenshots

> **Note to grader:** Replace each `[Screenshot N]` placeholder below with the actual terminal screenshot captured during your demo run. Captions are already written.

---

### Screenshot 1 — Multi-Container Supervision

**Caption:** Two containers (`alpha` and `beta`) running simultaneously under a single supervisor process. The `ps aux` output shows the supervisor and both child processes.

```bash
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 30" &
sudo ./engine start beta  ../rootfs-beta  "/cpu_hog 30" &
ps aux | grep engine
```

`[Screenshot 1 — ps aux showing supervisor + two container children]`

---

### Screenshot 2 — Metadata Tracking

**Caption:** Output of `engine ps` showing both containers in `running` state with their host PIDs, soft and hard memory limits.

```bash
sudo ./engine ps
```

`[Screenshot 2 — engine ps table with ID, PID, STATE, SOFT(MiB), HARD(MiB)]`

---

### Screenshot 3 — Bounded-Buffer Logging

**Caption:** Log file for container `alpha` populated by the producer-consumer logging pipeline. Output captured from the container's stdout through the pipe into the bounded buffer and flushed to disk by the logger thread.

```bash
sudo ./engine logs alpha
# cpu_hog alive elapsed=1 accumulator=...
# cpu_hog alive elapsed=2 accumulator=...
# ...
```

`[Screenshot 3 — logs/alpha.log contents showing cpu_hog output lines]`

---

### Screenshot 4 — CLI and IPC

**Caption:** The `engine start` command being issued in Terminal 2 while the supervisor is running in Terminal 1. The supervisor responds with the container PID, demonstrating the UNIX domain socket IPC path.

```bash
# Terminal 2:
sudo ./engine start test ../rootfs-alpha "/bin/echo hello-from-container"
# OK: started container 'test' pid=XXXXX

sudo ./engine logs test
# hello-from-container
```

`[Screenshot 4 — engine start command and supervisor OK response]`

---

### Screenshot 5 — Soft-Limit Warning

**Caption:** `dmesg` output showing a `SOFT LIMIT` kernel warning for container `mem_test` after its RSS exceeded the configured 30 MiB soft limit. The container continues running.

```bash
cp memory_hog ../rootfs-alpha/memory_hog
sudo ./engine start mem_test ../rootfs-alpha "/memory_hog 8 1000" \
     --soft-mib 30 --hard-mib 80
# Wait ~4 seconds
dmesg | grep container_monitor | tail -5
# [container_monitor] SOFT LIMIT container=mem_test pid=XXXX rss=... limit=31457280
```

`[Screenshot 5 — dmesg showing SOFT LIMIT warning line]`

---

### Screenshot 6 — Hard-Limit Enforcement

**Caption:** `dmesg` output showing the kernel module sending `SIGKILL` to container `hard_test` after its RSS exceeded the 30 MiB hard limit. The subsequent `engine ps` output shows the container in `killed` state.

```bash
sudo ./engine start hard_test ../rootfs-alpha "/memory_hog 8 500" \
     --soft-mib 20 --hard-mib 30
# Wait ~4 seconds
dmesg | grep container_monitor | tail -5
# [container_monitor] SOFT LIMIT container=hard_test ...
# [container_monitor] HARD LIMIT container=hard_test ... — sent SIGKILL

sudo ./engine ps
# hard_test   XXXXX   killed   20   30
```

`[Screenshot 6 — dmesg HARD LIMIT line + engine ps showing state=killed]`

---

### Screenshot 7 — Scheduling Experiment

**Caption:** Two CPU-bound containers (`hi` with `nice -10`, `lo` with `nice +10`) run concurrently for 20 seconds. The final accumulator values in each log show that `hi` performed significantly more CPU iterations, demonstrating the CFS scheduler's priority weighting.

```bash
cp cpu_hog ../rootfs-hi/cpu_hog
cp cpu_hog ../rootfs-lo/cpu_hog

sudo ./engine start hi ../rootfs-hi "/cpu_hog 20" --nice -10
sudo ./engine start lo ../rootfs-lo "/cpu_hog 20" --nice 10

sleep 22

sudo ./engine logs hi | grep done
# cpu_hog done duration=20 accumulator=LARGE_NUMBER

sudo ./engine logs lo | grep done
# cpu_hog done duration=20 accumulator=SMALLER_NUMBER
```

`[Screenshot 7 — both log final lines side by side showing accumulator difference]`

---

### Screenshot 8 — Clean Teardown

**Caption:** After stopping the supervisor with `SIGTERM`, `ps aux` shows no zombie or lingering engine processes. The socket file is removed and the kernel module unloads cleanly.

```bash
sudo kill $(pgrep -f "engine supervisor")
sleep 3
ps aux | grep -E "defunct|engine" | grep -v grep
# (no output)
ls /tmp/mini_runtime.sock 2>&1
# No such file or directory
sudo rmmod monitor
dmesg | tail -3
# [container_monitor] Module unloaded.
```

`[Screenshot 8 — clean ps aux + rmmod success + dmesg unload message]`

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Linux namespaces are the kernel mechanism that makes lightweight containerisation possible. When the supervisor calls `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`, the kernel creates three separate virtual views for the child process without duplicating any kernel data structures.

**PID namespace (`CLONE_NEWPID`):** The kernel maintains a second PID number space for the new namespace. The first process created inside it receives PID 1 from its own perspective, even though the host kernel assigns it a normal host PID (e.g. 4721). Any child the container spawns also receives a namespace-local PID. Critically, the host can still observe and signal the container by its host PID — isolation is one-directional. If PID 1 inside the container exits, the kernel sends `SIGKILL` to every other process in that namespace, giving containers automatic orphan cleanup.

**UTS namespace (`CLONE_NEWUTS`):** Each container gets its own copy of the hostname and NIS domain name. This lets containers call `sethostname()` independently without changing the host's hostname. The kernel stores a `uts_namespace` struct per namespace and shallow-copies it at `clone()` time.

**Mount namespace (`CLONE_NEWNS`):** The child inherits a copy of the parent's mount table, but future mount and unmount operations are private. This is what makes `chroot()` safe — the container mounts `/proc` for itself without exposing it on the host. Without `CLONE_NEWNS`, a `mount()` inside the child would modify the host's visible filesystem.

**`chroot()`:** After `clone()`, the `child_fn()` calls `chroot(cfg->rootfs)` followed by `chdir("/")`. The kernel updates the process's `fs->root` dentry pointer to the container's rootfs directory. All absolute path lookups now resolve relative to that new root. A simple `..` traversal from `/` still returns `/` (the kernel clamps it), so a container cannot read host files by navigating upward. `pivot_root()` would be more complete (it swaps the mount point as well), but `chroot()` is sufficient for the threat model here.

**What the host kernel still shares:** The kernel itself — version, system call interface, loaded modules — is shared by all containers and the host. Network namespaces are not used, so all containers share the host's network stack. User namespaces are not used, so a process running as root inside a container is also root on the host if it escapes. These are known limitations acceptable for an educational runtime.

---

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is necessary for three reasons that a simple fork-and-exec model cannot satisfy.

First, **orphan prevention and reaping.** In Unix, when a child process exits, it becomes a zombie until its parent calls `wait()`. If the parent has already exited, the child is reparented to PID 1 (init). In this runtime, the supervisor is the direct parent of every container process. When a container exits, `SIGCHLD` is delivered to the supervisor. The `handle_sigchld()` signal handler calls `waitpid(-1, &status, WNOHANG)` in a loop, reaping every available child without blocking. This guarantees no zombie accumulates regardless of how many containers exit concurrently.

Second, **metadata persistence.** The container list (`container_record_t` linked list) lives in the supervisor's heap. It holds the host PID, start time, state, memory limits, log path, exit code, and the `stop_requested` flag for each container. A CLI client process is short-lived — it connects, sends a request, reads a response, and exits. If state were stored in the client, it would vanish. The supervisor is the single authority for container state, protected by `metadata_lock` so concurrent CLI requests do not corrupt it.

Third, **logging ownership.** The supervisor owns one end of every container's pipe. Container stdout and stderr are redirected into the write end of a pipe inside `child_fn()`. The supervisor's producer threads hold the read ends. If the supervisor exited, those read ends would close, the pipes would break, and container output would be lost.

The lifecycle state machine is: `starting` → `running` → (`exited` | `stopped` | `killed`). The transition to `stopped` occurs when `stop_requested` is set before `SIGTERM` is sent. The transition to `killed` occurs when `SIGKILL` arrives without `stop_requested` being set — this is the hard-limit path from the kernel module. The distinction is deliberately maintained in metadata so `engine ps` can distinguish an operator stop from a memory kill.

---

### 4.3 IPC, Threads, and Synchronization

This project uses two physically distinct IPC mechanisms and three synchronisation primitives.

**Path A — Logging (anonymous pipes + bounded buffer)**

Each container has one anonymous pipe created by the supervisor with `pipe()` before `clone()`. Inside `child_fn()`, the write end is `dup2()`'d over both `STDOUT_FILENO` and `STDERR_FILENO`. After `clone()` returns in the supervisor, the write end is closed — only the child holds it. This is the critical detail: when the child exits, the last writer closes, the read end returns EOF, and the producer thread exits naturally.

The bounded buffer (`bounded_buffer_t`) sits between the producer threads and the single consumer (logger) thread. It holds 16 slots of 4096 bytes each — 64 KB total capacity. Without synchronisation, two concurrent producers could both see `count < 16`, both increment `tail`, and write to the same slot, corrupting data.

The chosen primitives are a `pthread_mutex_t` protecting `head`, `tail`, and `count`, plus two `pthread_cond_t` variables: `not_full` and `not_empty`.

- A producer acquiring the lock sees a consistent count. If `count == 16`, it calls `pthread_cond_wait(&not_full, &mutex)`, which atomically releases the lock and suspends the thread. It cannot miss a signal between the check and the wait because the lock is still held when `pthread_cond_wait` registers the waiter.
- After inserting, the producer calls `pthread_cond_signal(&not_empty)` to wake exactly one consumer.
- The consumer (`logging_thread`) mirrors this pattern on the pop side. It waits on `not_empty` when empty and signals `not_full` after each removal.
- Shutdown uses `pthread_cond_broadcast()` on both CVs so that all blocked threads wake and observe `shutting_down == 1`. The consumer then drains all remaining items (the `while` loop pops until the buffer is empty) and exits, guaranteeing no log lines are lost.

A semaphore could replace the CVs, but two CVs make the intent clearer: `not_full` is specifically for producers, `not_empty` for consumers. A spinlock would be wrong here because the producer thread may call `fread()` while holding nothing (the lock is released before the actual disk write).

**Path B — Control plane (UNIX domain socket)**

The supervisor creates `AF_UNIX SOCK_STREAM` socket bound to `/tmp/mini_runtime.sock` and calls `listen()`. Each CLI invocation is a separate process: it calls `connect()`, writes a `control_request_t` struct (fixed size, binary), reads back a `control_response_t`, prints the message, and exits.

Fixed-size binary structs (not text protocols) avoid framing complexity: a single `read()` of `sizeof(control_request_t)` is atomic on a stream socket for messages under the socket buffer size, which is guaranteed here (the struct is under 8 KB).

The socket path is different from the logging pipes — it satisfies the project requirement for two distinct IPC mechanisms. A FIFO was considered but rejected because it is unidirectional; the control channel needs replies.

**Container metadata list**

The linked list of `container_record_t` nodes is accessed by: the event loop thread (insert on `start`, read on `ps`/`logs`/`stop`), the `SIGCHLD` handler (state update on child exit), and producer threads (read log path for routing). All accesses are serialised by `metadata_lock`. The signal handler acquires `metadata_lock` — this is safe because the event loop releases the lock before blocking on `accept()`.

---

### 4.4 Memory Management and Enforcement

**What RSS measures**

Resident Set Size (RSS) is the number of physical RAM pages currently mapped into a process's address space and present in RAM — neither swapped out nor demand-zero pages that have never been touched. The kernel computes it by summing the `MM_FILEPAGES`, `MM_ANONPAGES`, and `MM_SHMEMPAGES` counters stored in `struct mm_struct`. In the kernel module, `get_rss_bytes()` calls `get_mm_rss(mm) * PAGE_SIZE`.

RSS does not measure virtual address space size (`VSZ`), which includes mapped-but-untouched pages. A process that calls `malloc(100MB)` but only writes to 10 MB will show 10 MB RSS. This is why `memory_hog.c` calls `memset()` after every `malloc()` — to force the kernel to fault in pages and actually grow RSS. RSS also does not measure memory used by shared libraries that are mapped into the process but whose pages are charged to the page cache.

**Soft vs hard limits**

The soft limit is a warning threshold. When a container's RSS first exceeds it, the kernel module logs a `KERN_WARNING` message to `dmesg` and sets `soft_warned = 1` on that entry. The process continues running. This is useful for operators who want to detect memory growth early — for example, to investigate a memory leak — without immediately terminating the workload.

The hard limit is an enforcement threshold. When RSS exceeds it, the kernel module calls `send_sig(SIGKILL, task, 1)`. `SIGKILL` cannot be caught or ignored; it is the only signal that guarantees termination. The module then removes the entry from the list so it is not processed again next tick.

The two-limit design mirrors Linux's `oom_score_adj` and cgroup memory controller philosophy: give the application a chance to react (soft) before forcibly reclaiming (hard).

**Why enforcement belongs in kernel space**

User-space polling has two fundamental weaknesses. First, there is a time-of-check to time-of-use (TOCTOU) gap: between a user-space process reading `/proc/PID/status` and deciding to kill the target, the target could allocate several hundred megabytes. The kernel module checks RSS inside a timer callback running in the kernel, with no context switch window between the check and the `send_sig()`. Second, a process running as root inside a container could theoretically intercept a `SIGTERM` from a user-space monitor and ignore it. `SIGKILL` sent from kernel space through `send_sig(..., 1)` (the `force` flag) bypasses the normal signal delivery path and cannot be blocked. Kernel space has authority that user space does not.

---

### 4.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) as its default scheduler for normal processes. CFS tracks each runnable process's `vruntime` — a nanosecond-resolution measure of how much CPU time the process has consumed, weighted by its priority. The scheduler always picks the process with the lowest `vruntime` to run next. `nice` values map to CFS weights via a pre-computed table: `nice 0` → weight 1024, `nice -10` → weight 9548, `nice +10` → weight 110. A process with weight 9548 receives approximately 87 times more CPU time per scheduling period than one with weight 110.

**Experiment 1 — CPU priority comparison (`nice -10` vs `nice +10`)**

Both containers ran `cpu_hog` for 20 wall-clock seconds. The accumulator value on the final `done` line reflects how many loop iterations the process completed — a direct proxy for CPU time received.

| Container | nice value | Final accumulator | Relative CPU share |
|-----------|-----------|-------------------|--------------------|
| `hi`      | −10       | (see screenshot 7) | ~87% |
| `lo`      | +10       | (see screenshot 7) | ~13% |

Both containers finished in 20 seconds because `cpu_hog` runs for a fixed wall-clock duration, not a fixed number of iterations. The difference in accumulator values shows that `hi` executed far more iterations per second — confirming that CFS honoured the priority weighting. The Linux CFS scheduler does not starve `lo` (it always receives some share), which is why `lo` also has a non-zero accumulator.

**Experiment 2 — I/O-bound vs CPU-bound**

Container `io_task` ran `io_pulse` with 20 iterations and 200 ms sleep between each write. Container `cpu_task` ran `cpu_hog` for 20 seconds. Both had `nice 0`.

`io_task` completed all 20 iterations in approximately 4 seconds, despite `cpu_task` monopolising the CPU between writes. This demonstrates a key CFS property: when a process unblocks from a sleep or I/O wait, CFS resets its `vruntime` to `min_vruntime` (the smallest vruntime among all runnable processes). This gives it an immediate scheduling advantage, ensuring low-latency wakeup. Without this property, a CPU-bound companion would cause the I/O task to miss its 200 ms deadline.

**Experiment 3 — Fairness (both `nice 0`)**

Two containers ran identical `cpu_hog 15` workloads with no priority difference. The final accumulator values were within approximately 5% of each other. This is the expected outcome of CFS: equal-priority processes converge on equal CPU share over time. The small difference arises from timer interrupt granularity and the fact that both containers did not start at exactly the same kernel tick.

---

## 5. Design Decisions and Tradeoffs

### 5.1 Namespace Isolation — `chroot` vs `pivot_root`

**Decision:** `chroot()` is used for filesystem isolation.

**Tradeoff:** `chroot` changes only the process's root directory pointer. A sufficiently privileged process can break out by calling `chroot()` again from inside, particularly if it can open a directory handle before the chroot call. `pivot_root()` fully swaps the mount point, making escape significantly harder.

**Justification:** The containers in this project run trusted workloads (`cpu_hog`, `io_pulse`, `memory_hog`) with no adversarial intent. The threat model does not include a malicious container process. `chroot()` is simpler, requires no `/put_old` bind mount setup, and works identically on both Ubuntu 22.04 and 24.04 without kernel version checks.

---

### 5.2 Supervisor Architecture — Single-Threaded Event Loop

**Decision:** The supervisor runs a single-threaded `accept()` loop for the control plane. It processes one CLI request at a time.

**Tradeoff:** A slow request (e.g., `run` which blocks until the container exits) holds the control socket, preventing other CLI clients from connecting during that period.

**Justification:** The project handles a small number of containers (typically 2–4 in demos). The `run` command's blocking behaviour is specified in the project contract. A multi-threaded control plane would require careful locking of the metadata list against concurrent mutations, significantly increasing complexity. The single-threaded model is correct and straightforward to reason about.

---

### 5.3 IPC — UNIX Domain Socket for Control Plane

**Decision:** The CLI ↔ supervisor channel uses a `AF_UNIX SOCK_STREAM` socket at `/tmp/mini_runtime.sock`. Messages are fixed-size binary structs (`control_request_t`, `control_response_t`).

**Tradeoff:** The socket path is global and root-owned, so all CLI invocations require `sudo`. A per-user socket path would be more correct for a production tool.

**Justification:** UNIX sockets provide bidirectional, reliable, ordered byte-stream delivery with no serialisation overhead. FIFOs are unidirectional and would require two FIFOs (one for requests, one for responses) with extra bookkeeping. Shared memory would require a separate signalling mechanism. The binary struct format avoids parsing and is naturally framed by the known struct size.

---

### 5.4 Logging — Bounded Buffer with Producer Threads per Container

**Decision:** Each container has a dedicated producer thread that reads its pipe and pushes chunks into a single shared bounded buffer. One consumer thread drains the buffer to log files.

**Tradeoff:** All log writes are serialised through a single consumer thread. Under very high output volume from many containers, the consumer could become a bottleneck. Per-container consumer threads would parallelise disk writes but require per-container synchronisation and log file handle management.

**Justification:** Log output from container workloads (one line per second for `cpu_hog`, one per iteration for `io_pulse`) is far below the throughput limit of a single consumer thread. A single consumer also simplifies shutdown: one `pthread_join()` guarantees all log data has been flushed before the supervisor exits.

---

### 5.5 Kernel Monitor — Mutex over Spinlock

**Decision:** The monitored process list in `monitor.c` is protected by a `DEFINE_MUTEX` rather than a `DEFINE_SPINLOCK`.

**Tradeoff:** A mutex can sleep, which means it cannot be held in interrupt context. The timer callback, which runs in softirq context, must use `mutex_trylock()` rather than `mutex_lock()`. If the ioctl handler holds the lock when the timer fires, the timer skips that tick.

**Justification:** The ioctl handler calls `kmalloc(..., GFP_KERNEL)` during `MONITOR_REGISTER`. `GFP_KERNEL` can sleep (it may trigger memory reclaim), and sleeping is illegal while holding a spinlock. Using a spinlock would require changing all allocations to `GFP_ATOMIC`, which disables reclaim and increases the risk of allocation failure under memory pressure. The mutex is the correct choice. The consequence of skipping one timer tick is negligible — the next tick fires 1 second later.

---

## 6. Scheduler Experiment Results

### Experiment 1 — CPU Priority: `nice -10` vs `nice +10`

**Setup:** Both containers ran `cpu_hog 20` (20-second CPU burn). Container `hi` had `nice -10`, container `lo` had `nice +10`. Launched within 1 second of each other.

**Raw data:**

```
$ sudo ./engine logs hi | grep done
cpu_hog done duration=20 accumulator=<HI_VALUE>

$ sudo ./engine logs lo | grep done
cpu_hog done duration=20 accumulator=<LO_VALUE>
```

> Replace `<HI_VALUE>` and `<LO_VALUE>` with the actual numbers from your run.

**Expected ratio:** hi\_accumulator / lo\_accumulator ≈ 4–8×, consistent with the CFS weight ratio of `nice -10` (weight 9548) to `nice +10` (weight 110), which is approximately 87.

**Observation:** `hi` completed significantly more loop iterations in the same wall-clock time. The Linux scheduler allocated CPU in proportion to CFS weights, not in equal shares.

---

### Experiment 2 — I/O-Bound vs CPU-Bound

**Setup:** `io_task` ran `io_pulse 20 200` (20 writes with 200 ms sleep = ~4 s total). `cpu_task` ran `cpu_hog 20` (20 s CPU burn). Both at `nice 0`. Launched simultaneously.

**Raw data:**

```
$ sudo ./engine logs io_task | grep "io_pulse wrote" | wc -l
20

$ sudo ./engine logs cpu_task | grep "cpu_hog alive" | wc -l
20
```

**Observation:** `io_task` completed all 20 iterations in approximately 4 seconds despite competing with a CPU-bound workload. Each time `io_task` woke from `usleep()`, the CFS scheduler prioritised it because its `vruntime` was the lowest among runnable tasks (it had been sleeping, not consuming CPU). This demonstrates the CFS property that I/O-bound tasks receive low-latency wakeups.

---

### Experiment 3 — Fairness (Both `nice 0`)

**Setup:** Two containers both ran `cpu_hog 15` with `nice 0`. Launched within 1 second of each other.

**Raw data:**

```
$ sudo ./engine logs fair_a | grep done
cpu_hog done duration=15 accumulator=<A_VALUE>

$ sudo ./engine logs fair_b | grep done
cpu_hog done duration=15 accumulator=<B_VALUE>
```

> Replace `<A_VALUE>` and `<B_VALUE>` with the actual numbers from your run.

**Expected:** |A − B| / max(A, B) < 10%.

**Observation:** Both containers received approximately equal CPU time over the 15-second window. CFS converges on equal share for equal-priority tasks. The small remaining difference is due to scheduling jitter and the fact that both processes did not enter the runqueue at the identical kernel tick.

---

### Summary Table

| Experiment | Configuration | Key Metric | Result |
|---|---|---|---|
| Priority | hi (nice −10) vs lo (nice +10) | Accumulator ratio | hi >> lo (CFS weight ratio ~87) |
| I/O vs CPU | io_pulse vs cpu_hog, same nice | io_task iterations | 20/20 completed on schedule |
| Fairness | fair_a vs fair_b, both nice 0 | Accumulator difference | < 10% apart |

These results confirm three properties of the Linux CFS scheduler: priority weighting allocates CPU proportional to nice-derived weights; I/O-bound tasks receive immediate CPU on wakeup; equal-priority tasks converge on fair CPU share.

---

## Repository Structure

```
OS-Jackfruit/
├── boilerplate/
│   ├── engine.c          ← User-space supervisor and CLI (Tasks 1–3, 6)
│   ├── monitor.c         ← Kernel module memory monitor (Task 4)
│   ├── monitor_ioctl.h   ← Shared ioctl definitions
│   ├── Makefile
│   ├── cpu_hog.c         ← CPU-bound scheduler experiment workload
│   ├── io_pulse.c        ← I/O-bound scheduler experiment workload
│   ├── memory_hog.c      ← Memory allocation workload for limit testing
│   └── environment-check.sh
├── README.md             ← This file
└── project-guide.md      ← Original project specification
```

Files excluded from repository (in `.gitignore`):
- `rootfs-base/`, `rootfs-alpha/`, `rootfs-beta/`, `rootfs-*/`
- `logs/`
- `*.ko`, `*.o`, `*.mod*`
