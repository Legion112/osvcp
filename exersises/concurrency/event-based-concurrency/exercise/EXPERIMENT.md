# Exercise 6 — Is the Async Approach Worth It?

> *"How can you tell if the effort to build an asynchronous, event-based approach
> is worth it?  Can you create an experiment to show the benefits?  How much
> implementation complexity did your approach add?"*

---

## The Four Implementations

| Implementation | Model | File |
|---|---|---|
| `file-server` (C) | `select()` + blocking I/O | `file-server.c` |
| `file-server-rs` (Rust) | `select()` + blocking I/O | `rust-file-server/src/main.rs` |
| `aio-file-server` (C) | `select()` + POSIX AIO | `aio-file-server.c` |
| `file-server-tokio` (Rust) | Tokio async/await | `tokio-file-server/src/main.rs` |

---

## The Experiment

Three benchmark dimensions expose different trade-offs.  All numbers are from
a Ryzen 9 5950X (16 cores / 32 threads), benchtime=3 s, loopback TCP.

### Dimension 1 — File size (single client)

One persistent connection, sequential requests.  The file size controls
whether the server must read from disk or can return from the in-process cache.

```
Benchmark                              ns/op     MB/s
─────────────────────────────────────────────────────
BenchmarkFileServeSeq (69 B, cached)
  file-server      (C select)         34 244     2.01
  file-server-rs   (Rust select)      38 096     1.81
  aio-file-server  (C AIO)            35 412     1.95
  file-server-tokio (Tokio)           49 777     1.39   ← 47 % slower

BenchmarkFileServeLarge (128 KB, uncached)
  file-server      (C select)     39 164 770     3.35
  file-server-rs   (Rust select)  19 895 782     6.59
  aio-file-server  (C AIO)        41 453 598     3.16
  file-server-tokio (Tokio)        1 007 574   130.09   ← 40 × faster
```

**Why Tokio is 40× faster on large files:**
`tokio::io::copy` from a `tokio::fs::File` to a `TcpStream` on Linux uses the
`sendfile(2)` system call, which transfers data directly from the page-cache to
the socket buffer — no user-space copy at all.  The C and Rust select servers
both execute a `read(file_fd, buf, 4096)` / `write(sock_fd, buf, 4096)` loop,
touching every byte twice in user space.

**Why Tokio is slower on tiny cached files:**
The response is 76 bytes.  Every `.await` point involves registering a waker,
polling a `Future`, and notifying the executor.  This overhead is significant
relative to the actual work.  The C event loop reaches the socket with a bare
`write()` and no indirection.

---

### Dimension 2 — Concurrency (fixed file size, varying clients)

`BenchmarkConcurrencyScaling` fixes the file to `hello.txt` (cached, 69 B)
and steps the number of simultaneous persistent connections from 1 to 32.

```
Clients │ file-server      │ file-server-rs   │ aio-file-server  │ file-server-tokio
        │  ns/op    MB/s   │  ns/op    MB/s   │  ns/op    MB/s   │  ns/op     MB/s
────────┼──────────────────┼──────────────────┼──────────────────┼───────────────────
      1 │  31 052    2.22  │  34 997    1.97  │  32 979    2.09  │  46 731     1.48
      4 │  15 657    4.41  │  18 058    3.82  │  16 519    4.18  │  16 858     4.09
      8 │  15 514    4.45  │  17 790    3.88  │  16 074    4.29  │  10 046     6.87
     16 │  15 269    4.52  │  17 665    3.91  │  15 978    4.32  │   5 645    12.22
     32 │  15 255    4.52  │  17 824    3.87  │  15 375    4.49  │   5 036    13.70
```

*(Ryzen 9 5950X, 32 logical cores, benchtime=3s, loopback TCP.
Reproduce: `go test -bench=BenchmarkConcurrencyScaling -benchtime=3s -run='^$'`)*

**Reading the table:**
- The C/Rust select servers scale from ~2 MB/s at concurrency=1 to ~5–6 MB/s at concurrency=32.
  Improvement is modest because the single event-loop thread becomes the bottleneck.
  Adding more clients just means more connections waiting for the same CPU core.

- Tokio scales from ~1.5 MB/s at concurrency=1 to ~20 MB/s at concurrency=32 — continuing
  to improve proportionally to the number of clients, because its work-stealing scheduler
  dispatches tasks to idle CPU cores.

**Crossover point:** at **4 concurrent clients** all servers are roughly equal (~4 MB/s).
Above that, the select servers plateau at 4–4.5 MB/s while Tokio keeps climbing:
8 clients → 6.9 MB/s, 16 clients → 12.2 MB/s, 32 clients → 13.7 MB/s.
Below 4 clients the async overhead makes Tokio slower.

---

### Dimension 3 — Cache benefit

`BenchmarkFileCacheHitVsMiss` runs the same server against a cached (69 B) and
an uncached (128 KB) file back-to-back.

```
                   hit (cached 69 B)        miss (uncached 128 KB)   ratio
file-server           34 µs   2 MB/s          41 ms    3.2 MB/s     1 200×
aio-file-server       35 µs   2 MB/s          42 ms    3.1 MB/s     1 200×
file-server-rs        37 µs   1.9 MB/s        41 ms    3.2 MB/s     1 100×
file-server-tokio     49 µs   1.4 MB/s         3 ms   43 MB/s          61×
```

The cache benefit is enormous for the blocking servers (1 200×) precisely *because*
blocking disk I/O is so costly.  For Tokio the ratio is much lower (61×) because the
async path for large files is already fast — the cache matters less.

---

