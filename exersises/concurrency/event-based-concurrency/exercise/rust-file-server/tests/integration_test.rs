// Integration tests for the Rust file server.
//
// Each test starts a fresh server process on its own port, exercises it over
// real TCP connections, then kills the server.  No mocking — every test
// hits the actual binary.
//
// Run:
//   cargo test
//   cargo test -- --nocapture         (see server stdout)
//   cargo test test_path_escape       (single test)

use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU16, Ordering};
use std::time::{Duration, Instant};

// ── port allocation ───────────────────────────────────────────────────────────
// Each test gets a unique port, so they can run in parallel safely.
static NEXT_PORT: AtomicU16 = AtomicU16::new(19200);

fn next_port() -> u16 {
    NEXT_PORT.fetch_add(1, Ordering::Relaxed)
}

// ── server lifecycle ──────────────────────────────────────────────────────────

struct Server {
    child:   Child,
    port:    u16,
    docroot: PathBuf,
}

impl Server {
    /// Start the server binary and wait until the port is accepting connections.
    fn start(port: u16) -> Self {
        // Resolve docroot relative to the workspace root (where `cargo test` runs).
        let docroot = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .parent()           // exercise/rust-file-server → exercise/
            .unwrap()
            .join("docroot")
            .canonicalize()
            .expect("docroot must exist");

        let bin = PathBuf::from(env!("CARGO_BIN_EXE_file-server-rs"));

        let child = Command::new(&bin)
            .arg(port.to_string())
            .arg(&docroot)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("failed to spawn server");

        // Poll until the port accepts connections (up to 1 s).
        let addr = format!("127.0.0.1:{port}");
        let deadline = Instant::now() + Duration::from_secs(1);
        loop {
            if TcpStream::connect(&addr).is_ok() { break; }
            if Instant::now() > deadline {
                panic!("server on port {port} not ready within 1 s");
            }
            std::thread::sleep(Duration::from_millis(20));
        }

        Server { child, port, docroot }
    }

