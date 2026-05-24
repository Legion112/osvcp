// Exercise 4 (Rust): async file-serving TCP server using Tokio
//
// File cache (exercise 5):
//   • An Arc<Mutex<FileCache>> is shared between the accept loop and every
//     connection task.  On a cache hit the file is served from memory with no
//     tokio::fs I/O at all.
//   • A dedicated Tokio task listens for SIGUSR1 and flushes the cache.
//   • Signal safety: tokio::signal::unix converts the OS signal into an async
//     event, so no raw signal-handler concerns arise.
//
// Compare this to aio-file-server.c — both solve the same problem:
// never block the event loop on file I/O or slow clients.
//
// Usage:
//   cargo run --bin file-server-tokio -- [port [docroot]]
//   kill -USR1 <pid>    # flush cache

use std::collections::HashMap;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use tokio::fs;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, tcp::OwnedWriteHalf};
use tokio::signal;
use tokio::signal::unix::{signal as unix_signal, SignalKind};

const DEFAULT_PORT:         u16   = 8085;
const FILE_CHUNK:           usize = 4096;
const MAX_CACHE_ENTRIES:    usize = 16;
const MAX_CACHE_FILE_BYTES: u64   = 64 * 1024;

// ── file cache ────────────────────────────────────────────────────────────────

/// Thread-safe in-process cache shared across all connection tasks.
///
/// Values are pre-built response bytes (`+OK <n>\n` + file content) so a
/// cache hit requires no disk I/O — just a single `write_all`.
///
/// Arc<Mutex<…>> is safe here because the lock is held only briefly (HashMap
/// lookup / insert), never across an `.await` point.
type FileCache = Arc<Mutex<FileCacheInner>>;

struct FileCacheInner {
    entries: HashMap<PathBuf, Vec<u8>>,
    order:   Vec<PathBuf>,
}

impl FileCacheInner {
    fn new() -> Self {
        FileCacheInner { entries: HashMap::new(), order: Vec::new() }
    }

    fn get(&self, key: &Path) -> Option<Vec<u8>> {
        self.entries.get(key).cloned()
    }

