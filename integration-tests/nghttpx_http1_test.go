package nghttp2

import (
	"bufio"
	"fmt"
	"github.com/bradfitz/http2/hpack"
	"io"
	"net/http"
	"syscall"
	"testing"
)

// TestH1H1PlainGET tests whether simple HTTP/1 GET request works.
func TestH1H1PlainGET(t *testing.T) {
	st := newServerTester(nil, t, noopHandler)
	defer st.Close()

	res, err := st.http1(requestParam{
		name: "TestH1H1PlainGET",
	})
	if err != nil {
		t.Fatalf("Error st.http1() = %v", err)
	}

	want := 200
	if got := res.status; got != want {
		t.Errorf("status = %v; want %v", got, want)
	}
}

// TestH1H1PlainGETClose tests whether simple HTTP/1 GET request with
// Connetion: close request header field works.
func TestH1H1PlainGETClose(t *testing.T) {
	st := newServerTester(nil, t, noopHandler)
	defer st.Close()

	res, err := st.http1(requestParam{
		name: "TestH1H1PlainGETClose",
		header: []hpack.HeaderField{
			pair("Connection", "close"),
		},
	})
	if err != nil {
		t.Fatalf("Error st.http1() = %v", err)
	}

	want := 200
	if got := res.status; got != want {
		t.Errorf("status = %v; want %v", got, want)
	}
}

// TestH1H1MultipleRequestCL tests that server rejects request which
// contains multiple Content-Length header fields.
func TestH1H1MultipleRequestCL(t *testing.T) {
	st := newServerTester(nil, t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("server should not forward bad request")
	})
	defer st.Close()

	if _, err := io.WriteString(st.conn, fmt.Sprintf(`GET / HTTP/1.1
Host: %v
Test-Case: TestH1H1MultipleRequestCL
Content-Length: 0
Content-Length: 0

`, st.authority)); err != nil {
		t.Fatalf("Error io.WriteString() = %v", err)
	}

	resp, err := http.ReadResponse(bufio.NewReader(st.conn), nil)
	if err != nil {
		t.Fatalf("Error http.ReadResponse() = %v", err)
	}

	want := 400
	if got := resp.StatusCode; got != want {
		t.Errorf("status: %v; want %v", got, want)
	}
}

// TestH1H1ConnectFailure tests that server handles the situation that
// connection attempt to HTTP/1 backend failed.
func TestH1H1ConnectFailure(t *testing.T) {
	st := newServerTester(nil, t, noopHandler)
	defer st.Close()

	// shutdown backend server to simulate backend connect failure
	st.ts.Close()

	res, err := st.http1(requestParam{
		name: "TestH1H1ConnectFailure",
	})
	if err != nil {
		t.Fatalf("Error st.http1() = %v", err)
	}
	want := 503
	if got := res.status; got != want {
		t.Errorf("status: %v; want %v", got, want)
	}
}

// TestH1H1GracefulShutdown tests graceful shutdown.
func TestH1H1GracefulShutdown(t *testing.T) {
	st := newServerTester(nil, t, noopHandler)
	defer st.Close()

	res, err := st.http1(requestParam{
		name: "TestH1H1GracefulShutdown-1",
	})
	if err != nil {
		t.Fatalf("Error st.http1() = %v", err)
	}

	if got, want := res.status, 200; got != want {
		t.Errorf("status: %v; want %v", got, want)
	}

	st.cmd.Process.Signal(syscall.SIGQUIT)

	res, err = st.http1(requestParam{
		name: "TestH1H1GracefulShutdown-2",
	})
	if err != nil {
		t.Fatalf("Error st.http1() = %v", err)
	}

	if got, want := res.status, 200; got != want {
		t.Errorf("status: %v; want %v", got, want)
	}

	if got, want := res.connClose, true; got != want {
		t.Errorf("res.connClose: %v; want %v", got, want)
	}

	want := io.EOF
	if _, err := st.conn.Read(nil); err == nil || err != want {
		t.Errorf("st.conn.Read(): %v; want %v", err, want)
	}
}

// TestH1H2ConnectFailure tests that server handles the situation that
// connection attempt to HTTP/2 backend failed.
func TestH1H2ConnectFailure(t *testing.T) {
	st := newServerTester([]string{"--http2-bridge"}, t, noopHandler)
	defer st.Close()

	// simulate backend connect attempt failure
	st.ts.Close()

	res, err := st.http1(requestParam{
		name: "TestH1H2ConnectFailure",
	})
	if err != nil {
		t.Fatalf("Error st.http1() = %v", err)
	}
	want := 503
	if got := res.status; got != want {
		t.Errorf("status: %v; want %v", got, want)
	}
}

// TestH1H2NoHost tests that server rejects request without Host
// header field for HTTP/2 backend.
func TestH1H2NoHost(t *testing.T) {
	st := newServerTester([]string{"--http2-bridge"}, t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("server should not forward bad request")
	})
	defer st.Close()

	// without Host header field, we expect 400 response
	if _, err := io.WriteString(st.conn, "GET / HTTP/1.1\r\nTest-Case: TestH1H2NoHost\r\n\r\n"); err != nil {
		t.Fatalf("Error io.WriteString() = %v", err)
	}

	resp, err := http.ReadResponse(bufio.NewReader(st.conn), nil)
	if err != nil {
		t.Fatalf("Error http.ReadResponse() = %v", err)
	}

	want := 400
	if got := resp.StatusCode; got != want {
		t.Errorf("status: %v; want %v", got, want)
	}
}

// TestH1H2CrumbleCookie tests that Cookies are crumbled and assembled
// when forwarding to HTTP/2 backend link.  go-nghttp2 server
// concatenates crumbled Cookies automatically, so this test is not
// much effective now.
func TestH1H2CrumbleCookie(t *testing.T) {
	st := newServerTester([]string{"--http2-bridge"}, t, func(w http.ResponseWriter, r *http.Request) {
		if got, want := r.Header.Get("Cookie"), "alpha; bravo; charlie"; got != want {
			t.Errorf("Cookie: %v; want %v", got, want)
		}
	})
	defer st.Close()

	res, err := st.http1(requestParam{
		name: "TestH1H2CrumbleCookie",
		header: []hpack.HeaderField{
			pair("Cookie", "alpha; bravo; charlie"),
		},
	})
	if err != nil {
		t.Fatalf("Error st.http1() = %v", err)
	}
	if got, want := res.status, 200; got != want {
		t.Errorf("status: %v; want %v", got, want)
	}
}