    fn connect(&self) -> io::Result<Conn> {
        let stream = TcpStream::connect(format!("127.0.0.1:{}", self.port))?;
        stream.set_read_timeout(Some(Duration::from_secs(2)))?;
        Ok(Conn { reader: BufReader::new(stream.try_clone()?), writer: stream })
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

// ── connection helpers ────────────────────────────────────────────────────────

struct Conn {
    reader: BufReader<TcpStream>,
    writer: TcpStream,
}

impl Conn {
    /// Send "<filename>\n" to the server.
    fn request(&mut self, filename: &str) -> io::Result<()> {
        write!(self.writer, "{filename}\n")?;
        self.writer.flush()
    }

    /// Read the status line (+OK <size> or -ERR <reason>).
    fn read_status(&mut self) -> io::Result<String> {
        let mut line = String::new();
        self.reader.read_line(&mut line)?;
        Ok(line.trim_end_matches(['\n', '\r']).to_owned())
    }

    /// Read exactly `n` bytes of body content.
    fn read_body(&mut self, n: u64) -> io::Result<Vec<u8>> {
        let mut buf = vec![0u8; n as usize];
        self.reader.read_exact(&mut buf)?;
        Ok(buf)
    }

    /// High-level: send request, read and parse status, read body if +OK.
    /// Returns (status_line, Option<body_bytes>).
    fn do_request(&mut self, filename: &str) -> io::Result<(String, Option<Vec<u8>>)> {
        self.request(filename)?;
        let status = self.read_status()?;

        if let Some(rest) = status.strip_prefix("+OK ") {
            let size: u64 = rest.parse().expect("size must be a number");
            let body = self.read_body(size)?;
            Ok((status, Some(body)))
        } else {
            Ok((status, None))
        }
    }
}

// ── tests ─────────────────────────────────────────────────────────────────────

/// Verify content of a served file matches what's on disk.
#[test]
fn test_serve_file() {
    let srv = Server::start(next_port());
    let mut conn = srv.connect().unwrap();

    let (status, body) = conn.do_request("hello.txt").unwrap();
    assert!(status.starts_with("+OK"), "expected +OK, got {status:?}");

    let want = std::fs::read(srv.docroot.join("hello.txt")).unwrap();
    assert_eq!(body.unwrap(), want);
}

/// Files inside subdirectories must be reachable.
#[test]
fn test_serve_subdir_file() {
    let srv = Server::start(next_port());
    let mut conn = srv.connect().unwrap();

    let (status, body) = conn.do_request("subdir/nested.txt").unwrap();
    assert!(status.starts_with("+OK"), "expected +OK, got {status:?}");

    let want = std::fs::read(srv.docroot.join("subdir/nested.txt")).unwrap();
    assert_eq!(body.unwrap(), want);
}

/// Two requests on the same connection — tests that the +OK <size> framing is
/// correct: the second request's reader starts at the right byte position.
#[test]
fn test_multiple_requests_same_conn() {
    let srv = Server::start(next_port());
    let mut conn = srv.connect().unwrap();

    for name in ["hello.txt", "pangrams.txt"] {
        let (status, body) = conn.do_request(name).unwrap();
        assert!(status.starts_with("+OK"), "[{name}] expected +OK, got {status:?}");

        let want = std::fs::read(srv.docroot.join(name)).unwrap();
        assert_eq!(body.unwrap(), want, "[{name}] content mismatch");
    }
}

/// Requesting a non-existent file must return -ERR.
#[test]
fn test_missing_file() {
    let srv = Server::start(next_port());
    let mut conn = srv.connect().unwrap();

    let (status, _) = conn.do_request("does-not-exist.txt").unwrap();
    assert!(status.starts_with("-ERR"), "expected -ERR, got {status:?}");
}

/// Absolute paths ("/etc/passwd") must be rejected before touching the fs.
#[test]
fn test_absolute_path_rejected() {
    let srv = Server::start(next_port());
    let mut conn = srv.connect().unwrap();

    let (status, _) = conn.do_request("/etc/passwd").unwrap();
    assert!(status.starts_with("-ERR"), "expected -ERR, got {status:?}");
}

/// "../" traversal attempts must never escape the docroot.
#[test]
fn test_path_escape_rejected() {
    let srv = Server::start(next_port());

    let escape_paths = [
        "../../etc/passwd",
        "../file-server.c",
        "../file-server",
        "subdir/../../file-server.c",
    ];

    for path in escape_paths {
        // Need a fresh connection per request because the server may close on
        // error (reconnect keeps tests independent even if one fails).
        let mut c = srv.connect().unwrap();
        let (status, _) = c.do_request(path).unwrap();
        assert!(
            status.starts_with("-ERR"),
            "escape {path:?} was NOT rejected: got {status:?}"
        );
    }
}

/// A client that connects and immediately closes must not crash the server.
#[test]
fn test_abrupt_disconnect_survives() {
    let srv = Server::start(next_port());

    // Rude client: connect and drop immediately.
    drop(srv.connect().unwrap());
    std::thread::sleep(Duration::from_millis(50));

    // Server must still be alive.
    let (status, _) = srv.connect().unwrap().do_request("hello.txt").unwrap();
    assert!(status.starts_with("+OK"), "server dead after disconnect: {status:?}");
}

/// The request is trickled one byte at a time. The server must buffer the
/// partial data and only reply after the terminating '\n' arrives.
#[test]
fn test_partial_line_buffering() {
    let srv = Server::start(next_port());

    let stream = TcpStream::connect(format!("127.0.0.1:{}", srv.port)).unwrap();
    stream.set_read_timeout(Some(Duration::from_millis(150))).unwrap();
    let mut reader = BufReader::new(stream.try_clone().unwrap());
    let mut writer = stream.try_clone().unwrap();

    // Write "hello.txt" one byte at a time, pausing between each byte.
    for b in b"hello.txt" {
        writer.write_all(&[*b]).unwrap();
        writer.flush().unwrap();
        std::thread::sleep(Duration::from_millis(20));
    }

    // No '\n' yet — server must NOT have replied.
    let mut early = String::new();
    let _ = reader.read_line(&mut early); // expected to time out
    assert!(early.is_empty(), "server replied before newline: {early:?}");

    // Send the newline; now a response must arrive.
    writer.write_all(b"\n").unwrap();
    writer.flush().unwrap();

    stream.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    let mut status = String::new();
    reader.read_line(&mut status).unwrap();
    assert!(
        status.trim_end_matches(['\n', '\r']).starts_with("+OK"),
        "expected +OK after newline, got {status:?}"
    );
}

/// 20 goroutines-worth of threads each request a different file at the same
/// time. Every one must get the correct bytes back.
#[test]
fn test_simultaneous_reads() {
    let port = next_port();
    let srv = Server::start(port);

    let docroot = srv.docroot.clone();
    let files   = ["hello.txt", "pangrams.txt", "subdir/nested.txt"];
    let wants: Vec<Vec<u8>> = files
        .iter()
        .map(|f| std::fs::read(docroot.join(f)).unwrap())
        .collect();

    let handles: Vec<_> = (0..20)
        .map(|i| {
            let name  = files[i % files.len()].to_owned();
            let want  = wants[i % files.len()].clone();
            std::thread::spawn(move || {
                let stream = TcpStream::connect(format!("127.0.0.1:{port}")).unwrap();
                stream.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
                let mut conn = Conn {
                    reader: BufReader::new(stream.try_clone().unwrap()),
                    writer: stream,
                };
                let (status, body) = conn.do_request(&name).unwrap();
                assert!(status.starts_with("+OK"), "client {i} [{name}]: {status:?}");
                assert_eq!(body.unwrap(), want, "client {i} [{name}]: content mismatch");
            })
        })
        .collect();

    for h in handles { h.join().expect("thread panicked"); }
}
