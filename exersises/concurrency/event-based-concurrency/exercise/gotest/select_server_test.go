// Tests for select-server (Exercise 2).
//
// Run:
//
//	go test -v -run TestSelect
package selectserver_test

import (
	"fmt"
	"net"
	"regexp"
	"sync"
	"testing"
	"time"
)

const selectBinary = "../select-server"

var timePattern = regexp.MustCompile(`^Current time: \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$`)

func assertTimeResponse(t *testing.T, got string) {
	t.Helper()
	if !timePattern.MatchString(got) {
		t.Errorf("response %q does not match expected pattern", got)
	}
}

const (
	portSelectSingle       = 19001
	portSelectMultiSame    = 19002
	portSelectSimultaneous = 19003
	portSelectPartial      = 19004
	portSelectDisconnect   = 19005
	portSelectRapidFire    = 19006
)

// TestSelectSingleRequest: one connection, one request → one time response.
func TestSelectSingleRequest(t *testing.T) {
	cmd := startServer(t, selectBinary, portSelectSingle)
	defer stopServer(t, cmd)

	conn, r, w := dial(t, portSelectSingle)
	defer conn.Close()

	sendLine(t, w, "hello")
	assertTimeResponse(t, readLine(t, conn, r))
}

// TestSelectMultipleRequestsSameConn: three requests on the same open connection.
// The server must respond to each without closing between them.
func TestSelectMultipleRequestsSameConn(t *testing.T) {
	cmd := startServer(t, selectBinary, portSelectMultiSame)
	defer stopServer(t, cmd)

	conn, r, w := dial(t, portSelectMultiSame)
	defer conn.Close()

	for i := range 3 {
		sendLine(t, w, fmt.Sprintf("request-%d", i))
		assertTimeResponse(t, readLine(t, conn, r))
	}
}

// TestSelectSimultaneousClients: 10 goroutines connect at the same time.
// The single-threaded select() loop must serve all of them.
func TestSelectSimultaneousClients(t *testing.T) {
	const n = 10
	cmd := startServer(t, selectBinary, portSelectSimultaneous)
	defer stopServer(t, cmd)

	var wg sync.WaitGroup
	errs := make(chan string, n)

	for i := range n {
		wg.Go(func() {
			time.Sleep(time.Duration(i*5) * time.Millisecond)

			conn, r, w := dial(t, portSelectSimultaneous)
			defer conn.Close()

			sendLine(t, w, fmt.Sprintf("client-%d", i))
			resp := readLine(t, conn, r)
			if !timePattern.MatchString(resp) {
				errs <- fmt.Sprintf("client %d got %q", i, resp)
			}
		})
	}

	wg.Wait()
	close(errs)
	for msg := range errs {
		t.Error(msg)
	}
}

// TestSelectPartialLineBuffering: the request is trickled one byte at a time.
// The server must NOT reply until it receives the terminating '\n'.
func TestSelectPartialLineBuffering(t *testing.T) {
	cmd := startServer(t, selectBinary, portSelectPartial)
	defer stopServer(t, cmd)

	conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", portSelectPartial), time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	// Trickle "time?" one byte at a time — no newline yet.
	for _, b := range []byte("time?") {
		conn.Write([]byte{b}) //nolint:errcheck
		time.Sleep(30 * time.Millisecond)
	}

	// Server must NOT have replied yet.
	conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond)) //nolint:errcheck
	buf := make([]byte, 64)
	n, _ := conn.Read(buf)
	if n > 0 {
		t.Errorf("server replied before newline: %q", buf[:n])
	}

	// Send the newline — now the server must reply.
	conn.Write([]byte("\n")) //nolint:errcheck
	assertTimeResponse(t, readLineFromConn(t, conn))
}

// TestSelectAbruptDisconnect: a client disconnects without sending data.
// The server must survive and serve the next client normally.
func TestSelectAbruptDisconnect(t *testing.T) {
	cmd := startServer(t, selectBinary, portSelectDisconnect)
	defer stopServer(t, cmd)

	rude, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", portSelectDisconnect), time.Second)
	if err != nil {
		t.Fatalf("rude dial: %v", err)
	}
	rude.Close()
	time.Sleep(50 * time.Millisecond)

	conn, r, w := dial(t, portSelectDisconnect)
	defer conn.Close()
	sendLine(t, w, "after-disconnect")
	assertTimeResponse(t, readLine(t, conn, r))
}

// TestSelectRapidFireClients: 50 goroutines hammer the server simultaneously.
func TestSelectRapidFireClients(t *testing.T) {
	const n = 50
	cmd := startServer(t, selectBinary, portSelectRapidFire)
	defer stopServer(t, cmd)

	var wg sync.WaitGroup
	errs := make(chan string, n)

	for i := range n {
		wg.Go(func() {
			conn, r, w := dial(t, portSelectRapidFire)
			defer conn.Close()

			sendLine(t, w, "ping")
			resp := readLine(t, conn, r)
			if !timePattern.MatchString(resp) {
				errs <- fmt.Sprintf("client %d got %q", i, resp)
			}
		})
	}

	wg.Wait()
	close(errs)
	for msg := range errs {
		t.Error(msg)
	}
}
