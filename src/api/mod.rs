use crate::{ExportOptions, Kernel, KernelConfig, TimeBucket};
use anyhow::{anyhow, Result};
use rand::RngCore;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;

const MAX_REQUEST_BYTES: usize = 8192;

#[derive(Clone, Debug)]
pub struct ApiConfig {
    pub addr: String,
    pub export_options: ExportOptions,
    pub token_path: Option<PathBuf>,
}

impl Default for ApiConfig {
    fn default() -> Self {
        Self {
            addr: "127.0.0.1:8799".to_string(),
            export_options: ExportOptions::default(),
            token_path: None,
        }
    }
}

#[derive(Debug)]
pub struct ApiHandle {
    pub addr: SocketAddr,
    pub token: String,
    pub token_path: Option<PathBuf>,
    shutdown: Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

impl ApiHandle {
    pub fn stop(mut self) -> Result<()> {
        self.shutdown.store(true, Ordering::SeqCst);
        if let Some(join) = self.join.take() {
            join.join()
                .map_err(|_| anyhow!("api server thread panicked"))?;
        }
        Ok(())
    }
}

#[derive(Clone, Debug)]
pub struct CapabilityTokenManager {
    current_bucket: Option<TimeBucket>,
    token: [u8; 32],
}

impl CapabilityTokenManager {
    pub fn new(bucket: TimeBucket) -> Result<Self> {
        let mut token = [0u8; 32];
        rand::thread_rng().fill_bytes(&mut token);
        Ok(Self {
            current_bucket: Some(bucket),
            token,
        })
    }

    pub fn rotate_if_needed(&mut self, bucket: TimeBucket) -> Result<bool> {
        if self.current_bucket == Some(bucket) {
            return Ok(false);
        }
        let mut token = [0u8; 32];
        rand::thread_rng().fill_bytes(&mut token);
        self.current_bucket = Some(bucket);
        self.token = token;
        Ok(true)
    }

    pub fn token_hex(&self) -> String {
        hex::encode(self.token)
    }

    pub fn validate(&self, presented: &str, bucket: TimeBucket) -> Result<()> {
        if self.current_bucket != Some(bucket) {
            return Err(anyhow!("capability token expired"));
        }
        let presented = parse_hex32(presented)?;
        if presented != self.token {
            return Err(anyhow!("capability token invalid"));
        }
        Ok(())
    }
}

pub struct ApiServer {
    cfg: ApiConfig,
    kernel_cfg: KernelConfig,
}

impl ApiServer {
    pub fn new(cfg: ApiConfig, kernel_cfg: KernelConfig) -> Self {
        Self { cfg, kernel_cfg }
    }

    pub fn spawn(self) -> Result<ApiHandle> {
        let configured_addr: SocketAddr = self.cfg.addr.parse()?;
        let listener = TcpListener::bind(configured_addr)?;
        let addr = listener.local_addr()?;
        if configured_addr.ip().is_loopback() && !addr.ip().is_loopback() {
            return Err(anyhow!(
                "api configured for loopback address '{}', but bound to non-loopback address '{}'",
                configured_addr,
                addr
            ));
        }
        listener.set_nonblocking(true)?;

        let bucket = TimeBucket::now_10min()?;
        let mut token_mgr = CapabilityTokenManager::new(bucket)?;
        let token = token_mgr.token_hex();
        if let Some(path) = &self.cfg.token_path {
            write_token_file(path, &token)?;
        }

        let shutdown = Arc::new(AtomicBool::new(false));
        let shutdown_thread = shutdown.clone();
        let cfg = self.cfg.clone();
        let kernel_cfg = self.kernel_cfg.clone();
        let token_path = cfg.token_path.clone();
        let join = std::thread::spawn(move || {
            if let Err(err) = run_api(listener, cfg, kernel_cfg, &mut token_mgr, shutdown_thread) {
                log::error!("event api stopped: {}", err);
            }
        });

        Ok(ApiHandle {
            addr,
            token,
            token_path,
            shutdown,
            join: Some(join),
        })
    }
}

fn run_api(
    listener: TcpListener,
    cfg: ApiConfig,
    kernel_cfg: KernelConfig,
    token_mgr: &mut CapabilityTokenManager,
    shutdown: Arc<AtomicBool>,
) -> Result<()> {
    let mut kernel = Kernel::open(&kernel_cfg)?;
    let expected_ruleset_hash = kernel_cfg.ruleset_hash;
    loop {
        if shutdown.load(Ordering::SeqCst) {
            break;
        }
        match listener.accept() {
            Ok((stream, _)) => {
                if let Err(err) =
                    handle_connection(stream, &mut kernel, &cfg, token_mgr, expected_ruleset_hash)
                {
                    log::warn!("event api request rejected: {}", err);
                }
            }
            Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(50));
                continue;
            }
            Err(err) => return Err(err.into()),
        }
    }
    Ok(())
}

