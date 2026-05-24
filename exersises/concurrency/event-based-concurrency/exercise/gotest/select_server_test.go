// Tests for the time-server (Exercise 2: select()-based server).
//
// Run all:          go test -v -run TestTime
// Run one test:     go test -v -run TestTimePartial
// C server only:    TEST_TIME_SERVERS=../select-server go test -v -run TestTime
package selectserver

import (
	"fmt"
	"net"
	"regexp"
	"sync"
	"testing"
	"time"
)

var timePattern = regexp.MustCompile(`^Current time: \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$`)

func assertTimeResponse(t *testing.T, got string) {
	t.Helper()
	if !timePattern.MatchString(got) {
		t.Errorf("response %q does not match expected time pattern", got)
	}
}

// eachTimeServer runs f as a parallel sub-test for every configured binary.
func eachTimeServer(t *testing.T, f func(t *testing.T, port int)) {
	t.Helper()
	for _, bin := range timeServerBinaries() {
		bin := bin
		t.Run(serverLabel(bin), func(t *testing.T) {
			t.Parallel()
			port := nextPort()
			cmd := startServer(t, bin, port)
			defer stopServer(t, cmd)
			f(t, port)
		})
	}
}

// ── tests ─────────────────────────────────────────────────────────────────────

// TestTimeSingleRequest: one connection, one request → one time response.
func TestTimeSingleRequest(t *testing.T) {
	eachTimeServer(t, func(t *testing.T, port int) {
		conn, r, w := dial(t, port)
		defer conn.Close()
		sendLine(t, w, "hello")
		assertTimeResponse(t, readLine(t, conn, r))
	})
}

// TestTimeMultipleRequestsSameConn: three requests on the same open connection.
func TestTimeMultipleRequestsSameConn(t *testing.T) {
	eachTimeServer(t, func(t *testing.T, port int) {
		conn, r, w := dial(t, port)
		defer conn.Close()
		for i := range 3 {
			sendLine(t, w, fmt.Sprintf("request-%d", i))
			assertTimeResponse(t, readLine(t, conn, r))
		}
	})
}

// TestTimeSimultaneousClients: 10 goroutines connect at the same time.
func TestTimeSimultaneousClients(t *testing.T) {
	eachTimeServer(t, func(t *testing.T, port int) {
		const n = 10
		var wg sync.WaitGroup
		errs := make(chan string, n)
		for i := range n {
			wg.Go(func() {
				time.Sleep(time.Duration(i*5) * time.Millisecond)
				conn, r, w := dial(t, port)
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
	})
}

// TestTimePartialLineBuffering: server must buffer partial data until '\n'.
func TestTimePartialLineBuffering(t *testing.T) {
	eachTimeServer(t, func(t *testing.T, port int) {
		conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
		if err != nil {
			t.Fatalf("dial: %v", err)
		}
		defer conn.Close()

		for _, b := range []byte("time?") {
			conn.Write([]byte{b}) //nolint:errcheck
			time.Sleep(30 * time.Millisecond)
		}

		conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond)) //nolint:errcheck
		buf := make([]byte, 64)
		n, _ := conn.Read(buf)
		if n > 0 {
			t.Errorf("server replied before newline: %q", buf[:n])
		}

		conn.Write([]byte("\n")) //nolint:errcheck
		assertTimeResponse(t, readLineFromConn(t, conn))
	})
}

// TestTimeAbruptDisconnect: abrupt client close must not crash the server.
func TestTimeAbruptDisconnect(t *testing.T) {
	eachTimeServer(t, func(t *testing.T, port int) {
		rude, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
		if err != nil {
			t.Fatalf("rude dial: %v", err)
		}
		rude.Close()
		time.Sleep(50 * time.Millisecond)

		conn, r, w := dial(t, port)
		defer conn.Close()
		sendLine(t, w, "after-disconnect")
		assertTimeResponse(t, readLine(t, conn, r))
	})
}

// TestTimeRapidFireClients: 50 goroutines hammer the server simultaneously.
func TestTimeRapidFireClients(t *testing.T) {
	eachTimeServer(t, func(t *testing.T, port int) {
		const n = 50
		var wg sync.WaitGroup
		errs := make(chan string, n)
		for i := range n {
			wg.Go(func() {
				conn, r, w := dial(t, port)
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
	})
}
