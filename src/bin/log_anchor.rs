//! log_anchor - Anchor the witness chain to an external RFC 3161 TSA.
//!
//! Each anchor is a Time Stamping Authority's countersignature over the
//! chain head hash: independent, third-party proof that the chain existed
//! in its current state at a point in time. A verifier with an anchor no
//! longer needs to trust the device clock or device key alone — history
//! recorded before an anchor cannot be silently rewritten afterwards, even
//! by someone holding the device key.
//!
//! Anchoring is always operator-initiated (SecuraCV never makes outbound
//! network calls on its own) and the request carries only a 32-byte hash
//! plus a random nonce. Two flows:
//!
//!   online   log_anchor request --url https://...   (needs `--features tsa`)
//!   offline  log_anchor query --out chain.tsq        → submit out-of-band →
//!            log_anchor import --response chain.tsr
//!
//! `log_anchor verify` re-checks every stored anchor structurally (imprint
//! belongs to recorded chain history) and, given the TSA's CA certificate,
//! delegates the CMS countersignature check to an independent
//! implementation: `openssl ts -verify`.

use anyhow::{anyhow, bail, Context, Result};
use clap::{Parser, Subcommand};
use rusqlite::Connection;

use witness_kernel::tsa;

#[derive(Parser, Debug)]
#[command(
    name = "log_anchor",
    about = "Anchor the sealed witness chain to an RFC 3161 timestamping authority"
)]
struct Args {
    /// Path to the witness SQLite DB
    #[arg(long, default_value = "witness.db", global = true)]
    db: String,

    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Request a timestamp for the current chain head from a TSA over HTTPS
    Request {
        /// TSA endpoint, e.g. <https://freetsa.org/tsr>
        #[arg(long)]
        url: String,
        /// Permit a plaintext http:// TSA endpoint (token integrity does not
        /// depend on transport, but https is required by default)
        #[arg(long)]
        allow_http: bool,
        /// Anchor an explicit hex SHA-256 digest (e.g. an export envelope
        /// digest) instead of the chain head
        #[arg(long, value_name = "HEX")]
        digest: Option<String>,
    },
    /// Write a DER timestamp query for the chain head to a file (offline flow)
    Query {
        /// Output path for the .tsq request
        #[arg(long, value_name = "PATH")]
        out: String,
        /// Anchor an explicit hex SHA-256 digest instead of the chain head
        #[arg(long, value_name = "HEX")]
        digest: Option<String>,
    },
    /// Import and store a TSA response obtained out-of-band
    Import {
        /// Path to the DER .tsr response
        #[arg(long, value_name = "PATH")]
        response: String,
        /// TSA URL to record alongside the anchor (provenance only)
        #[arg(long, default_value = "(offline)")]
        url: String,
    },
    /// List stored anchors
    List,
    /// Verify stored anchors against chain history (and, with --ca, run the
    /// full openssl countersignature check)
    Verify {
        /// TSA CA certificate (PEM); enables `openssl ts -verify`
        #[arg(long, value_name = "PATH")]
        ca: Option<String>,
    },
}

fn main() -> Result<()> {
    let args = Args::parse();
    let conn =
        Connection::open(&args.db).with_context(|| format!("opening witness DB '{}'", args.db))?;
    tsa::ensure_anchor_table(&conn)?;

    match args.command {
        Command::Request {
            url,
            allow_http,
            digest,
        } => request(&conn, &url, allow_http, digest.as_deref()),
        Command::Query { out, digest } => query(&conn, &out, digest.as_deref()),
        Command::Import { response, url } => import(&conn, &response, &url),
        Command::List => list(&conn),
        Command::Verify { ca } => verify(&conn, ca.as_deref()),
    }
}

