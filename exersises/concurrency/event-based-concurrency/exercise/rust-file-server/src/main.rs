// Exercise 3 (Rust): select()-based file-serving TCP server
//
// Mirrors the C implementation so the two can be read side-by-side.
//
// Protocol (line-oriented, connection stays open for multiple requests):
//
//   Client → Server:   <relative-filename>\n
//   Server → Client:   +OK <size>\n<file content>
//                  or: -ERR <reason>\n
//
// Security:
//   • All files must live under the configured docroot.
//   • std::fs::canonicalize() resolves symlinks and ".." components before
//     any path is opened, equivalent to the C realpath() call.
//   • Path::starts_with() does component-level prefix matching, so
//     "/docroot_extra" does NOT match docroot "/docroot".
//   • Absolute request paths ("/etc/passwd") are rejected immediately.
//   • Only regular files are served (directories, symlinks, etc. rejected).
//
// Usage:
//   cargo run -- [port [docroot]]
//   Default port:    8083
//   Default docroot: ./docroot

use std::fs;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::io::{AsRawFd, BorrowedFd, RawFd};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

use nix::sys::select::{select, FdSet};
use nix::sys::signal::{signal, SigHandler, Signal};

// ── constants ─────────────────────────────────────────────────────────────────

const DEFAULT_PORT: u16 = 8083;
const MAX_CLIENTS: usize = 64;
const REQ_BUF_SIZE: usize = 512;
const FILE_CHUNK: usize = 4096;

// ── signal handling ───────────────────────────────────────────────────────────

static RUNNING: AtomicBool = AtomicBool::new(true);

extern "C" fn handle_sigint(_: libc::c_int) {
    RUNNING.store(false, Ordering::Relaxed);
}

// ── per-connection state ──────────────────────────────────────────────────────

struct Client {
    stream:    TcpStream,
    buf:       Vec<u8>,      // partial-line accumulator
    peer_addr: String,
}

impl Client {
    fn new(stream: TcpStream) -> Self {
        let peer_addr = stream
            .peer_addr()
            .map(|a| a.to_string())
            .unwrap_or_else(|_| "unknown".into());
        Client { stream, buf: Vec::new(), peer_addr }
    }
}

// ── file serving ──────────────────────────────────────────────────────────────

/// Send a response and flush. Returns Err only on broken pipe / write failure.
fn send(stream: &mut TcpStream, msg: &[u8]) -> io::Result<()> {
    stream.write_all(msg)
}

/// Serve one file request. Returns Ok(false) if the connection should close.
fn serve_file(stream: &mut TcpStream, peer: &str, filename: &str, docroot: &Path) -> io::Result<bool> {
    // ── Security check 1: reject empty and absolute paths ────────────────────
    if filename.is_empty() {
        send(stream, b"-ERR empty filename\n")?;
        return Ok(true);
    }
    if filename.starts_with('/') {
        send(stream, b"-ERR absolute paths not allowed\n")?;
        return Ok(true);
    }

    // ── Security check 2: canonicalize and verify prefix ─────────────────────
    // canonicalize() calls realpath(2) under the hood, resolving all symlinks
    // and ".." components. It returns Err if the path does not exist.
    let candidate = docroot.join(filename);
    let resolved = match fs::canonicalize(&candidate) {
        Ok(p)  => p,
        Err(_) => {
            send(stream, b"-ERR not found\n")?;
            println!("[{peer}] not found: {filename}");
            return Ok(true);
        }
    };

    // Path::starts_with() does component-level matching: "/a/b" starts with
    // "/a" but "/a_extra" does NOT — immune to prefix confusion attacks.
    if !resolved.starts_with(docroot) {
        send(stream, b"-ERR access denied\n")?;
        println!("[{peer}] path escape attempt: {filename} → {}", resolved.display());
        return Ok(true);
    }

    // ── Security check 3: regular files only ─────────────────────────────────
    let meta = match fs::metadata(&resolved) {
        Ok(m)  => m,
        Err(e) => {
            send(stream, format!("-ERR stat: {e}\n").as_bytes())?;
            return Ok(true);
        }
    };
    if !meta.is_file() {
        send(stream, b"-ERR not a regular file\n")?;
        return Ok(true);
    }

    // ── Open / read / close using std::fs::File ───────────────────────────────
    // std::fs::File wraps the open(2)/read(2)/close(2) syscalls.
    let mut file = match fs::File::open(&resolved) {
        Ok(f)  => f,
        Err(e) => {
            send(stream, format!("-ERR open: {e}\n").as_bytes())?;
            return Ok(true);
        }
    };

    // Send "+OK <size>\n" header first so the client knows how many bytes follow.
    let size = meta.len();
    send(stream, format!("+OK {size}\n").as_bytes())?;

    // Stream file contents in fixed-size chunks.
    let mut chunk = [0u8; FILE_CHUNK];
    loop {
        let n = file.read(&mut chunk)?;
        if n == 0 { break; }
        stream.write_all(&chunk[..n])?;
    }

    println!("[{peer}] served '{filename}' ({size} bytes)");
    Ok(true)
}

