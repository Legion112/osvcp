// Integration tests for the file server (Exercise 3).
//
// The same test suite runs against every configured implementation.
// By default both the C and Rust binaries are tested.
// Override with the TEST_FILE_SERVERS env var (colon-separated paths):
//
//   TEST_FILE_SERVERS=../file-server                   cargo test   # C only
//   TEST_FILE_SERVERS=target/debug/file-server-rs      cargo test   # Rust only
//   TEST_FILE_SERVERS=../file-server:target/debug/...  cargo test   # both
//
// Run a single test:
//   cargo test test_path_escape

use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU16, Ordering};
use std::time::{Duration, Instant};

// ── port allocation ───────────────────────────────────────────────────────────

static NEXT_PORT: AtomicU16 = AtomicU16::new(19300);

fn next_port() -> u16 {
    NEXT_PORT.fetch_add(1, Ordering::Relaxed)
}

// ── binary list ───────────────────────────────────────────────────────────────

/// Returns the list of server binaries to test.
/// Reads TEST_FILE_SERVERS (colon-separated) or falls back to both C and Rust.
fn server_binaries() -> Vec<PathBuf> {
    if let Ok(var) = std::env::var("TEST_FILE_SERVERS") {
        return var.split(':').map(PathBuf::from).collect();
    }

    let rust_bin = PathBuf::from(env!("CARGO_BIN_EXE_file-server-rs"));

    // The C binary lives one directory above the Cargo workspace root.
    let c_bin = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("file-server");

    vec![c_bin, rust_bin]
}

// ── server lifecycle ──────────────────────────────────────────────────────────

struct Server {
    child:   Child,
    port:    u16,
    docroot: PathBuf,
    label:   String,
}

impl Server {
    fn start(binary: &PathBuf, port: u16) -> Self {
        let docroot = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .join("docroot")
            .canonicalize()
            .expect("docroot must exist");

        let label = binary
            .file_name()
            .unwrap_or(binary.as_os_str())
            .to_string_lossy()
            .into_owned();

        let child = Command::new(binary)
            .arg(port.to_string())
            .arg(&docroot)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap_or_else(|e| panic!("spawn {label}: {e}"));

        let addr = format!("127.0.0.1:{port}");
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            if TcpStream::connect(&addr).is_ok() { break; }
            if Instant::now() > deadline {
                panic!("{label} on port {port} not ready within 2 s");
            }
            std::thread::sleep(Duration::from_millis(20));
        }

        Server { child, port, docroot, label }
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
    fn request(&mut self, filename: &str) -> io::Result<()> {
        write!(self.writer, "{filename}\n")?;
        self.writer.flush()
    }

    fn read_status(&mut self) -> io::Result<String> {
        let mut line = String::new();
        self.reader.read_line(&mut line)?;
        Ok(line.trim_end_matches(['\n', '\r']).to_owned())
    }

    fn read_body(&mut self, n: u64) -> io::Result<Vec<u8>> {
        let mut buf = vec![0u8; n as usize];
        self.reader.read_exact(&mut buf)?;
        Ok(buf)
    }

    /// Send request, parse status, read body if +OK.
    fn do_request(&mut self, filename: &str) -> io::Result<(String, Option<Vec<u8>>)> {
        self.request(filename)?;
        let status = self.read_status()?;
        if let Some(rest) = status.strip_prefix("+OK ") {
            let size: u64 = rest.parse().expect("size must be a number");
            Ok((status, Some(self.read_body(size)?)))
        } else {
            Ok((status, None))
        }
    }
}

// ── test runner ───────────────────────────────────────────────────────────────

/// Run `f` once for every configured server binary.
/// Panics with a clear label if any invocation fails.
fn for_each_server(f: impl Fn(&Server)) {
    for bin in server_binaries() {
        let srv = Server::start(&bin, next_port());
        println!("  → testing {}", srv.label);
        f(&srv);
    }
}

// ── tests ─────────────────────────────────────────────────────────────────────

#[test]
fn test_serve_file() {
    for_each_server(|srv| {
        let want = std::fs::read(srv.docroot.join("hello.txt")).unwrap();
        let (status, body) = srv.connect().unwrap().do_request("hello.txt").unwrap();
        assert!(status.starts_with("+OK"), "[{}] expected +OK, got {status:?}", srv.label);
        assert_eq!(body.unwrap(), want, "[{}] content mismatch", srv.label);
    });
}