    fn insert(&mut self, key: PathBuf, resp: Vec<u8>) {
        if self.entries.contains_key(&key) {
            self.entries.insert(key, resp);
            return;
        }
        if self.entries.len() >= MAX_CACHE_ENTRIES {
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

// ── security ──────────────────────────────────────────────────────────────────

fn resolve_path(filename: &str, docroot: &Path) -> Result<PathBuf, &'static str> {
    if filename.is_empty()       { return Err("empty filename"); }
    if filename.starts_with('/') { return Err("absolute paths not allowed"); }

    let resolved = std::fs::canonicalize(docroot.join(filename))
        .map_err(|_| "not found")?;
    if !resolved.starts_with(docroot) {
        return Err("access denied");
    }
    Ok(resolved)
}

// ── file serving ──────────────────────────────────────────────────────────────

async fn serve_file(
    writer:  &mut OwnedWriteHalf,
    name:    &str,
    docroot: &Path,
    peer:    &str,
    cache:   &FileCache,
) -> io::Result<()> {
    let resolved = match resolve_path(name, docroot) {
        Ok(p)    => p,
        Err(msg) => {
            writer.write_all(format!("-ERR {msg}\n").as_bytes()).await?;
            return Ok(());
        }
    };

    // ── Cache hit ────────────────────────────────────────────────────────────
    // Scope the lock so the guard is dropped before any .await point.
    // (MutexGuard is !Send so tokio::spawn requires it not to be held across
    // an await.)
    let cached: Option<Vec<u8>> = cache.lock().unwrap().get(&resolved);
    if let Some(resp) = cached {
        writer.write_all(&resp).await?;
        println!("[{peer}] served '{name}' [cache]");
        return Ok(());
    }

    // ── Cache miss: read from disk ────────────────────────────────────────────
    let meta = match fs::metadata(&resolved).await {
        Ok(m) if m.is_file() => m,
        Ok(_)  => { writer.write_all(b"-ERR not a regular file\n").await?; return Ok(()); }
        Err(e) => { writer.write_all(format!("-ERR metadata: {e}\n").as_bytes()).await?; return Ok(()); }
    };

    let file = match fs::File::open(&resolved).await {
        Ok(f)  => f,
        Err(e) => { writer.write_all(format!("-ERR open: {e}\n").as_bytes()).await?; return Ok(()); }
    };

    let size   = meta.len();
    let header = format!("+OK {size}\n");

    if size <= MAX_CACHE_FILE_BYTES {
        // Read entire file, build full response, cache, send.
        let mut body = vec![0u8; size as usize];
        let mut reader = BufReader::with_capacity(FILE_CHUNK, file);
        reader.read_exact(&mut body).await?;

        let mut resp = Vec::with_capacity(header.len() + body.len());
        resp.extend_from_slice(header.as_bytes());
        resp.extend_from_slice(&body);

        writer.write_all(&resp).await?;
        cache.lock().unwrap().insert(resolved, resp);
    } else {
        // Large file: stream without caching.
        writer.write_all(header.as_bytes()).await?;
        let mut reader = BufReader::with_capacity(FILE_CHUNK, file);
        let mut chunk  = vec![0u8; FILE_CHUNK];
        loop {
            let n = reader.read(&mut chunk).await?;
            if n == 0 { break; }
            writer.write_all(&chunk[..n]).await?;
        }
    }

    println!("[{peer}] served '{name}' ({size} bytes)");
    Ok(())
}

// ── per-connection handler ────────────────────────────────────────────────────

async fn handle_connection(
    stream:  tokio::net::TcpStream,
    docroot: Arc<PathBuf>,
    cache:   FileCache,
) {
    let peer = stream.peer_addr()
        .map(|a| a.to_string())
        .unwrap_or_else(|_| "unknown".into());
    println!("[{peer}] connected");

    let (read_half, mut write_half) = stream.into_split();
    let mut lines = BufReader::new(read_half).lines();

    loop {
        match lines.next_line().await {
            Ok(Some(line)) => {
                let filename = line.trim_end_matches(['\r', '\n']);
                if serve_file(&mut write_half, filename, &docroot, &peer, &cache)
                    .await
                    .is_err()
                {
                    break;
                }
            }
            _ => break,
        }
    }

    println!("[{peer}] disconnected");
}

// ── main ──────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    let port: u16  = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(DEFAULT_PORT);
    let root: &str = args.get(2).map(String::as_str).unwrap_or("./docroot");

    let docroot = Arc::new(match std::fs::canonicalize(root) {
        Ok(p)  => p,
        Err(e) => { eprintln!("docroot '{root}': {e}"); std::process::exit(1); }
    });

    let cache: FileCache = Arc::new(Mutex::new(FileCacheInner::new()));

    let listener = TcpListener::bind(("0.0.0.0", port)).await.unwrap_or_else(|e| {
        eprintln!("bind: {e}"); std::process::exit(1);
    });

    println!(
        "Tokio async file server listening on port {port}, docroot={}",
        docroot.display()
    );
    println!("(send SIGUSR1 to flush the file cache)");

    // Spawn a task that listens for SIGUSR1 and clears the cache.
    // tokio::signal::unix converts the OS signal into an async event stream,
    // so this task parks itself cheaply between signals.
    let cache_for_signal = Arc::clone(&cache);
    tokio::spawn(async move {
        let mut sigusr1 = unix_signal(SignalKind::user_defined1())
            .expect("SIGUSR1 handler");
        loop {
            sigusr1.recv().await;
            cache_for_signal.lock().unwrap().clear();
        }
    });

    // Accept loop races against SIGINT (ctrl_c).
    tokio::select! {
        _ = signal::ctrl_c() => {},
        _ = async {
            loop {
                match listener.accept().await {
                    Ok((stream, _)) => {
                        let docroot = Arc::clone(&docroot);
                        let cache   = Arc::clone(&cache);
                        tokio::spawn(handle_connection(stream, docroot, cache));
                    }
                    Err(e) => eprintln!("accept: {e}"),
                }
            }
        } => {},
    }
}
