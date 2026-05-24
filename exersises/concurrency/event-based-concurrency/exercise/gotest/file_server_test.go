// Tests for the file-server (Exercise 3).
//
// Protocol:
//
//	Client → Server:  <relative-filename>\n
//	Server → Client:  +OK <size>\n<content>   on success
//	                  -ERR <reason>\n          on failure
//
// Run all:             go test -v -run TestFile
// C server only:       TEST_FILE_SERVERS=../file-server go test -v -run TestFile
// Rust server only:    TEST_FILE_SERVERS=../rust-file-server/target/debug/file-server-rs go test -v -run TestFile
// Both (default):      go test -v -run TestFile
package selectserver

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

const docroot = "../docroot"

// ── protocol helpers ──────────────────────────────────────────────────────────

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

// startFileServer resolves docroot and starts the given binary.
func startFileServer(t *testing.T, binary string, port int) *exec.Cmd {
	t.Helper()
	abs, err := filepath.Abs(docroot)
	if err != nil {
		t.Fatalf("abs docroot: %v", err)
	}
	return startServer(t, binary, port, abs)
}

// fixture reads a file from docroot for comparison in tests.
func fixture(t *testing.T, rel string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(docroot, rel))
	if err != nil {
		t.Fatalf("read fixture %s: %v", rel, err)
	}
	return b
}

// eachFileServer runs f as a parallel sub-test for every configured binary.
// Sub-test names are the base filename of the binary, so output looks like:
//
//	--- PASS: TestFileServeFile/file-server (0.02s)
//	--- PASS: TestFileServeFile/file-server-rs (0.02s)
func eachFileServer(t *testing.T, f func(t *testing.T, port int)) {
	t.Helper()
	for _, bin := range fileServerBinaries() {
		bin := bin
		t.Run(serverLabel(bin), func(t *testing.T) {
			t.Parallel()
			port := nextPort()
			cmd := startFileServer(t, bin, port)
			defer stopServer(t, cmd)
			f(t, port)
		})
	}
}

// ── tests ─────────────────────────────────────────────────────────────────────

// TestFileServeFile: request a file; content must match the file on disk.
func TestFileServeFile(t *testing.T) {
	eachFileServer(t, func(t *testing.T, port int) {
		c := newFileConn(t, port)
		defer c.Close()
		status, got := c.Do(t, "hello.txt")
		if !strings.HasPrefix(status, "+OK") {
			t.Fatalf("expected +OK, got %q", status)
		}
		if string(got) != string(fixture(t, "hello.txt")) {
			t.Error("content mismatch")
		}
	})
}

// TestFileServeSubdirFile: files inside subdirectories must be reachable.
func TestFileServeSubdirFile(t *testing.T) {
	eachFileServer(t, func(t *testing.T, port int) {
		c := newFileConn(t, port)
		defer c.Close()
		status, got := c.Do(t, "subdir/nested.txt")
		if !strings.HasPrefix(status, "+OK") {
			t.Fatalf("expected +OK, got %q", status)
		}
		if string(got) != string(fixture(t, "subdir/nested.txt")) {
			t.Error("content mismatch")
		}
	})
}

// TestFileMultipleRequestsSameConn: two requests on one connection.
// Verifies that +OK <size> framing is correct across consecutive requests.
func TestFileMultipleRequestsSameConn(t *testing.T) {
	eachFileServer(t, func(t *testing.T, port int) {
		c := newFileConn(t, port)
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
	})
}

// TestFileMissingFile: a non-existent file must return -ERR.
func TestFileMissingFile(t *testing.T) {
	eachFileServer(t, func(t *testing.T, port int) {
		c := newFileConn(t, port)
		defer c.Close()
		status, _ := c.Do(t, "does-not-exist.txt")
		if !strings.HasPrefix(status, "-ERR") {
			t.Errorf("expected -ERR, got %q", status)
		}
	})
}

// TestFileAbsolutePathRejected: requests starting with '/' must be rejected.
func TestFileAbsolutePathRejected(t *testing.T) {
	eachFileServer(t, func(t *testing.T, port int) {
		c := newFileConn(t, port)
		defer c.Close()
		status, _ := c.Do(t, "/etc/passwd")
		if !strings.HasPrefix(status, "-ERR") {
			t.Errorf("expected -ERR for absolute path, got %q", status)
		}
	})
}

// TestFilePathEscapeRejected: "../" traversal must not escape the docroot.
func TestFilePathEscapeRejected(t *testing.T) {
	eachFileServer(t, func(t *testing.T, port int) {
		for _, path := range []string{
			"../../etc/passwd",
			"../file-server.c",
			"../file-server",
			"subdir/../../file-server.c",
		} {
			c := newFileConn(t, port)
			status, _ := c.Do(t, path)
			c.Close()
			if !strings.HasPrefix(status, "-ERR") {
				t.Errorf("escape %q not rejected: got %q", path, status)
			}
		}
	})
}

// TestFileSimultaneousReads: 20 concurrent clients fetch different files.
func TestFileSimultaneousReads(t *testing.T) {
	eachFileServer(t, func(t *testing.T, port int) {
		files := []string{"hello.txt", "pangrams.txt", "subdir/nested.txt"}
		wants := make(map[string][]byte, len(files))
		for _, f := range files {
			wants[f] = fixture(t, f)
		}

		const n = 20
		var wg sync.WaitGroup
		errs := make(chan string, n)

		for i := range n {
			wg.Go(func() {
				name := files[i%len(files)]
				c := newFileConn(t, port)
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
	})
}