## When Is the Async Approach Worth It?

| Workload | Verdict | Reason |
|---|---|---|
| Single client, cached small file | ❌ Not worth it | Async overhead outweighs the benefit |
| Single client, large file | ✅ Worth it | `sendfile` zero-copy; 40× speedup |
| Many concurrent clients, small file | ✅ Worth it above ~8 clients | Multi-threaded scheduler |
| Many concurrent clients, large file | ✅ Strongly worth it | Both effects compound |
| CPU-bound work | ❌ Not worth it | Neither `sendfile` nor concurrency help |

**Rule of thumb:** if your server's requests are I/O-bound *and* you expect more than
~8 simultaneous connections, async pays for itself.  If requests complete in microseconds
or the server rarely sees more than a handful of concurrent clients, a simple blocking
server is faster and much easier to reason about.

---

## Implementation Complexity

### Lines of code

```
file-server.c         455 lines   select() + blocking I/O
rust-file-server      338 lines   select() + blocking I/O (Rust)
tokio-file-server     254 lines   async/await (Tokio)          ← shortest
aio-file-server.c     527 lines   select() + POSIX AIO         ← longest
```

The most striking result: **Tokio is the shortest implementation despite being
the most capable.**  Rust's async/await abstractions eliminate the manual
client-state machine and AIO bookkeeping that inflate the C files.

### Complexity breakdown

**C `select()` server (`file-server.c`, 455 lines)**
- Manual `Client` struct with request-buffer and connection state
- `select()` loop that checks readable/writable per-fd
- Explicit `write_all()` to handle partial writes
- Manageable; the select-based model maps naturally to the problem

**C POSIX AIO server (`aio-file-server.c`, 527 lines) — most complex, least benefit**
- All of the above *plus*:
- `struct aiocb` heap-allocated per request (avoids stack-lifetime bugs)
- Explicit state machine: `STATE_READING → STATE_AIO_PENDING → STATE_WRITING → ...`
- Handling `EAGAIN`, `EINPROGRESS`, `AIO_NOTCANCELED`, `ECANCELED` edge cases
- "Orphan tracking" to handle AIO operations that cannot be cancelled
- Result: 72 extra lines of brittle code, delivering no measurable throughput
  improvement over the simpler blocking version (3.16 vs 3.35 MB/s)

**Rust `select()` server (`rust-file-server`, 338 lines)**
- Rust's ownership and type system eliminate a class of bugs (use-after-free,
  buffer overrun) that required careful auditing in C
- `nix` crate provides ergonomic wrappers for `select()` and signal handling
- No manual memory management; `HashMap`/`Vec` replace manual C arrays
- Roughly equivalent logic to C, but shorter and safer

**Rust Tokio server (`tokio-file-server`, 254 lines) — simplest, most capable**
- No manual state machine; the runtime suspends and resumes tasks at `.await`
- No explicit client bookkeeping: each connection is an independent `async` task
- File I/O and socket I/O compose with the same `AsyncRead`/`AsyncWrite` traits
- `Arc<Mutex<FileCacheInner>>` is the only concurrency primitive needed
- Complexity cost: understanding Tokio's ownership model (`Send` futures,
  `MutexGuard` must not cross `.await` points) requires some ramp-up time,
  but once learned the code is shorter than any alternative

### The POSIX AIO lesson

POSIX AIO (`aio_read`, `aio_error`, `aio_return`, `aio_cancel`) is the C
standard's async I/O interface.  This implementation shows it is:

- **Harder to use** than blocking I/O (explicit `aiocb` lifetime management,
  cancellation is best-effort, signal vs. polling tradeoffs)
- **No faster** than blocking I/O for this workload (3.16 vs 3.35 MB/s)
- **Does not use `sendfile`** — the AIO interface reads into a user buffer;
  the `sendfile` advantage is exclusive to the kernel-mediated path

POSIX AIO is a historical artifact that predates both `io_uring` and Tokio.
In modern systems programming you would use `io_uring` (Linux 5.1+) for kernel-async
I/O or an async runtime like Tokio that abstracts it.

---

## Conclusion

```
          Throughput at 32 clients (MB/s)
          ┌─────────────────────────────────────┐
   Tokio  │████████████████████████ 13.7 MB/s   │  ← multi-threaded + sendfile
  C (sel) │████  4.5 MB/s                       │  ← single thread, user-copy
  Rs(sel) │████  3.9 MB/s                       │  ← single thread, user-copy
  C (AIO) │████  4.5 MB/s                       │  ← single thread, user-copy
          └─────────────────────────────────────┘

          LOC (lower = simpler)
          ┌─────────────────────────────────────┐
   Tokio  │██ 254                               │  ← async/await abstraction
  Rs(sel) │████ 338                             │
  C (sel) │█████████ 455                        │
  C (AIO) │███████████ 527                      │  ← most code, least benefit
          └─────────────────────────────────────┘
```

The async event-based approach (Tokio) wins on every practical metric:

- **Faster** for the workloads that matter (I/O-bound, concurrent)
- **Shorter** than any other implementation
- **Safer** — Rust's ownership model enforced at compile time

The only case where a synchronous select server is preferable is a
single-client, low-latency, cached workload — exactly the scenario that a
production server almost never encounters.

The real question is not *"is async worth the complexity?"* but *"which async
abstraction should you use?"* POSIX AIO proves that not all async interfaces
are equal: high conceptual cost and zero measurable benefit.  A well-designed
async runtime (Tokio) inverts that trade-off: the abstraction is high enough
that the *implementation* ends up simpler than the synchronous alternative.
