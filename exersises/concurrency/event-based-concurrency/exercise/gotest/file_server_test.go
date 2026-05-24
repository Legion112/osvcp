// Tests for file-server (Exercise 3).
//
// Protocol under test:
//
//	Client → Server:  <relative-filename>\n
//	Server → Client:  +OK <size>\n<content>   on success
//	                  -ERR <reason>\n          on failure
//
// Run:
//
//	go test -v -run TestFile
package selectserver_test

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	fileBinary = "../file-server"
	docroot    = "../docroot"
)

const (
	portFileServe        = 19101
	portFileSubdir       = 19102
	portFileMultiReq     = 19103
	portFileMissing      = 19104
	portFileAbsPath      = 19105
	portFilePathEscape   = 19106
	portFileSimultaneous = 19107
)

// ── protocol helpers ──────────────────────────────────────────────────────────

// fileConn wraps a connection for the file-server protocol.
type fileConn struct {
	conn net.Conn
	r    *bufio.Reader
}

func newFileConn(t *testing.T, port int) *fileConn {
	t.Helper()
	c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	return &fileConn{conn: c, r: bufio.NewReader(c)}
}

func (fc *fileConn) Close() { fc.conn.Close() }

// Do sends a file request and returns (statusLine, body).
// Body is nil for -ERR responses.
func (fc *fileConn) Do(t *testing.T, filename string) (status string, body []byte) {
	t.Helper()

	fc.conn.SetWriteDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	if _, err := fmt.Fprintf(fc.conn, "%s\n", filename); err != nil {
		t.Fatalf("write request: %v", err)
	}

	fc.conn.SetReadDeadline(time.Now().Add(2 * time.Second)) //nolint:errcheck
	line, err := fc.r.ReadString('\n')
	if err != nil {
		t.Fatalf("read status: %v", err)
	}
	status = strings.TrimRight(line, "\r\n")

	if !strings.HasPrefix(status, "+OK ") {
		return status, nil
	}

	size, err := strconv.ParseInt(strings.TrimPrefix(status, "+OK "), 10, 64)
	if err != nil {
		t.Fatalf("bad size in %q: %v", status, err)
	}

	fc.conn.SetReadDeadline(time.Now().Add(5 * time.Second)) //nolint:errcheck
	body = make([]byte, size)
	if _, err := io.ReadFull(fc.r, body); err != nil {
		t.Fatalf("read body: %v", err)
	}
	return status, body
}

// startFileServer resolves docroot to an absolute path and starts the server.
func startFileServer(t *testing.T, port int) *exec.Cmd {
	t.Helper()
	abs, err := filepath.Abs(docroot)
	if err != nil {
		t.Fatalf("abs docroot: %v", err)
	}
	return startServer(t, fileBinary, port, abs)
}

// fixture reads a file from docroot and returns its contents.
func fixture(t *testing.T, rel string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(docroot, rel))
	if err != nil {
		t.Fatalf("read fixture %s: %v", rel, err)
	}
	return b
}

// ── tests ─────────────────────────────────────────────────────────────────────

// TestFileServeFile: request a file and verify the content matches the disk.
func TestFileServeFile(t *testing.T) {
	cmd := startFileServer(t, portFileServe)
	defer stopServer(t, cmd)

	c := newFileConn(t, portFileServe)
	defer c.Close()

	status, got := c.Do(t, "hello.txt")
	if !strings.HasPrefix(status, "+OK") {
		t.Fatalf("expected +OK, got %q", status)
	}
	if string(got) != string(fixture(t, "hello.txt")) {
		t.Error("content mismatch")
	}
}