/// The digest to anchor: an operator-supplied artifact digest, or the chain head.
fn subject(conn: &Connection, digest: Option<&str>) -> Result<(&'static str, [u8; 32])> {
    match digest {
        Some(hex_digest) => {
            let bytes = hex::decode(hex_digest).context("--digest is not valid hex")?;
            let hash: [u8; 32] = bytes
                .try_into()
                .map_err(|_| anyhow!("--digest must be a 32-byte SHA-256 hex digest"))?;
            Ok(("digest", hash))
        }
        None => Ok(("chain_head", tsa::chain_head(conn)?)),
    }
}

#[cfg(feature = "tsa")]
fn request(conn: &Connection, url: &str, allow_http: bool, digest: Option<&str>) -> Result<()> {
    let (kind, hash) = subject(conn, digest)?;
    let nonce = tsa::random_nonce();
    let query = tsa::build_request(&hash, Some(&nonce), true);
    eprintln!("anchoring {} {} via {}", kind, hex::encode(hash), url);
    let response = tsa::fetch_timestamp(url, &query, allow_http)?;
    let token = tsa::parse_response(&response)?;
    tsa::verify_match(&token, &hash, Some(&nonce))?;
    let id = tsa::insert_anchor(conn, kind, &hash, url, &token)?;
    println!(
        "anchor #{id} stored: {} {}\n  genTime {}  serial 0x{}  policy {}",
        kind,
        hex::encode(hash),
        token.gen_time,
        token.serial_hex,
        token.policy_oid
    );
    Ok(())
}

#[cfg(not(feature = "tsa"))]
fn request(_: &Connection, _: &str, _: bool, _: Option<&str>) -> Result<()> {
    bail!(
        "this build has no online TSA client (rebuild with `--features tsa`), \
         or use the offline flow: `log_anchor query` then `log_anchor import`"
    )
}

fn query(conn: &Connection, out: &str, digest: Option<&str>) -> Result<()> {
    let (kind, hash) = subject(conn, digest)?;
    // No nonce in the offline flow: the query may be submitted much later,
    // and import correlates by message imprint against chain history instead.
    let der = tsa::build_request(&hash, None, true);
    std::fs::write(out, &der).with_context(|| format!("writing {out}"))?;
    println!(
        "query for {} {} written to {}\nsubmit it, e.g.:\n  curl -s -H 'Content-Type: application/timestamp-query' \\\n       --data-binary @{} https://freetsa.org/tsr > {}.tsr\nthen: log_anchor import --response {}.tsr",
        kind,
        hex::encode(hash),
        out,
        out,
        out.trim_end_matches(".tsq"),
        out.trim_end_matches(".tsq"),
    );
    Ok(())
}

fn import(conn: &Connection, response_path: &str, url: &str) -> Result<()> {
    let der = std::fs::read(response_path).with_context(|| format!("reading {response_path}"))?;
    let token = tsa::parse_response(&der)?;
    let hash: [u8; 32] = token
        .imprint
        .clone()
        .try_into()
        .map_err(|_| anyhow!("token imprint is not a 32-byte SHA-256 digest"))?;
    // The chain may have advanced since the query was written; the imprint
    // must match *some* recorded chain state, not necessarily the current head.
    let kind = if tsa::hash_in_history(conn, &hash)? {
        "chain_head"
    } else {
        eprintln!(
            "note: imprint {} is not in this DB's chain history; storing as a \
             generic digest anchor (use this for export-envelope digests)",
            hex::encode(hash)
        );
        "digest"
    };
    let id = tsa::insert_anchor(conn, kind, &hash, url, &token)?;
    println!(
        "anchor #{id} stored: {} {}\n  genTime {}  serial 0x{}  policy {}",
        kind,
        hex::encode(hash),
        token.gen_time,
        token.serial_hex,
        token.policy_oid
    );
    Ok(())
}

fn list(conn: &Connection) -> Result<()> {
    let anchors = tsa::list_anchors(conn)?;
    if anchors.is_empty() {
        println!("no anchors stored");
        return Ok(());
    }
    for a in anchors {
        println!(
            "#{}  {}  {}  genTime {}  via {}",
            a.id,
            a.subject,
            hex::encode(a.subject_hash),
            a.gen_time,
            a.tsa_url
        );
    }
    Ok(())
}

