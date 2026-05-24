// Shared test helpers for both select-server and file-server tests.
package selectserver_test

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

// startServer launches the given binary on port, waits until the port accepts
// connections (up to 1 s), and returns the running *exec.Cmd.
// Pass extra arguments (e.g. docroot) via extraArgs.
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

// readLine reads one response line (strips trailing \r\n) using a 2 s deadline.
func readLine(t *testing.T, conn net.Conn, r *bufio.Reader) string {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	line, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	return strings.TrimRight(line, "\r\n")
}

// readLineFromConn wraps conn in a fresh bufio.Reader and reads one line.
// Use this when you already hold the conn but haven't created a bufio.Reader yet.
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
