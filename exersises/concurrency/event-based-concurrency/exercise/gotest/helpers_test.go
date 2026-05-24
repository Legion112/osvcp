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
//   TEST_FILE_SERVERS=./file-server               go test ./...   # C only
//   TEST_FILE_SERVERS=./file-server,./rust-bin    go test ./...   # both
//
// Similarly, the time-server (select-server) list can be overridden with
// TEST_TIME_SERVERS (defaults to the single C implementation).

package selectserver_test

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

// ── server lifecycle ──────────────────────────────────────────────────────────

// startServer launches binary on port (with optional extra args), waits up to
// 1 s for the port to be ready, and returns the running *exec.Cmd.
func startServer(t *testing.T, binary string, port int, extraArgs ...string) *exec.Cmd {
	t.Helper()
	args := append([]string{fmt.Sprintf("%d", port)}, extraArgs...)
	cmd := exec.Command(binary, args...)
	cmd.Stdout = os.NewFile(0, os.DevNull)
	cmd.Stderr = os.NewFile(0, os.DevNull)
	if err := cmd.Start(); err != nil {
		t.Fatalf("start %s: %v", binary, err)
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
	t.Fatalf("%s on port %d not ready within 1 s", binary, port)
	return nil
}

func stopServer(t *testing.T, cmd *exec.Cmd) {
	t.Helper()
	if cmd == nil || cmd.Process == nil {
		return
	}
	cmd.Process.Signal(os.Interrupt) //nolint:errcheck
	cmd.Wait()                       //nolint:errcheck
}

// ── raw TCP helpers ───────────────────────────────────────────────────────────

// dial opens a TCP connection and returns it with a buffered reader/writer.
func dial(t *testing.T, port int) (net.Conn, *bufio.Reader, *bufio.Writer) {
	t.Helper()
	c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	return c, bufio.NewReader(c), bufio.NewWriter(c)
}

// sendLine writes "line\n" and flushes.
func sendLine(t *testing.T, w *bufio.Writer, line string) {
	t.Helper()
	if _, err := fmt.Fprintf(w, "%s\n", line); err != nil {
		t.Fatalf("write: %v", err)
	}
	if err := w.Flush(); err != nil {
		t.Fatalf("flush: %v", err)
	}
}

// readLine reads one response line (strips trailing \r\n) with a 2 s deadline.
func readLine(t *testing.T, conn net.Conn, r *bufio.Reader) string {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	line, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	return strings.TrimRight(line, "\r\n")
}

// readLineFromConn wraps an existing conn in a fresh bufio.Reader and reads one line.
func readLineFromConn(t *testing.T, conn net.Conn) string {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	r := bufio.NewReader(conn)
	line, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	return strings.TrimRight(line, "\r\n")
}
