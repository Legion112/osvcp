// Tests for select-server (Exercise 2).
//
// Each test starts a fresh server on its own port, runs assertions over
// real TCP connections, then stops the server. No mocking — every test
// hits the actual binary.
//
// Run from this directory:
//
//	go test -v ./...
//	go test -v -run TestPartial

package selectserver_test

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/exec"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"
)

// Path to the server binary relative to the gotest/ directory.
const serverBinary = "../select-server"

// timePattern matches the server's response line.
var timePattern = regexp.MustCompile(`^Current time: \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$`)

// ── server lifecycle ──────────────────────────────────────────────────────────

// startServer launches the binary on the given port and blocks until the port
// accepts connections (up to 1 s). Call stopServer when done.
func startServer(t *testing.T, port int) *exec.Cmd {
	t.Helper()
	cmd := exec.Command(serverBinary, fmt.Sprintf("%d", port))
	cmd.Stdout = os.NewFile(0, os.DevNull)
	cmd.Stderr = os.NewFile(0, os.DevNull)
	if err := cmd.Start(); err != nil {
		t.Fatalf("could not start server: %v", err)
	}

	addr := fmt.Sprintf("127.0.0.1:%d", port)
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 50*time.Millisecond)
		if err == nil {
			conn.Close()
			return cmd
		}
		time.Sleep(20 * time.Millisecond)
	}
	cmd.Process.Kill() //nolint:errcheck
	t.Fatalf("server on port %d did not become ready within 1 s", port)
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

// ── connection helpers ────────────────────────────────────────────────────────

// dial opens a TCP connection and returns it with a buffered reader/writer.
func dial(t *testing.T, port int) (net.Conn, *bufio.Reader, *bufio.Writer) {
	t.Helper()
	conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	return conn, bufio.NewReader(conn), bufio.NewWriter(conn)
}

// sendLine writes "line\n" to the server and flushes.
func sendLine(t *testing.T, w *bufio.Writer, line string) {
	t.Helper()
	if _, err := fmt.Fprintf(w, "%s\n", line); err != nil {
		t.Fatalf("write: %v", err)
	}
	if err := w.Flush(); err != nil {
		t.Fatalf("flush: %v", err)
	}
}

// readLine reads one response line (strips the trailing newline) with a 2 s deadline.
func readLine(t *testing.T, conn net.Conn, r *bufio.Reader) string {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	line, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	return strings.TrimRight(line, "\r\n")
}

func assertTimeResponse(t *testing.T, got string) {
	t.Helper()
	if !timePattern.MatchString(got) {
		t.Errorf("response %q does not match %q", got, timePattern)
	}
}

// Each test uses a distinct port so they can run in parallel without conflict.
const (
	portSingleRequest         = 19001
	portMultipleRequestsSame  = 19002
	portSimultaneousClients   = 19003
	portPartialLineBuffering  = 19004
	portAbruptDisconnect      = 19005
	portRapidFireClients      = 19006
)

// ── tests ─────────────────────────────────────────────────────────────────────

// TestSingleRequest: one connection, one request → one time response.
func TestSingleRequest(t *testing.T) {
	cmd := startServer(t, portSingleRequest)
	defer stopServer(t, cmd)

	conn, r, w := dial(t, portSingleRequest)
	defer conn.Close()

	sendLine(t, w, "hello")
	assertTimeResponse(t, readLine(t, conn, r))
}

// TestMultipleRequestsSameConn: three requests on the same open connection.
// The server must respond to each one without closing between them.
func TestMultipleRequestsSameConn(t *testing.T) {
	cmd := startServer(t, portMultipleRequestsSame)
	defer stopServer(t, cmd)

	conn, r, w := dial(t, portMultipleRequestsSame)
	defer conn.Close()

	for i := 0; i < 3; i++ {
		sendLine(t, w, fmt.Sprintf("request-%d", i))
		assertTimeResponse(t, readLine(t, conn, r))
	}
}

// TestSimultaneousClients: 10 goroutines connect at the same time.
// The single-threaded select() loop must serve all of them.
func TestSimultaneousClients(t *testing.T) {
	const numClients = 10
	cmd := startServer(t, portSimultaneousClients)
	defer stopServer(t, cmd)

	var wg sync.WaitGroup
	errs := make(chan string, numClients)

	for i := 0; i < numClients; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			time.Sleep(time.Duration(id*5) * time.Millisecond) // stagger slightly

			conn, r, w := dial(t, portSimultaneousClients)
			defer conn.Close()

			sendLine(t, w, fmt.Sprintf("client-%d", id))
			resp := readLine(t, conn, r)
			if !timePattern.MatchString(resp) {
				errs <- fmt.Sprintf("client %d got %q", id, resp)
			}
		}(i)
	}

	wg.Wait()
	close(errs)
	for msg := range errs {
		t.Error(msg)
	}
}

// TestPartialLineBuffering: the request is trickled one byte at a time.
// The server must NOT reply until it receives the terminating '\n'.
func TestPartialLineBuffering(t *testing.T) {
	cmd := startServer(t, portPartialLineBuffering)
	defer stopServer(t, cmd)

	conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", portPartialLineBuffering), time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	// Write each byte of "time?" with a gap between them — no newline yet.
	for _, b := range []byte("time?") {
		conn.Write([]byte{b}) //nolint:errcheck
		time.Sleep(30 * time.Millisecond)
	}

	// Confirm the server has NOT replied yet (no complete request received).
	conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond)) //nolint:errcheck
	buf := make([]byte, 64)
	n, _ := conn.Read(buf)
	if n > 0 {
		t.Errorf("server replied before newline: %q", buf[:n])
	}

	// Send the newline — the server must respond right away.
	conn.Write([]byte("\n")) //nolint:errcheck

	r := bufio.NewReader(conn)
	assertTimeResponse(t, readLine(t, conn, r))
}

// TestAbruptDisconnect: a client connects and closes immediately (sends no data).
// The server must survive and continue serving the next client.
func TestAbruptDisconnect(t *testing.T) {
	cmd := startServer(t, portAbruptDisconnect)
	defer stopServer(t, cmd)

	// Rude client: connect and hang up.
	rude, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", portAbruptDisconnect), time.Second)
	if err != nil {
		t.Fatalf("rude dial: %v", err)
	}
	rude.Close()
	time.Sleep(50 * time.Millisecond) // let the server process the EOF

	// Normal client: server must still respond.
	conn, r, w := dial(t, portAbruptDisconnect)
	defer conn.Close()

	sendLine(t, w, "after-disconnect")
	assertTimeResponse(t, readLine(t, conn, r))
}

// TestRapidFireClients: 50 goroutines hammer the server simultaneously.
func TestRapidFireClients(t *testing.T) {
	const numClients = 50
	cmd := startServer(t, portRapidFireClients)
	defer stopServer(t, cmd)

	var wg sync.WaitGroup
	errs := make(chan string, numClients)

	for i := 0; i < numClients; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()

			conn, r, w := dial(t, portRapidFireClients)
			defer conn.Close()

			sendLine(t, w, "ping")
			resp := readLine(t, conn, r)
			if !timePattern.MatchString(resp) {
				errs <- fmt.Sprintf("client %d got %q", id, resp)
			}
		}(i)
	}

	wg.Wait()
	close(errs)
	for msg := range errs {
		t.Error(msg)
	}
}
