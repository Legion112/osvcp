// Exercise 3 (Rust): select()-based file-serving TCP server
//
// Mirrors the C implementation so the two can be read side-by-side.
//
// File cache (exercise 5):
//   • A small in-process cache avoids repeated disk reads for hot files.
//   • Sending SIGUSR1 flushes the entire cache (administrative action).
//   • Signal safety: the handler only sets an AtomicBool flag; the actual
//     cache clear happens in the event loop, never inside the handler.
//
// Protocol (line-oriented, connection stays open for multiple requests):
//
//   Client → Server:   <relative-filename>\n
//   Server → Client:   +OK <size>\n<file content>
//                  or: -ERR <reason>\n
//
// Usage:
//   cargo run -- [port [docroot]]
//   kill -USR1 <pid>    # flush cache

use std::collections::HashMap;
use std::fs;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::io::{AsRawFd, BorrowedFd, RawFd};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

use nix::sys::select::{select, FdSet};
use nix::sys::signal::{signal, SigHandler, Signal};

// ── constants ─────────────────────────────────────────────────────────────────

const DEFAULT_PORT:         u16   = 8083;
const MAX_CLIENTS:          usize = 64;
const REQ_BUF_SIZE:         usize = 512;
const FILE_CHUNK:           usize = 4096;
const MAX_CACHE_ENTRIES:    usize = 16;
const MAX_CACHE_FILE_BYTES: usize = 64 * 1024;

// ── signal handling ───────────────────────────────────────────────────────────

static RUNNING:     AtomicBool = AtomicBool::new(true);
static CACHE_CLEAR: AtomicBool = AtomicBool::new(false);

extern "C" fn handle_sigint(_: libc::c_int) {
    RUNNING.store(false, Ordering::Relaxed);
}

extern "C" fn handle_sigusr1(_: libc::c_int) {
    CACHE_CLEAR.store(true, Ordering::Relaxed);
}

// ── file cache ────────────────────────────────────────────────────────────────

/// In-process file cache.
///
/// Stores pre-built responses (`+OK <n>\n` + file bytes) keyed by the
/// resolved canonical path.  When the table reaches MAX_CACHE_ENTRIES the
/// oldest entry is evicted (FIFO – tracked via `order`).
///
/// Thread-safety: the server runs on a single thread so a plain
/// non-Send/non-Sync struct is fine here.
struct FileCache {
    entries: HashMap<PathBuf, Vec<u8>>,
    order:   Vec<PathBuf>,              // insertion order for FIFO eviction
}

impl FileCache {
    fn new() -> Self {
        FileCache {
            entries: HashMap::new(),
            order:   Vec::new(),
        }
    }

    fn get(&self, key: &Path) -> Option<&Vec<u8>> {
        self.entries.get(key)
    }

    fn insert(&mut self, key: PathBuf, resp: Vec<u8>) {
        if self.entries.contains_key(&key) {
            // Update in-place without changing eviction order.
            self.entries.insert(key, resp);
            return;
        }
        if self.entries.len() >= MAX_CACHE_ENTRIES {
            // Evict the oldest entry.
            if !self.order.is_empty() {
                let oldest = self.order.remove(0);
                self.entries.remove(&oldest);
            }
        }
        self.order.push(key.clone());
        self.entries.insert(key, resp);
    }

    fn clear(&mut self) {
        let n = self.entries.len();
        self.entries.clear();
        self.order.clear();
        eprintln!("[cache] cleared ({n} entries flushed)");
    }
}

// ── per-connection state ──────────────────────────────────────────────────────

