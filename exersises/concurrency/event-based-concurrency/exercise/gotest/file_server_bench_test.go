// Benchmarks for all file-server implementations.
//
// Each benchmark is parameterised over every configured server binary so the
// results can be compared side-by-side with a tool like benchstat.
//
// Quick run (5 s per sub-benchmark):
//
//	go test -bench=BenchmarkFile -benchmem -benchtime=5s -run='^$'
//
// Collect data for statistical comparison (5 samples):
//
//	go test -bench=. -benchmem -count=5 -run='^$' | tee bench.txt
//	benchstat bench.txt
//
// Run only one server (C file-server):
//
//	TEST_FILE_SERVERS=../file-server go test -bench=. -benchmem -run='^$'
//
// Benchmarks
// ──────────
//   BenchmarkFileServeSeq         – single persistent conn, sequential requests,
//                                   small file (fits in cache after first hit)
//   BenchmarkFileServeLarge       – same but with a 128 KB file that exceeds the
//                                   64 KB cache limit → always reads from disk
//   BenchmarkFileServeConcurrent  – GOMAXPROCS goroutines, each with its own
//                                   persistent connection (use -cpu 1,2,4,8,16)
//   BenchmarkFileCacheHitVsMiss   – side-by-side hit (small file) vs miss
//                                   (large file) on the same server instance

package selectserver

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"
)

// ── constants ─────────────────────────────────────────────────────────────────

const (
	benchSmallFile = "hello.txt"    // well under the 64 KB cache limit
	benchMedFile   = "pangrams.txt" // still cached, slightly larger
	benchLargeFile = "bench-large.bin"
	benchLargeSize = 128 * 1024 // 128 KB > MAX_CACHE_FILE_SIZE(64 KB) → no cache
)

// ── low-level connection ──────────────────────────────────────────────────────

// benchConn is a thin wrapper around a single TCP connection.
// Unlike fileConn.Do it does NOT print the body to stderr, which would
// otherwise dominate the benchmark timing.
type benchConn struct {
	conn net.Conn
	r    *bufio.Reader
}

func newBenchConn(b *testing.B, port int) *benchConn {
	b.Helper()
	c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 2*time.Second)
	if err != nil {
		b.Fatalf("dial: %v", err)
	}
	return &benchConn{conn: c, r: bufio.NewReaderSize(c, 64*1024)}
}

func (bc *benchConn) close() { bc.conn.Close() }

// request sends one file request, reads the status line, drains the body into
// io.Discard, and returns the number of body bytes received.  No heap
// allocation for the body itself.
func (bc *benchConn) request(b *testing.B, filename string) int64 {
	b.Helper()
	bc.conn.SetWriteDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	if _, err := fmt.Fprintf(bc.conn, "%s\n", filename); err != nil {
		b.Fatalf("write: %v", err)
	}
	bc.conn.SetReadDeadline(time.Now().Add(5 * time.Second)) //nolint:errcheck
	line, err := bc.r.ReadString('\n')
	if err != nil {
		b.Fatalf("read status: %v", err)
	}
	status := strings.TrimRight(line, "\r\n")
	if !strings.HasPrefix(status, "+OK ") {
		b.Fatalf("unexpected status %q for %q", status, filename)
	}
	size, err := strconv.ParseInt(strings.TrimPrefix(status, "+OK "), 10, 64)
	if err != nil {
		b.Fatalf("parse size from %q: %v", status, err)
	}
	if size > 0 {
		if _, err := io.CopyN(io.Discard, bc.r, size); err != nil {
			b.Fatalf("drain body: %v", err)
		}
	}
	return size
}

// ── fixtures ──────────────────────────────────────────────────────────────────

// ensureLargeFixture creates a synthetic binary file of benchLargeSize bytes
// in docroot if it does not already exist, and registers a cleanup to remove
// it when the benchmark finishes.
func ensureLargeFixture(b *testing.B) {
	b.Helper()
	p := filepath.Join(docroot, benchLargeFile)
	if _, err := os.Stat(p); err == nil {
		return // already present from a previous run
	}
	data := make([]byte, benchLargeSize)
	for i := range data {
		data[i] = byte(i & 0xff)
	}
	if err := os.WriteFile(p, data, 0o644); err != nil {
		b.Fatalf("create large fixture: %v", err)
	}
	b.Cleanup(func() { os.Remove(p) }) //nolint:errcheck
}

// ── per-implementation harness ────────────────────────────────────────────────

// benchEachServer starts one server per configured binary and runs f as a
// sub-benchmark named after the binary.  Accepts testing.TB-compatible helpers
// so it works from both *testing.B sub-benchmarks and the top level.
func benchEachServer(b *testing.B, f func(b *testing.B, port int)) {
	b.Helper()
	for _, bin := range fileServerBinaries() {
		bin := bin
		b.Run(serverLabel(bin), func(b *testing.B) {
			port := nextPort()
			cmd := startFileServer(b, bin, port)
			b.Cleanup(func() { stopServer(b, cmd) })
			f(b, port)
		})
	}
}

// ── benchmarks ────────────────────────────────────────────────────────────────

// BenchmarkFileServeSeq measures the round-trip latency for a single client
// making sequential requests on one persistent connection.
//
// The file (hello.txt) fits inside the 64 KB cache limit, so after the first
// request every subsequent iteration is a pure cache-hit.  This benchmark
// captures the combined cost of: protocol parsing + response framing +
// cache lookup + TCP send + bufio drain.
func BenchmarkFileServeSeq(b *testing.B) {
	fileSize := int64(len(fixture(b, benchSmallFile)))
	benchEachServer(b, func(b *testing.B, port int) {
		bc := newBenchConn(b, port)
		defer bc.close()

		b.SetBytes(fileSize)
		b.ReportAllocs()
		b.ResetTimer()

		for b.Loop() {
			bc.request(b, benchSmallFile)
		}
	})
}

