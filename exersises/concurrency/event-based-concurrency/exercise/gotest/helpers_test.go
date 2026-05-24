// Shared test helpers.
//
// Port allocation
// ---------------
// Tests use nextPort() instead of hardcoded constants so that subtests that
// run in parallel (t.Parallel) never collide.
//
// Server selection
// ----------------
// File-server tests are parameterised over a list of server binaries.
// By default both the C and Rust implementations are tested.
// Override with the TEST_FILE_SERVERS environment variable:
//
//	TEST_FILE_SERVERS=./file-server               go test ./...   # C only
//	TEST_FILE_SERVERS=./file-server,./rust-bin    go test ./...   # both
//
// Similarly, the time-server (select-server) list can be overridden with
// TEST_TIME_SERVERS (defaults to the single C implementation).

package selectserver

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// devnull returns an *os.File pointing to /dev/null for writing.
// It is used to ensure child-process stdout/stderr are silenced without
// inheriting the parent's file descriptors (which in some environments,
// e.g. Cursor IDE, are sockets rather than a terminal or pipe).
func devnull(tb testing.TB) *os.File {
	tb.Helper()
	f, err := os.OpenFile(os.DevNull, os.O_WRONLY, 0)
	if err != nil {
		tb.Fatalf("open /dev/null: %v", err)
	}
	tb.Cleanup(func() { f.Close() })
	return f
}

// ── port allocator ────────────────────────────────────────────────────────────

var portSeed atomic.Int32

func init() { portSeed.Store(19000) }

// nextPort returns a unique TCP port for each caller.
func nextPort() int { return int(portSeed.Add(1)) }

// ── binary lists ──────────────────────────────────────────────────────────────

// fileServerBinaries returns the list of file-server binaries to test.
func fileServerBinaries() []string {
	if v := os.Getenv("TEST_FILE_SERVERS"); v != "" {
		return strings.Split(v, ",")
	}
	return []string{
		"../file-server",
		"../rust-file-server/target/debug/file-server-rs",
		"../aio-file-server",
		"../tokio-file-server/target/debug/file-server-tokio",
	}
}

// timeServerBinaries returns the list of time-server binaries to test.
func timeServerBinaries() []string {
	if v := os.Getenv("TEST_TIME_SERVERS"); v != "" {
		return strings.Split(v, ",")
	}
	return []string{"../select-server"}
}

// serverLabel returns a short human-readable name used in t.Run sub-test names.
func serverLabel(binary string) string {
	return filepath.Base(binary)
}

// serverStderr returns os.Stderr when the caller explicitly opts in by
// setting TEST_VERBOSE_SERVERS=1, otherwise nil (→ /dev/null).
//
// Why not auto-detect a TTY?
//
//	GoLand (and many CI systems) connect the process to a pseudo-terminal
//	(PTY).  A PTY is a character device, so os.ModeCharDevice is set, but
//	its kernel buffer is tiny (~4 KB).  Four servers logging 20 concurrent
//	clients fill it instantly, blocking every server on printf → deadlock.
//
// Usage:
//
//	TEST_VERBOSE_SERVERS=1 go test -v -run TestFile
func serverStderr() *os.File {
	if os.Getenv("TEST_VERBOSE_SERVERS") != "" {
		return os.Stderr
	}
	return nil
}

// ── server lifecycle ──────────────────────────────────────────────────────────

// startServer launches binary on port (with optional extra args), waits up to
// 1 s for the port to be ready, and returns the running *exec.Cmd.
//
// Accepts testing.TB so it can be used from both *testing.T and *testing.B.
func startServer(tb testing.TB, binary string, port int, extraArgs ...string) *exec.Cmd {
	tb.Helper()
	args := append([]string{fmt.Sprintf("%d", port)}, extraArgs...)
	cmd := exec.Command(binary, args...)
	// Route child stdout/stderr to os.Stderr only when TEST_VERBOSE_SERVERS=1.
	// In all other cases use an explicit /dev/null file.
	//
	// Why not just pass nil?  In the Cursor IDE (and similar environments) the
	// parent test process has *sockets* as fd 1 / fd 2 rather than a pipe or
	// terminal.  Go's exec.Cmd with nil Stdout/Stderr inherits the parent's
	// file descriptors for stdout/stderr in that situation.  When the child C
	// server then calls printf()/fprintf(stderr, ...) it accidentally writes to
	// the inherited socket, corrupting the benchmark client's read stream.
	// Passing an explicit devnull *os.File forces the child to use /dev/null.
	var out *os.File
	if serverStderr() != nil {
		out = serverStderr()
	} else {
		out = devnull(tb)
	}
	cmd.Stdout = out
	cmd.Stderr = out
	if err := cmd.Start(); err != nil {
		tb.Fatalf("start %s: %v", binary, err)
	}

	addr := fmt.Sprintf("127.0.0.1:%d", port)
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		c, err := net.DialTimeout("tcp", addr, 50*time.Millisecond)
		if err == nil {
			c.Close()
			return cmd
		}
		time.Sleep(20 * time.Millisecond)
	}
	cmd.Process.Kill() //nolint:errcheck
	tb.Fatalf("%s on port %d not ready within 1 s", binary, port)
	return nil
}

func stopServer(tb testing.TB, cmd *exec.Cmd) {
	tb.Helper()
	if cmd == nil || cmd.Process == nil {
		return
	}
	cmd.Process.Signal(os.Interrupt) //nolint:errcheck

	// Give the server up to 2 s to exit gracefully after SIGINT, then SIGKILL.
	done := make(chan struct{})
	go func() { cmd.Wait(); close(done) }() //nolint:errcheck
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		cmd.Process.Kill() //nolint:errcheck
		<-done
	}
}

// ── raw TCP helpers ───────────────────────────────────────────────────────────

// dial opens a TCP connection and returns it with a buffered reader/writer.
func dial(tb testing.TB, port int) (net.Conn, *bufio.Reader, *bufio.Writer) {
	tb.Helper()
	c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
	if err != nil {
		tb.Fatalf("dial: %v", err)
	}
	return c, bufio.NewReader(c), bufio.NewWriter(c)
}

// sendLine writes "line\n" and flushes.
func sendLine(tb testing.TB, w *bufio.Writer, line string) {
	tb.Helper()
	if _, err := fmt.Fprintf(w, "%s\n", line); err != nil {
		tb.Fatalf("write: %v", err)
	}
	if err := w.Flush(); err != nil {
		tb.Fatalf("flush: %v", err)
	}
}

// readLine reads one response line (strips trailing \r\n) with a 2 s deadline.
func readLine(tb testing.TB, conn net.Conn, r *bufio.Reader) string {
	tb.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	line, err := r.ReadString('\n')
	if err != nil {
		tb.Fatalf("read: %v", err)
	}
	return strings.TrimRight(line, "\r\n")
}

// readLineFromConn wraps an existing conn in a fresh bufio.Reader and reads one line.
func readLineFromConn(tb testing.TB, conn net.Conn) string {
	tb.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	r := bufio.NewReader(conn)
	line, err := r.ReadString('\n')
	if err != nil {
		tb.Fatalf("read: %v", err)
	}
	return strings.TrimRight(line, "\r\n")
}