fn handle_connection(
    mut stream: TcpStream,
    kernel: &mut Kernel,
    cfg: &ApiConfig,
    token_mgr: &mut CapabilityTokenManager,
    expected_ruleset_hash: [u8; 32],
) -> Result<()> {
    let peer = stream.peer_addr()?;
    let local = stream.local_addr()?;
    if local.ip().is_loopback() && !peer.ip().is_loopback() {
        write_json_response(&mut stream, 403, r#"{"error":"forbidden"}"#)?;
        return Ok(());
    }

    let request = read_request(&mut stream)?;
    if request.method != "GET" {
        write_json_response(&mut stream, 405, r#"{"error":"method_not_allowed"}"#)?;
        return Ok(());
    }
    match request.path.as_str() {
        "/health" => {
            write_json_response(&mut stream, 200, r#"{"status":"ok"}"#)?;
            return Ok(());
        }
        "/events" | "/events/latest" => {}
        _ => {
            write_json_response(&mut stream, 404, r#"{"error":"not_found"}"#)?;
            return Ok(());
        }
    }

    if request.has_query_token() {
        write_json_response(
            &mut stream,
            400,
            r#"{"error":"token_query_param_not_allowed"}"#,
        )?;
        return Ok(());
    }

    let token = match request.bearer_token() {
        Some(token) => token,
        None => {
            write_json_response(&mut stream, 401, r#"{"error":"missing_token"}"#)?;
            return Ok(());
        }
    };

    let now_bucket = TimeBucket::now_10min()?;
    if token_mgr.rotate_if_needed(now_bucket)? {
        if let Some(path) = &cfg.token_path {
            write_token_file(path, &token_mgr.token_hex())?;
        } else {
            log::warn!(
                "event api capability token rotated; configure WITNESS_API_TOKEN_PATH to persist"
            );
            log::warn!(
                "event api capability token (handle securely): {}",
                token_mgr.token_hex()
            );
        }
    }

    if let Err(err) = token_mgr.validate(&token, now_bucket) {
        write_json_response(&mut stream, 401, r#"{"error":"invalid_token"}"#)?;
        return Err(err);
    }

    let artifact = kernel.export_events_for_api(expected_ruleset_hash, cfg.export_options)?;
    if request.path == "/events/latest" {
        if let Some(event) = latest_event(&artifact) {
            let payload = serde_json::to_vec(event)?;
            write_response(&mut stream, 200, "application/json", &payload)?;
        } else {
            write_json_response(&mut stream, 404, r#"{"error":"no_events"}"#)?;
        }
        return Ok(());
    }

    let payload = serde_json::to_vec(&artifact)?;
    write_response(&mut stream, 200, "application/json", &payload)?;
    Ok(())
}

fn read_request(stream: &mut TcpStream) -> Result<HttpRequest> {
    stream.set_read_timeout(Some(Duration::from_secs(2)))?;
    let mut buf = [0u8; 1024];
    let mut data = Vec::new();
    loop {
        let n = stream.read(&mut buf)?;
        if n == 0 {
            break;
        }
        data.extend_from_slice(&buf[..n]);
        if data.len() > MAX_REQUEST_BYTES {
            return Err(anyhow!("request too large"));
        }
        if data.windows(4).any(|w| w == b"\r\n\r\n") {
            break;
        }
    }
    let text = String::from_utf8_lossy(&data);
    let mut lines = text.split("\r\n");
    let request_line = lines.next().ok_or_else(|| anyhow!("empty request"))?;
    let mut parts = request_line.split_whitespace();
    let method = parts.next().ok_or_else(|| anyhow!("missing method"))?;
    let raw_path = parts.next().ok_or_else(|| anyhow!("missing path"))?;
    let mut headers = HashMap::new();
    for line in lines {
        if line.is_empty() {
            break;
        }
        if let Some((k, v)) = line.split_once(':') {
            headers.insert(k.trim().to_lowercase(), v.trim().to_string());
        }
    }
    let path = raw_path.split('?').next().unwrap_or(raw_path).to_string();
    Ok(HttpRequest {
        method: method.to_string(),
        path,
        headers,
        raw_path: raw_path.to_string(),
    })
}

fn write_json_response(stream: &mut TcpStream, status: u16, body: &str) -> Result<()> {
    write_response(stream, status, "application/json", body.as_bytes())
}

fn write_response(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    body: &[u8],
) -> Result<()> {
    let status_line = match status {
        200 => "HTTP/1.1 200 OK",
        401 => "HTTP/1.1 401 Unauthorized",
        403 => "HTTP/1.1 403 Forbidden",
        404 => "HTTP/1.1 404 Not Found",
        405 => "HTTP/1.1 405 Method Not Allowed",
        _ => "HTTP/1.1 500 Internal Server Error",
    };
    let header = format!(
        "{status_line}\r\nContent-Type: {content_type}\r\nContent-Length: {len}\r\nCache-Control: no-store\r\n\r\n",
        status_line = status_line,
        content_type = content_type,
        len = body.len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body)?;
    Ok(())
}

#[derive(Debug)]
struct HttpRequest {
    method: String,
    path: String,
    headers: HashMap<String, String>,
    raw_path: String,
}

impl HttpRequest {
    fn bearer_token(&self) -> Option<String> {
        if let Some(value) = self.headers.get("authorization") {
            let parts: Vec<&str> = value.split_whitespace().collect();
            if parts.len() == 2 && parts[0].eq_ignore_ascii_case("bearer") {
                return Some(parts[1].to_string());
            }
        }
        None
    }

    fn has_query_token(&self) -> bool {
        if let Some(query) = self.raw_path.split('?').nth(1) {
            for pair in query.split('&') {
                if let Some((k, _)) = pair.split_once('=') {
                    if k == "token" {
                        return true;
                    }
                }
            }
        }
        false
    }
}

fn write_token_file(path: &Path, token: &str) -> Result<()> {
    std::fs::write(path, format!("{token}\n"))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let perms = std::fs::Permissions::from_mode(0o600);
        std::fs::set_permissions(path, perms)?;
    }
    Ok(())
}

fn parse_hex32(value: &str) -> Result<[u8; 32]> {
    let bytes = hex::decode(value)?;
    if bytes.len() != 32 {
        return Err(anyhow!("token must be 32 bytes"));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn latest_event(artifact: &crate::ExportArtifact) -> Option<&crate::ExportEvent> {
    artifact
        .batches
        .iter()
        .flat_map(|batch| &batch.buckets)
        .flat_map(|bucket| &bucket.events)
        .last()
}
