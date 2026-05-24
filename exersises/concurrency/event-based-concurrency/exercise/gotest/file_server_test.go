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
	"syscall"
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
	fmt.Fprint(os.Stderr, string(body))
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
			t.Cleanup(func() {
				stopServer(t, cmd)
			})
			f(t, port)
		})
	}
}

// eachFileServerWithCmd is like eachFileServer but also passes the server
// *exec.Cmd so tests can send signals (e.g. SIGUSR1 to flush the cache).
func eachFileServerWithCmd(t *testing.T, f func(t *testing.T, port int, cmd *exec.Cmd)) {
	t.Helper()
	for _, bin := range fileServerBinaries() {
		bin := bin
		t.Run(serverLabel(bin), func(t *testing.T) {
			t.Parallel()
			port := nextPort()
			cmd := startFileServer(t, bin, port)
			t.Cleanup(func() {
				stopServer(t, cmd)
			})
			f(t, port, cmd)
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

// TestFileCacheInvalidation verifies that:
//  1. A file served once is cached (second fetch returns stale content after
//     the file is overwritten on disk).
//  2. Sending SIGUSR1 flushes the cache so the next fetch reads fresh content.
func TestFileCacheInvalidation(t *testing.T) {
	eachFileServerWithCmd(t, func(t *testing.T, port int, cmd *exec.Cmd) {
		// Create a uniquely named temp file inside docroot.
		tmpName := fmt.Sprintf("cache-test-%d.txt", port)
		tmpPath := filepath.Join(docroot, tmpName)
		const v1 = "version-one\n"
		const v2 = "version-two\n"

		if err := os.WriteFile(tmpPath, []byte(v1), 0o644); err != nil {
			t.Fatalf("create temp file: %v", err)
		}
		t.Cleanup(func() { os.Remove(tmpPath) }) //nolint:errcheck

		fc := newFileConn(t, port)
		defer fc.Close()

		// 1. First fetch — populates the cache.
		status, got := fc.Do(t, tmpName)
		if !strings.HasPrefix(status, "+OK") {
			t.Fatalf("first fetch: %q", status)
		}
		if string(got) != v1 {
			t.Fatalf("first fetch: got %q, want %q", got, v1)
		}

		// 2. Overwrite the file on disk.
		if err := os.WriteFile(tmpPath, []byte(v2), 0o644); err != nil {
			t.Fatalf("overwrite file: %v", err)
		}

		// 3. Second fetch — must return stale (v1) content from cache.
		status, got = fc.Do(t, tmpName)
		if !strings.HasPrefix(status, "+OK") {
			t.Fatalf("second fetch: %q", status)
		}
		if string(got) != v1 {
			// The server does not cache (served updated content immediately).
			// This is not necessarily wrong for a non-caching implementation,
			// but all our servers implement the cache, so treat it as a failure.
			t.Errorf("second fetch: got %q, want cached %q (cache not working?)", got, v1)
			return
		}

		// 4. Send SIGUSR1 to flush the server's cache.
		if err := cmd.Process.Signal(syscall.SIGUSR1); err != nil {
			t.Fatalf("send SIGUSR1: %v", err)
		}
		// Give the server a moment to process the signal in its event loop.
		time.Sleep(100 * time.Millisecond)

		// 5. Third fetch — cache is empty; server must re-read from disk.
		status, got = fc.Do(t, tmpName)
		if !strings.HasPrefix(status, "+OK") {
			t.Fatalf("third fetch: %q", status)
		}
		if string(got) != v2 {
			t.Errorf("third fetch (after SIGUSR1): got %q, want %q (cache not cleared?)", got, v2)
		}
	})
}
