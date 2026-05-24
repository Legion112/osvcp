// Exercise 4 (Rust): async file-serving TCP server using Tokio
//
// Compare this to aio-file-server.c — both solve the same problem:
// never block the event loop on file I/O or slow clients.
//
// In C (POSIX AIO) we had to:
//   • Write a 3-state state machine per client (READING / AIO / WRITING)
//   • Poll aio_error() every loop iteration with a 1 ms timeout
//   • Manage O_NONBLOCK sockets and partial writes manually
//   • Handle aio_cancel() on disconnect
//   • ~400 lines of careful, error-prone C
//
// Here, the Tokio runtime handles all of that invisibly:
//   • tokio::fs::File         async file reads (thread pool or io_uring)
//   • tokio::net::TcpStream   async socket I/O (epoll/kqueue/IOCP)
//   • tokio::spawn            one lightweight task per connection
//   • async/await             the compiler generates the state machine
//
// The result is ~130 lines that are structurally identical to the
// original synchronous file-server.c — no manual state machine required.
//
// Usage:
//   cargo run --bin file-server-tokio -- [port [docroot]]
//   Default port:    8085
//   Default docroot: ./docroot

use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use tokio::fs;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, tcp::OwnedWriteHalf};

const DEFAULT_PORT: u16 = 8085;
const FILE_CHUNK:   usize = 4096;

// ── security ──────────────────────────────────────────────────────────────────

/// Validate the filename and resolve it to an absolute path inside docroot.
/// Returns `Ok(resolved)` or an error string to send to the client.
fn resolve_path(filename: &str, docroot: &Path) -> Result<PathBuf, &'static str> {
    if filename.is_empty()        { return Err("empty filename"); }
    if filename.starts_with('/')  { return Err("absolute paths not allowed"); }

    // std::fs::canonicalize calls realpath(2) — resolves ".." and symlinks.
    // Returns Err if the path does not exist, which we treat as "not found".
    let resolved = std::fs::canonicalize(docroot.join(filename))
        .map_err(|_| "not found")?;

    // Component-level prefix check: "/a/b" starts_with "/a" but "/ab" does not.
    if !resolved.starts_with(docroot) {
        return Err("access denied");
    }
    Ok(resolved)
}

// ── file serving ──────────────────────────────────────────────────────────────

/// Serve one file request: validate path, send +OK header, stream content.
/// Returns Err only on a broken pipe / network error (caller closes connection).
async fn serve_file(
    writer:   &mut OwnedWriteHalf,
    filename: &str,
    docroot:  &Path,
    peer:     &str,
) -> io::Result<()> {
    // Resolve and validate (sync — path resolution is fast on local disk).
    let resolved = match resolve_path(filename, docroot) {
        Ok(p)    => p,
        Err(msg) => {
            writer.write_all(format!("-ERR {msg}\n").as_bytes()).await?;
            return Ok(());
        }
    };

    // Check it is a regular file.
    let meta = match fs::metadata(&resolved).await {
        Ok(m) if m.is_file() => m,
        Ok(_)  => { writer.write_all(b"-ERR not a regular file\n").await?; return Ok(()); }
        Err(e) => { writer.write_all(format!("-ERR metadata: {e}\n").as_bytes()).await?; return Ok(()); }
    };

    // Open asynchronously — tokio delegates to a thread pool (or io_uring when
    // available) so this never blocks the executor.
    let file = match fs::File::open(&resolved).await {
        Ok(f)  => f,
        Err(e) => { writer.write_all(format!("-ERR open: {e}\n").as_bytes()).await?; return Ok(()); }
    };

    let size = meta.len();
    writer.write_all(format!("+OK {size}\n").as_bytes()).await?;

    // Stream the file in chunks. tokio::fs::File::read() is async; each .await
    // yields back to the executor if no data is immediately available, allowing
    // other tasks to run — no manual AIO polling or state machine needed.
    let mut reader = BufReader::with_capacity(FILE_CHUNK, file);
    let mut chunk  = vec![0u8; FILE_CHUNK];
    loop {
        let n = reader.read(&mut chunk).await?;
        if n == 0 { break; }
        writer.write_all(&chunk[..n]).await?;
    }

    println!("[{peer}] served '{filename}' ({size} bytes)");
    Ok(())
}

// ── per-connection handler ────────────────────────────────────────────────────

/// Handle all requests on one connection until the client disconnects.
/// Each connection runs in its own Tokio task — there is no shared mutable
/// state between tasks, so no locks are needed.
async fn handle_connection(stream: tokio::net::TcpStream, docroot: Arc<PathBuf>) {
    let peer = stream.peer_addr()
        .map(|a| a.to_string())
        .unwrap_or_else(|_| "unknown".into());
    println!("[{peer}] connected");

    // Split: the read half is line-buffered; the write half is used directly.
    let (read_half, mut write_half) = stream.into_split();
    let mut lines = BufReader::new(read_half).lines();

    // Read one request line at a time, serve each, repeat until EOF.
    loop {
        match lines.next_line().await {
            Ok(Some(line)) => {
                let filename = line.trim_end_matches(['\r', '\n']);
                if serve_file(&mut write_half, filename, &docroot, &peer).await.is_err() {
                    break;  // broken pipe — client disconnected
                }
            }
            _ => break,  // EOF or error
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

    // Resolve docroot once at startup (sync is fine here).
    let docroot = Arc::new(match std::fs::canonicalize(root) {
        Ok(p)  => p,
        Err(e) => { eprintln!("docroot '{root}': {e}"); std::process::exit(1); }
    });

    let listener = TcpListener::bind(("0.0.0.0", port)).await.unwrap_or_else(|e| {
        eprintln!("bind: {e}"); std::process::exit(1);
    });

    println!(
        "Tokio async file server listening on port {port}, docroot={}",
        docroot.display()
    );
    println!("(one task per connection, fully async I/O — no manual state machine)");

    // Accept loop: spawn a task per connection.
    // Each task is lightweight (~few KB stack) and independently scheduled by
    // Tokio — no select() loop, no fd_set, no O_NONBLOCK juggling.
    loop {
        match listener.accept().await {
            Ok((stream, _)) => {
                let docroot = Arc::clone(&docroot);
                tokio::spawn(handle_connection(stream, docroot));
            }
            Err(e) => eprintln!("accept: {e}"),
        }
    }
}