struct Client {
    stream:    TcpStream,
    buf:       Vec<u8>,
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

fn serve_file(
    stream:  &mut TcpStream,
    peer:    &str,
    name:    &str,
    docroot: &Path,
    cache:   &mut FileCache,
) -> io::Result<bool> {
    if name.is_empty() {
        stream.write_all(b"-ERR empty filename\n")?;
        return Ok(true);
    }
    if name.starts_with('/') {
        stream.write_all(b"-ERR absolute paths not allowed\n")?;
        return Ok(true);
    }

    let candidate = docroot.join(name);
    let resolved = match fs::canonicalize(&candidate) {
        Ok(p)  => p,
        Err(_) => {
            stream.write_all(b"-ERR not found\n")?;
            return Ok(true);
        }
    };
    if !resolved.starts_with(docroot) {
        stream.write_all(b"-ERR access denied\n")?;
        return Ok(true);
    }

    // ── Cache hit: send pre-built response, no disk I/O ──────────────────────
    if let Some(resp) = cache.get(&resolved) {
        stream.write_all(resp)?;
        println!("[{peer}] served '{name}' [cache]");
        return Ok(true);
    }

    // ── Cache miss: read from disk, maybe populate cache ─────────────────────
    let meta = match fs::metadata(&resolved) {
        Ok(m)  => m,
        Err(e) => {
            stream.write_all(format!("-ERR stat: {e}\n").as_bytes())?;
            return Ok(true);
        }
    };
    if !meta.is_file() {
        stream.write_all(b"-ERR not a regular file\n")?;
        return Ok(true);
    }

    let mut file = match fs::File::open(&resolved) {
        Ok(f)  => f,
        Err(e) => {
            stream.write_all(format!("-ERR open: {e}\n").as_bytes())?;
            return Ok(true);
        }
    };

    let size    = meta.len();
    let header  = format!("+OK {size}\n");

    if size <= MAX_CACHE_FILE_BYTES as u64 {
        // Read entire file into buffer, cache, then send in one call.
        let mut body = vec![0u8; size as usize];
        file.read_exact(&mut body)?;

        let mut resp = Vec::with_capacity(header.len() + body.len());
        resp.extend_from_slice(header.as_bytes());
        resp.extend_from_slice(&body);

        stream.write_all(&resp)?;
        cache.insert(resolved, resp);
    } else {
        // Large file: stream without caching.
        stream.write_all(header.as_bytes())?;
        let mut chunk = [0u8; FILE_CHUNK];
        loop {
            let n = file.read(&mut chunk)?;
            if n == 0 { break; }
            stream.write_all(&chunk[..n])?;
        }
    }

    println!("[{peer}] served '{name}' ({size} bytes)");
    Ok(true)
}

// ── per-client processing ─────────────────────────────────────────────────────

fn process_client(client: &mut Client, docroot: &Path, cache: &mut FileCache) -> bool {
    loop {
        let Some(nl) = client.buf.iter().position(|&b| b == b'\n') else { break };

        let mut line = client.buf.drain(..=nl).collect::<Vec<u8>>();
        line.pop();
        if line.last() == Some(&b'\r') { line.pop(); }

        let filename = String::from_utf8_lossy(&line);
        match serve_file(&mut client.stream, &client.peer_addr, &filename, docroot, cache) {
            Ok(true)  => {}
            Ok(false) | Err(_) => return false,
        }
    }

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
    let mut cache = FileCache::new();

    while RUNNING.load(Ordering::Relaxed) {
        // Process any pending cache-clear signal before blocking.
        if CACHE_CLEAR.swap(false, Ordering::Relaxed) {
            cache.clear();
        }

        let mut read_fds = FdSet::new();
        let blistenfd = unsafe { BorrowedFd::borrow_raw(listen_fd) };
        read_fds.insert(blistenfd);
        let mut max_fd = listen_fd;

        for c in &clients {
            let fd = c.stream.as_raw_fd();
            read_fds.insert(unsafe { BorrowedFd::borrow_raw(fd) });
            if fd > max_fd { max_fd = fd; }
        }

        match select(max_fd + 1, Some(&mut read_fds), None, None, None) {
            Ok(_)  => {}
            Err(nix::errno::Errno::EINTR) => {
                // A signal interrupted select().  Re-evaluate the loop
                // condition and the CACHE_CLEAR flag at the top.
                continue;
            }
            Err(e) => { eprintln!("select: {e}"); break; }
        }

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
                    process_client(&mut clients[i], &docroot, &mut cache)
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

    let docroot = match fs::canonicalize(root) {
        Ok(p)  => p,
        Err(e) => { eprintln!("docroot '{root}': {e}"); std::process::exit(1); }
    };

    unsafe {
        signal(Signal::SIGINT,  SigHandler::Handler(handle_sigint)).unwrap();
        signal(Signal::SIGUSR1, SigHandler::Handler(handle_sigusr1)).unwrap();
        signal(Signal::SIGPIPE, SigHandler::SigIgn).unwrap();
    }

    let listener = TcpListener::bind(("0.0.0.0", port)).unwrap_or_else(|e| {
        eprintln!("bind: {e}");
        std::process::exit(1);
    });
    listener.set_nonblocking(true).unwrap();

    println!("Rust file server listening on port {port}, docroot={}", docroot.display());
    println!("(send SIGUSR1 to flush the file cache)");
    event_loop(listener, docroot);
    println!("\nServer shut down.");
}