// BenchmarkFileServeLarge measures sequential requests for a 128 KB file that
// exceeds the cache limit, so the server reads from disk on every iteration.
// Comparing this with BenchmarkFileServeSeq shows the cache speedup.
func BenchmarkFileServeLarge(b *testing.B) {
	ensureLargeFixture(b)
	benchEachServer(b, func(b *testing.B, port int) {
		bc := newBenchConn(b, port)
		defer bc.close()

		b.SetBytes(benchLargeSize)
		b.ReportAllocs()
		b.ResetTimer()

		for b.Loop() {
			bc.request(b, benchLargeFile)
		}
	})
}

// BenchmarkFileServeConcurrent saturates each server with GOMAXPROCS goroutines
// (adjustable via -cpu 1,2,4,8,16).  Each goroutine maintains its own
// persistent connection so there is no connection-setup overhead in the loop.
//
// This benchmark reveals how well each server's event loop (or async runtime)
// scales under concurrent load.
func BenchmarkFileServeConcurrent(b *testing.B) {
	fileSize := int64(len(fixture(b, benchMedFile)))
	benchEachServer(b, func(b *testing.B, port int) {
		b.SetBytes(fileSize)
		b.ReportAllocs()
		b.ResetTimer()

		b.RunParallel(func(pb *testing.PB) {
			// Each goroutine gets its own connection — no sharing, no locks.
			bc := newBenchConn(b, port)
			defer bc.close()
			for pb.Next() {
				bc.request(b, benchMedFile)
			}
		})
	})
}

// BenchmarkFileCacheHitVsMiss runs a hit sub-benchmark (small file, always
// cached) and a miss sub-benchmark (large file, never cached) against the
// same server instance.  The ratio shows the value of the cache.
func BenchmarkFileCacheHitVsMiss(b *testing.B) {
	ensureLargeFixture(b)
	smallSize := int64(len(fixture(b, benchSmallFile)))

	for _, bin := range fileServerBinaries() {
		bin := bin
		b.Run(serverLabel(bin), func(b *testing.B) {
			port := nextPort()
			cmd := startFileServer(b, bin, port)
			b.Cleanup(func() { stopServer(b, cmd) })

			// Use one persistent connection for both sub-benchmarks.
			bc := newBenchConn(b, port)
			defer bc.close()

			b.Run("hit", func(b *testing.B) {
				b.SetBytes(smallSize)
				b.ReportAllocs()
				b.ResetTimer()
				for b.Loop() {
					bc.request(b, benchSmallFile)
				}
			})

			// Flush the cache so the miss benchmark always hits the disk.
			if err := cmd.Process.Signal(syscall.SIGUSR1); err != nil {
				b.Logf("SIGUSR1: %v (cache flush skipped)", err)
			}
			time.Sleep(30 * time.Millisecond)

			b.Run("miss", func(b *testing.B) {
				b.SetBytes(benchLargeSize)
				b.ReportAllocs()
				b.ResetTimer()
				for b.Loop() {
					bc.request(b, benchLargeFile)
				}
			})
		})
	}
}

// BenchmarkConcurrencyScaling sweeps from 1 to 32 concurrent persistent
// connections and reports aggregate throughput (ns/op and MB/s) for each
// server at each concurrency level.
//
// Running this benchmark reveals the crossover point at which an async server
// starts outperforming a synchronous one:
//
//	go test -bench=BenchmarkConcurrencyScaling -benchmem -benchtime=3s -run='^$'
//
// Expected pattern
// ────────────────
//
//	concurrency=1   → sync C wins  (lower async overhead)
//	concurrency=4   → roughly equal
//	concurrency≥8   → Tokio pulls ahead and keeps scaling
//	concurrency≥32  → select()-based servers plateau (single-threaded limit)
//
// The C select() and Rust select() servers are single-threaded: every
// connection competes for the same event-loop iteration.  Throughput
// saturates around 1 CPU core worth of work.
//
// Tokio uses a work-stealing multi-threaded scheduler (one OS thread per
// logical CPU), so throughput keeps climbing until all cores are busy.
func BenchmarkConcurrencyScaling(b *testing.B) {
	for _, clients := range []int{1, 4, 8, 16, 32} {
		clients := clients
		b.Run(fmt.Sprintf("%02d-clients", clients), func(b *testing.B) {
			benchEachServer(b, func(b *testing.B, port int) {
				// Pre-connect all clients before starting the timer.
				conns := make([]*benchConn, clients)
				for i := range conns {
					conns[i] = newBenchConn(b, port)
				}
				defer func() {
					for _, c := range conns {
						c.close()
					}
				}()

				b.SetBytes(int64(len(fixture(b, benchSmallFile))))
				b.ReportAllocs()
				b.ResetTimer()

				// Distribute b.N requests among `clients` goroutines.
				// Each goroutine owns exactly one connection so there is
				// no cross-connection locking or head-of-line blocking.
				var remaining atomic.Int64
				remaining.Store(int64(b.N))
				var wg sync.WaitGroup
				for _, c := range conns {
					c := c
					wg.Add(1)
					go func() {
						defer wg.Done()
						for remaining.Add(-1) >= 0 {
							c.request(b, benchSmallFile)
						}
					}()
				}
				wg.Wait()
			})
		})
	}
}