#[test]
fn test_serve_subdir_file() {
    for_each_server(|srv| {
        let want = std::fs::read(srv.docroot.join("subdir/nested.txt")).unwrap();
        let (status, body) = srv.connect().unwrap().do_request("subdir/nested.txt").unwrap();
        assert!(status.starts_with("+OK"), "[{}] expected +OK, got {status:?}", srv.label);
        assert_eq!(body.unwrap(), want, "[{}] content mismatch", srv.label);
    });
}

/// Two requests on one connection — verifies +OK framing across requests.
#[test]
fn test_multiple_requests_same_conn() {
    for_each_server(|srv| {
        let mut conn = srv.connect().unwrap();
        for name in ["hello.txt", "pangrams.txt"] {
            let want = std::fs::read(srv.docroot.join(name)).unwrap();
            let (status, body) = conn.do_request(name).unwrap();
            assert!(status.starts_with("+OK"), "[{}][{name}] expected +OK, got {status:?}", srv.label);
            assert_eq!(body.unwrap(), want, "[{}][{name}] content mismatch", srv.label);
        }
    });
}

#[test]
fn test_missing_file() {
    for_each_server(|srv| {
        let (status, _) = srv.connect().unwrap().do_request("no-such-file.txt").unwrap();
        assert!(status.starts_with("-ERR"), "[{}] expected -ERR, got {status:?}", srv.label);
    });
}

#[test]
fn test_absolute_path_rejected() {
    for_each_server(|srv| {
        let (status, _) = srv.connect().unwrap().do_request("/etc/passwd").unwrap();
        assert!(status.starts_with("-ERR"), "[{}] expected -ERR, got {status:?}", srv.label);
    });
}

#[test]
fn test_path_escape_rejected() {
    for_each_server(|srv| {
        for path in ["../../etc/passwd", "../file-server.c", "subdir/../../file-server.c"] {
            let (status, _) = srv.connect().unwrap().do_request(path).unwrap();
            assert!(
                status.starts_with("-ERR"),
                "[{}] escape {path:?} not rejected: got {status:?}", srv.label
            );
        }
    });
}

#[test]
fn test_abrupt_disconnect_survives() {
    for_each_server(|srv| {
        drop(srv.connect().unwrap());
        std::thread::sleep(Duration::from_millis(50));
        let (status, _) = srv.connect().unwrap().do_request("hello.txt").unwrap();
        assert!(status.starts_with("+OK"), "[{}] server dead after disconnect: {status:?}", srv.label);
    });
}

#[test]
fn test_partial_line_buffering() {
    for_each_server(|srv| {
        let stream = TcpStream::connect(format!("127.0.0.1:{}", srv.port)).unwrap();
        stream.set_read_timeout(Some(Duration::from_millis(150))).unwrap();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        let mut writer = stream.try_clone().unwrap();

        for b in b"hello.txt" {
            writer.write_all(&[*b]).unwrap();
            writer.flush().unwrap();
            std::thread::sleep(Duration::from_millis(20));
        }

        // No newline yet — server must NOT reply.
        let mut early = String::new();
        let _ = reader.read_line(&mut early);
        assert!(early.is_empty(), "[{}] replied before newline: {early:?}", srv.label);

        writer.write_all(b"\n").unwrap();
        writer.flush().unwrap();

        stream.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let mut status = String::new();
        reader.read_line(&mut status).unwrap();
        assert!(
            status.trim_end_matches(['\n', '\r']).starts_with("+OK"),
            "[{}] expected +OK after newline, got {status:?}", srv.label
        );
    });
}

#[test]
fn test_simultaneous_reads() {
    for_each_server(|srv| {
        let port   = srv.port;
        let files  = ["hello.txt", "pangrams.txt", "subdir/nested.txt"];
        let wants: Vec<Vec<u8>> = files.iter()
            .map(|f| std::fs::read(srv.docroot.join(f)).unwrap())
            .collect();

        let handles: Vec<_> = (0..20).map(|i| {
            let name  = files[i % files.len()].to_owned();
            let want  = wants[i % files.len()].clone();
            let label = srv.label.clone();
            std::thread::spawn(move || {
                let stream = TcpStream::connect(format!("127.0.0.1:{port}")).unwrap();
                stream.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
                let mut conn = Conn {
                    reader: BufReader::new(stream.try_clone().unwrap()),
                    writer: stream,
                };
                let (status, body) = conn.do_request(&name).unwrap();
                assert!(status.starts_with("+OK"), "[{label}] client {i} [{name}]: {status:?}");
                assert_eq!(body.unwrap(), want, "[{label}] client {i} [{name}]: mismatch");
            })
        }).collect();

        for h in handles { h.join().expect("thread panicked"); }
    });
}