fn verify(conn: &Connection, ca: Option<&str>) -> Result<()> {
    let anchors = tsa::list_anchors(conn)?;
    if anchors.is_empty() {
        println!("no anchors stored");
        return Ok(());
    }
    let mut failures = 0usize;
    let mut unverified = 0usize;
    for a in &anchors {
        let mut problems: Vec<String> = Vec::new();

        // Structural: stored subject hash must be what the token covers …
        match tsa::parse_token_imprint(&a.token_der) {
            Ok(imprint) if imprint == a.subject_hash => {}
            Ok(imprint) => problems.push(format!(
                "token covers {} but anchor row claims {}",
                hex::encode(imprint),
                hex::encode(a.subject_hash)
            )),
            Err(e) => problems.push(format!("token unparseable: {e}")),
        }
        // … and chain-head anchors must reference recorded chain history.
        if a.subject == "chain_head" && !tsa::hash_in_history(conn, &a.subject_hash)? {
            problems.push("anchored hash is not in chain history".to_string());
        }

        // Cryptographic: delegate the CMS countersignature to openssl.
        if let Some(ca) = ca {
            match openssl_ts_verify(&a.token_der, &a.subject_hash, ca) {
                Ok(()) => {}
                Err(e) => problems.push(format!("openssl ts -verify failed: {e}")),
            }
        }

        if problems.is_empty() {
            if ca.is_some() {
                println!(
                    "anchor #{}: OK (imprint matches, in chain history, countersignature OK) \
                     genTime {}",
                    a.id, a.gen_time
                );
            } else {
                // Structural checks passed, but the CMS countersignature — the
                // step that makes the timestamp an *authority* rather than an
                // attacker-chosen string — was NOT checked. Never print "OK"
                // here: it reads as a verified anchor. genTime is untrusted
                // until --ca cryptographically verifies the token.
                unverified += 1;
                println!(
                    "anchor #{}: UNVERIFIED (imprint matches, in chain history; countersignature \
                     NOT checked — re-run with --ca) genTime {} (untrusted)",
                    a.id, a.gen_time
                );
            }
        } else {
            failures += 1;
            println!("anchor #{}: FAIL", a.id);
            for p in problems {
                println!("    {p}");
            }
        }
    }
    if ca.is_none() {
        println!(
            "\n{unverified} anchor(s) structurally consistent but CRYPTOGRAPHICALLY UNVERIFIED. \
             Their genTime is attacker-controllable until the countersignature is checked; do not \
             rely on them as trusted timestamps. For the full countersignature check pass\n  \
             --ca <tsa-ca.pem>   (runs: openssl ts -verify -digest <hash> -in <token> -CAfile …)"
        );
    }
    if failures > 0 {
        bail!("{failures} anchor(s) failed verification");
    }
    Ok(())
}

/// Run `openssl ts -verify` on a stored token — an independent second
/// implementation for the trust-critical signature check.
fn openssl_ts_verify(token_der: &[u8], digest: &[u8; 32], ca_path: &str) -> Result<()> {
    // NamedTempFile: unpredictable name, 0600, O_EXCL creation (no symlink
    // following), removed on drop even when openssl fails.
    let mut token_file = tempfile::Builder::new()
        .prefix("securacv_anchor_")
        .suffix(".der")
        .tempfile()?;
    std::io::Write::write_all(&mut token_file, token_der)?;
    let output = std::process::Command::new("openssl")
        .args([
            "ts",
            "-verify",
            "-digest",
            &hex::encode(digest),
            "-in",
            token_file
                .path()
                .to_str()
                .ok_or_else(|| anyhow!("temp path not UTF-8"))?,
            "-token_in",
            "-CAfile",
            ca_path,
        ])
        .output()
        .context("running openssl (is it installed?)")?;
    if !output.status.success() {
        bail!("{}", String::from_utf8_lossy(&output.stderr).trim());
    }
    Ok(())
}