// TestFileServeSubdirFile: files inside subdirectories must be reachable.
func TestFileServeSubdirFile(t *testing.T) {
	cmd := startFileServer(t, portFileSubdir)
	defer stopServer(t, cmd)

	c := newFileConn(t, portFileSubdir)
	defer c.Close()

	status, got := c.Do(t, "subdir/nested.txt")
	if !strings.HasPrefix(status, "+OK") {
		t.Fatalf("expected +OK, got %q", status)
	}
	if string(got) != string(fixture(t, "subdir/nested.txt")) {
		t.Error("content mismatch")
	}
}

// TestFileMultipleRequestsSameConn: two file requests on one connection.
// Tests that +OK framing is correct — second read starts after the first body.
func TestFileMultipleRequestsSameConn(t *testing.T) {
	cmd := startFileServer(t, portFileMultiReq)
	defer stopServer(t, cmd)

	c := newFileConn(t, portFileMultiReq)
	defer c.Close()

	for _, name := range []string{"hello.txt", "pangrams.txt"} {
		status, got := c.Do(t, name)
		if !strings.HasPrefix(status, "+OK") {
			t.Fatalf("[%s] expected +OK, got %q", name, status)
		}
		if string(got) != string(fixture(t, name)) {
			t.Errorf("[%s] content mismatch", name)
		}
	}
}

// TestFileMissingFile: a non-existent file must return -ERR.
func TestFileMissingFile(t *testing.T) {
	cmd := startFileServer(t, portFileMissing)
	defer stopServer(t, cmd)

	c := newFileConn(t, portFileMissing)
	defer c.Close()

	status, _ := c.Do(t, "does-not-exist.txt")
	if !strings.HasPrefix(status, "-ERR") {
		t.Errorf("expected -ERR, got %q", status)
	}
}

// TestFileAbsolutePathRejected: requests starting with '/' must be rejected.
func TestFileAbsolutePathRejected(t *testing.T) {
	cmd := startFileServer(t, portFileAbsPath)
	defer stopServer(t, cmd)

	c := newFileConn(t, portFileAbsPath)
	defer c.Close()

	status, _ := c.Do(t, "/etc/passwd")
	if !strings.HasPrefix(status, "-ERR") {
		t.Errorf("expected -ERR for absolute path, got %q", status)
	}
}

// TestFilePathEscapeRejected: "../" traversal must not escape the docroot.
func TestFilePathEscapeRejected(t *testing.T) {
	cmd := startFileServer(t, portFilePathEscape)
	defer stopServer(t, cmd)

	c := newFileConn(t, portFilePathEscape)
	defer c.Close()

	escapes := []string{
		"../../etc/passwd",
		"../file-server.c",
		"../file-server",
		"subdir/../../file-server.c",
	}
	for _, path := range escapes {
		status, _ := c.Do(t, path)
		if !strings.HasPrefix(status, "-ERR") {
			t.Errorf("escape %q was NOT rejected, got %q", path, status)
		}
	}
}

// TestFileSimultaneousReads: 20 concurrent clients fetch different files.
// The event loop must interleave correctly and return exact content to each client.
func TestFileSimultaneousReads(t *testing.T) {
	const numClients = 20
	cmd := startFileServer(t, portFileSimultaneous)
	defer stopServer(t, cmd)

	files := []string{"hello.txt", "pangrams.txt", "subdir/nested.txt"}
	wants := make(map[string][]byte, len(files))
	for _, f := range files {
		wants[f] = fixture(t, f)
	}

	var wg sync.WaitGroup
	errs := make(chan string, numClients)

	for i := range numClients {
		wg.Go(func() {
			name := files[i%len(files)]

			c := newFileConn(t, portFileSimultaneous)
			defer c.Close()

			status, got := c.Do(t, name)
			switch {
			case !strings.HasPrefix(status, "+OK"):
				errs <- fmt.Sprintf("client %d [%s]: %q", i, name, status)
			case string(got) != string(wants[name]):
				errs <- fmt.Sprintf("client %d [%s]: content mismatch", i, name)
			}
		})
	}

	wg.Wait()
	close(errs)
	for msg := range errs {
		t.Error(msg)
	}
}