// ── per-client processing ─────────────────────────────────────────────────────

/// Drain as many complete newline-terminated lines as possible from the client
/// buffer, serving each as a file request.
/// Returns false if the connection should be dropped.
fn process_client(client: &mut Client, docroot: &Path) -> bool {
    // find and process all complete lines currently in the buffer
    loop {
        let Some(nl) = client.buf.iter().position(|&b| b == b'\n') else { break };

        // Extract the line (without the '\n'), stripping an optional '\r'.
        let mut line = client.buf.drain(..=nl).collect::<Vec<u8>>();
        line.pop(); // remove '\n'
        if line.last() == Some(&b'\r') { line.pop(); }

        let filename = String::from_utf8_lossy(&line);
        match serve_file(&mut client.stream, &client.peer_addr, &filename, docroot) {
            Ok(true)  => {} // keep going
            Ok(false) | Err(_) => return false,
        }
    }

    // Guard: if the buffer filled without a newline, the client is misbehaving.
    if client.buf.len() >= REQ_BUF_SIZE - 1 {
        eprintln!("[{}] request too long — closing", client.peer_addr);
        return false;
    }
    true
}

// ── event loop ────────────────────────────────────────────────────────────────

fn event_loop(listener: TcpListener, docroot: PathBuf) {
    let listen_fd: RawFd = listener.as_raw_fd();
    let mut clients: Vec<Client> = Vec::new();

    while RUNNING.load(Ordering::Relaxed) {
        // ── Step 1: rebuild FdSet from scratch every iteration ────────────────
        // select(2) modifies the set in place, so we can never reuse it.
        let mut read_fds = FdSet::new();
        // SAFETY: all fds are valid and owned by live sockets for this scope.
        let blistenfd = unsafe { BorrowedFd::borrow_raw(listen_fd) };
        read_fds.insert(blistenfd);
        let mut max_fd = listen_fd;

        for c in &clients {
            let fd = c.stream.as_raw_fd();
            read_fds.insert(unsafe { BorrowedFd::borrow_raw(fd) });
            if fd > max_fd { max_fd = fd; }
        }

        // ── Step 2: block until at least one fd is ready ──────────────────────
        match select(max_fd + 1, Some(&mut read_fds), None, None, None) {
            Ok(_)  => {}
            Err(nix::errno::Errno::EINTR) => break, // SIGINT
            Err(e) => { eprintln!("select: {e}"); break; }
        }

        // ── Step 3: accept all pending new connections ────────────────────────
        if read_fds.contains(unsafe { BorrowedFd::borrow_raw(listen_fd) }) {
            loop {
                if clients.len() >= MAX_CLIENTS { break; }
                match listener.accept() {
                    Ok((stream, addr)) => {
                        let _ = stream.set_nonblocking(false);
                        println!("[{addr}] connected ({} clients)", clients.len() + 1);
                        clients.push(Client::new(stream));
                    }
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => break,
                    Err(e) => { eprintln!("accept: {e}"); break; }
                }
            }
        }

        // ── Step 4: service readable client sockets ───────────────────────────
        let mut i = clients.len();
        while i > 0 {
            i -= 1;
            let fd = clients[i].stream.as_raw_fd();
            if !read_fds.contains(unsafe { BorrowedFd::borrow_raw(fd) }) { continue; }

            let mut tmp = [0u8; REQ_BUF_SIZE];
            let keep = match clients[i].stream.read(&mut tmp) {
                Ok(0)  => false,
                Ok(n)  => {
                    clients[i].buf.extend_from_slice(&tmp[..n]);
                    process_client(&mut clients[i], &docroot)
                }
                Err(e) if e.kind() == io::ErrorKind::ConnectionReset => false,
                Err(e) => { eprintln!("read: {e}"); false }
            };

            if !keep {
                println!("[{}] disconnected", clients[i].peer_addr);
                clients.swap_remove(i);
            }
        }
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let port: u16  = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(DEFAULT_PORT);
    let root: &str = args.get(2).map(String::as_str).unwrap_or("./docroot");

    // Resolve docroot to a canonical absolute path once at startup.
    let docroot = match fs::canonicalize(root) {
        Ok(p)  => p,
        Err(e) => { eprintln!("docroot '{root}': {e}"); std::process::exit(1); }
    };

    // Register SIGINT handler.
    unsafe {
        signal(Signal::SIGINT,  SigHandler::Handler(handle_sigint)).unwrap();
        signal(Signal::SIGPIPE, SigHandler::SigIgn).unwrap();
    }

    // Bind the listening socket in non-blocking mode so the accept() drain
    // loop terminates on EAGAIN without blocking the whole server.
    let listener = TcpListener::bind(("0.0.0.0", port)).unwrap_or_else(|e| {
        eprintln!("bind: {e}");
        std::process::exit(1);
    });
    listener.set_nonblocking(true).unwrap();

    println!("Rust file server listening on port {port}, docroot={}", docroot.display());
    event_loop(listener, docroot);
    println!("\nServer shut down.");
}
