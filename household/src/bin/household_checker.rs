//! `household_checker`: the hub-side household-key checker.
//!
//! ```text
//! household_checker init --data-dir D --kek K
//! household_checker admin-keygen --out FILE
//! household_checker roster genesis --data-dir D --kek K --admin-key FILE [--admin-key FILE…] [--threshold N] --physical-confirm
//! household_checker roster show|pending --data-dir D
//! household_checker roster propose add-member --data-dir D --kind phone|watch|fob|fido2|card --class intent|possession
//!                                             [--public-key HEX] [--rp-id RP --credential-id HEX] [--not-after UNIX]
//! household_checker roster propose revoke --data-dir D --handle H
//! household_checker roster propose add-admin --data-dir D --handle H --public-key HEX [--alg ed25519 --kind cli]
//! household_checker roster propose remove-admin --data-dir D --handle H
//! household_checker roster propose set-threshold --data-dir D --threshold N
//! household_checker roster approve --data-dir D --proposal DIGEST (--admin-key FILE | --approval-file F | --holder-proof-file F)
//! household_checker roster apply --data-dir D --kek K --proposal DIGEST [--physical-confirm DIGEST8]
//! household_checker card provision --data-dir D --kek K --handle H [--key-file F] [--replace]
//! household_checker card prune --data-dir D --kek K
//! household_checker pair --data-dir D [--reset]
//! household_checker serve --data-dir D --kek K [--bind ADDR] [--allow-lan-plaintext] [--home-ttl SECS]
//! household_checker client keygen --out FILE
//! household_checker client command --key FILE --handle H --token-file F --purpose away|home|night
//!                                  [--addr ADDR] [--fido2-rp-id RP [--uv]]
//! household_checker client posture --token-file F [--addr ADDR]
//! ```
//!
//! The KEK opens the sealed hub keys and card keys, so the commands that sign
//! or touch those take `--kek`: `init`, `roster genesis`, `roster apply`,
//! `card provision`, `card prune` and `serve`. The rest open no sealed material (some read
//! or write the caller's own key file: an admin key, a client key, a bearer
//! token).
//!
//! Every command accepts only its own options ([`COMMANDS`]): an unknown or
//! misspelled one, or one that another command or `roster propose` kind
//! takes, is refused, never ignored. No error message repeats an option's
//! value, and an option's name only when some command takes it
//! ([`known_option`]).
//!
//! Nothing this program logs names a handle, a credential id or a tap.

use household_keys::admin::{self, Approval};
use household_keys::change::{self, Mode};
use household_keys::checker::{Checker, DirPersist};
use household_keys::client::{self, Binding, SoftCredential};
use household_keys::roster::{
    AdminAlg, AdminEntry, AdminKind, CredentialClass, MemberAlg, MemberEntry, MemberKind, Roster,
};
use household_keys::secrets::{self, CardStore, HubKeys, Kek};
use household_keys::server::{self, DirReload, Server, ServerConfig, SystemClock};
use household_keys::store::{self, DataDir, Pending};
use household_keys::{hex, posture, rng, wire, Error, Result};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;
use std::sync::Arc;
use zeroize::Zeroizing;

const DEFAULT_ADDR: &str = "127.0.0.1:8787";

/// Each command's words and the options it accepts, and nothing else. The
/// `roster propose` kinds take different options, so each kind is its own
/// row: `--threshold` on `add-member` is refused like `--key-fle`, not
/// ignored.
const COMMANDS: &[(&[&str], &[&str])] = &[
    (&["init"], &["data-dir", "kek"]),
    (&["admin-keygen"], &["out"]),
    (
        &["roster", "genesis"],
        &[
            "data-dir",
            "kek",
            "admin-key",
            "threshold",
            "physical-confirm",
        ],
    ),
    (&["roster", "show"], &["data-dir"]),
    (&["roster", "pending"], &["data-dir"]),
    (
        &["roster", "propose", "add-member"],
        &[
            "data-dir",
            "kind",
            "class",
            "public-key",
            "rp-id",
            "credential-id",
            "not-after",
        ],
    ),
    (&["roster", "propose", "revoke"], &["data-dir", "handle"]),
    (
        &["roster", "propose", "add-admin"],
        &[
            "data-dir",
            "handle",
            "public-key",
            "alg",
            "kind",
            "rp-id",
            "credential-id",
        ],
    ),
    (
        &["roster", "propose", "remove-admin"],
        &["data-dir", "handle"],
    ),
    (
        &["roster", "propose", "set-threshold"],
        &["data-dir", "threshold"],
    ),
    (
        &["roster", "approve"],
        &[
            "data-dir",
            "proposal",
            "admin-key",
            "approval-file",
            "holder-proof-file",
        ],
    ),
    (
        &["roster", "apply"],
        &["data-dir", "kek", "proposal", "physical-confirm"],
    ),
    (
        &["card", "provision"],
        &["data-dir", "kek", "handle", "key-file", "replace"],
    ),
    (&["card", "prune"], &["data-dir", "kek"]),
    (&["pair"], &["data-dir", "reset"]),
    (
        &["serve"],
        &["data-dir", "kek", "bind", "allow-lan-plaintext", "home-ttl"],
    ),
    (&["client", "keygen"], &["out"]),
    (
        &["client", "command"],
        &[
            "key",
            "handle",
            "token-file",
            "purpose",
            "addr",
            "fido2-rp-id",
            "uv",
        ],
    ),
    (&["client", "posture"], &["token-file", "addr"]),
];

const PROPOSAL_KINDS: &str = "add-member|revoke|add-admin|remove-admin|set-threshold";

/// The options the command named by `words` accepts; `None` for words that
/// name no command.
fn allowed_options(words: &[&str]) -> Option<&'static [&'static str]> {
    COMMANDS
        .iter()
        .find(|(w, _)| *w == words)
        .map(|(_, opts)| *opts)
}

/// Whether some command takes `--name` (or it is `key-hex`, which an earlier
/// draft took and whose refusal carries a hint). These are the only option
/// names an error message may repeat: anything else may be a key, a token
/// or a handle typed with a leading `--`, and is named by its position.
fn known_option(name: &str) -> bool {
    name == "key-hex" || COMMANDS.iter().any(|(_, opts)| opts.contains(&name))
}

struct Args {
    flags: Vec<(String, Option<String>)>,
}

impl Args {
    /// Parse `raw` against the command's own options: an option outside
    /// `allowed` is refused here, before any command runs.
    fn parse(raw: &[String], allowed: &[&str]) -> std::result::Result<Args, String> {
        let mut flags = Vec::new();
        let mut i = 0;
        while i < raw.len() {
            let a = &raw[i];
            // Never echo an argument: it may be a key typed in the wrong place.
            let Some(rest) = a.strip_prefix("--") else {
                return Err(format!(
                    "argument {} is not an option (options start with --)",
                    i + 1
                ));
            };
            let (name, with_eq) = match rest.split_once('=') {
                Some((n, _)) => (n, true),
                None => (rest, false),
            };
            if !allowed.contains(&name) {
                if !known_option(name) {
                    return Err(format!(
                        "argument {} is not an option of this command",
                        i + 1
                    ));
                }
                let hint = if name == "key-hex" {
                    " (a key on the command line lands in shell history: put it in a file and pass --key-file)"
                } else {
                    ""
                };
                return Err(format!("--{name} is not an option of this command{hint}"));
            }
            if with_eq {
                return Err(format!(
                    "--{name}: give an option's value as the next argument, not after '='"
                ));
            }
            let takes_value = !matches!(name, "allow-lan-plaintext" | "uv" | "reset" | "replace")
                && !(name == "physical-confirm"
                    && raw.get(i + 1).is_none_or(|n| n.starts_with("--")));
            if takes_value {
                // A value never starts with `--`: an option in that place is a
                // missing value, and the option that follows is parsed on its
                // own merits next time around, never swallowed as a value.
                let v = raw
                    .get(i + 1)
                    .filter(|n| !n.starts_with("--"))
                    .ok_or(format!("--{name} needs a value"))?;
                flags.push((name.to_string(), Some(v.clone())));
                i += 2;
            } else {
                flags.push((name.to_string(), None));
                i += 1;
            }
        }
        Ok(Args { flags })
    }

    fn get(&self, n: &str) -> Option<&str> {
        self.flags
            .iter()
            .find(|(k, _)| k == n)
            .and_then(|(_, v)| v.as_deref())
    }

    fn all(&self, n: &str) -> Vec<&str> {
        self.flags
            .iter()
            .filter(|(k, _)| k == n)
            .filter_map(|(_, v)| v.as_deref())
            .collect()
    }

    fn has(&self, n: &str) -> bool {
        self.flags.iter().any(|(k, _)| k == n)
    }

    fn req(&self, n: &str) -> Result<&str> {
        self.get(n)
            .ok_or(Error::Invalid("a required option is missing (see --help)"))
    }

    fn path(&self, n: &str) -> Result<PathBuf> {
        Ok(PathBuf::from(self.req(n)?))
    }
}

fn now_unix() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn usage() -> &'static str {
    "usage: household_checker <init|admin-keygen|roster|card|pair|serve|client> … (see the README)"
}

fn main() {
    let raw: Vec<String> = std::env::args().skip(1).collect();
    if raw.is_empty() || raw.iter().any(|a| a == "--help" || a == "-h") {
        println!("{}", usage());
        std::process::exit(if raw.is_empty() { 2 } else { 0 });
    }
    let (words, rest): (Vec<&String>, Vec<&String>) = {
        let split = raw
            .iter()
            .position(|a| a.starts_with("--"))
            .unwrap_or(raw.len());
        (raw[..split].iter().collect(), raw[split..].iter().collect())
    };
    let rest: Vec<String> = rest.into_iter().cloned().collect();
    let words: Vec<&str> = words.iter().map(|s| s.as_str()).collect();
    // The command's own options are known before any option is read, so a
    // stray one is refused by the parser and nothing runs. (The words are
    // never echoed either: one may be a key typed in the wrong place.)
    let Some(allowed) = allowed_options(&words) else {
        match words.as_slice() {
            ["roster", "propose", ..] => eprintln!(
                "household_checker: roster propose takes one of {PROPOSAL_KINDS} (see the README)"
            ),
            _ => eprintln!("household_checker: {}", usage()),
        }
        std::process::exit(2);
    };
    let args = match Args::parse(&rest, allowed) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("household_checker: {e}");
            std::process::exit(2);
        }
    };
    let r = match words.as_slice() {
        ["init"] => cmd_init(&args),
        ["admin-keygen"] => cmd_admin_keygen(&args),
        ["roster", "genesis"] => cmd_genesis(&args),
        ["roster", "show"] => cmd_show(&args),
        ["roster", "pending"] => cmd_pending(&args),
        ["roster", "propose", what] => cmd_propose(what, &args),
        ["roster", "approve"] => cmd_approve(&args),
        ["roster", "apply"] => cmd_apply(&args),
        ["card", "provision"] => cmd_card(&args),
        ["card", "prune"] => cmd_card_prune(&args),
        ["pair"] => cmd_pair(&args),
        ["serve"] => cmd_serve(&args),
        ["client", "keygen"] => cmd_client_keygen(&args),
        ["client", "command"] => cmd_client_command(&args),
        ["client", "posture"] => cmd_client_posture(&args),
        _ => {
            eprintln!("household_checker: {}", usage());
            std::process::exit(2);
        }
    };
    match r {
        Ok(code) => std::process::exit(code),
        Err(e) => {
            eprintln!("household_checker: error: {e}");
            std::process::exit(1);
        }
    }
}

fn cmd_init(a: &Args) -> Result<i32> {
    let dir_path = a.path("data-dir")?;
    let kek_path = a.path("kek")?;
    // The directory first, so the KEK path can be checked against it; the
    // household goes in only once the KEK is in hand to seal its keys.
    store::mkdir_private(&dir_path)?;
    if dir_path.join("household.json").exists() {
        return Err(Error::Invalid(
            "this data directory already holds a household",
        ));
    }
    if secrets::is_inside(&kek_path, &dir_path)? {
        return Err(Error::Invalid(
            "the KEK must not live inside the data directory",
        ));
    }
    if !kek_path.exists() {
        let k = Kek::generate()?;
        store::write_atomic(&kek_path, k.to_hex().as_bytes())?;
        eprintln!("household_checker: wrote a new KEK file (keep it outside backups of the data directory)");
    }
    let kek = secrets::load_kek(&kek_path, &dir_path)?;
    let d = DataDir::create(&dir_path, &kek)?;
    println!(
        "{}",
        serde_json::to_string_pretty(&json!({
            "household": hex::encode(&d.household()?),
            "posture_public_key": hex::encode(&d.posture_public_key()?.to_bytes()),
            "roster_public_key": hex::encode(&d.roster_public_key()?.to_bytes()),
        }))
        .unwrap_or_default()
    );
    Ok(0)
}

fn cmd_admin_keygen(a: &Args) -> Result<i32> {
    let out = a.path("out")?;
    if out.exists() {
        return Err(Error::Invalid("refusing to overwrite an existing key file"));
    }
    let secret = Zeroizing::new(rng::array::<32>()?);
    let sk = ed25519_dalek::SigningKey::from_bytes(&secret);
    let handle: [u8; 16] = rng::array()?;
    let secret_hex = hex::encode_secret(&secret[..]);
    let body = Zeroizing::new(format!(
        "{{\"alg\":\"ed25519\",\"kind\":\"cli\",\"handle\":\"{}\",\"secret_key\":\"{}\"}}\n",
        hex::encode(&handle),
        secret_hex.as_str()
    ));
    store::write_atomic(&out, body.as_bytes())?;
    println!(
        "{}",
        json!({"handle": hex::encode(&handle), "alg": "ed25519", "kind": "cli",
               "public_key": hex::encode(&sk.verifying_key().to_bytes())})
    );
    Ok(0)
}

struct AdminKey {
    handle: [u8; 16],
    key: ed25519_dalek::SigningKey,
}

/// The file `admin-keygen` writes. Its secret lands in a [`Zeroizing`]
/// buffer as it is parsed, never in a `serde_json::Value`.
#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct AdminKeyFile {
    alg: String,
    kind: String,
    handle: String,
    #[serde(deserialize_with = "secrets::deserialize_zeroizing_string")]
    secret_key: Zeroizing<String>,
}

fn load_admin_key(p: &Path) -> Result<AdminKey> {
    let raw = store::read_bounded_secret(p, 4096)?;
    let f: AdminKeyFile = serde_json::from_slice(&raw)
        .map_err(|_| Error::Invalid("admin key file does not match the schema"))?;
    if f.alg != "ed25519" || f.kind != "cli" {
        return Err(Error::Invalid(
            "only ed25519/cli admin key files are supported by this CLI",
        ));
    }
    let handle = hex::decode_array(&f.handle)?;
    let s = hex::decode_secret_array::<32>(&f.secret_key)?;
    Ok(AdminKey {
        handle,
        key: ed25519_dalek::SigningKey::from_bytes(&s),
    })
}

fn admin_entry(k: &AdminKey) -> AdminEntry {
    AdminEntry {
        handle: k.handle,
        alg: AdminAlg::Ed25519,
        kind: AdminKind::Cli,
        public_key: k.key.verifying_key().to_bytes().to_vec(),
        rp_id: None,
        credential_id: None,
    }
}

fn open_dir(a: &Args) -> Result<DataDir> {
    DataDir::open(&a.path("data-dir")?)
}

/// Load the KEK named by `--kek` and open the hub keys with it (which also
/// proves the KEK belongs to this household).
fn open_keys(a: &Args, d: &DataDir) -> Result<(Kek, HubKeys)> {
    let kek = secrets::load_kek(&a.path("kek")?, d.root())?;
    let keys = d.hub_keys(&kek)?;
    Ok((kek, keys))
}

fn current(d: &DataDir) -> Result<Roster> {
    d.current_roster()?
        .ok_or(Error::Invalid("no roster yet: run `roster genesis` first"))
}

fn cmd_genesis(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    let (_, hub) = open_keys(a, &d)?;
    if d.current_roster()?.is_some() {
        return Err(Error::Invalid("this household already has a roster"));
    }
    let keys: Vec<AdminKey> = a
        .all("admin-key")
        .into_iter()
        .map(|p| load_admin_key(Path::new(p)))
        .collect::<Result<_>>()?;
    if keys.is_empty() {
        return Err(Error::Invalid("genesis needs at least one --admin-key"));
    }
    let threshold: u64 = a
        .get("threshold")
        .unwrap_or("1")
        .parse()
        .map_err(|_| Error::Invalid("bad --threshold"))?;
    let g = Roster::genesis(
        d.household()?,
        keys.iter().map(admin_entry).collect(),
        threshold,
    )?;
    let digest = g.digest()?;
    let approvals: Vec<Approval> = keys
        .iter()
        .map(|k| admin::sign_cli(&k.key, &k.handle, &digest))
        .collect();
    let mode = Mode::Live {
        now_unix: now_unix(),
        proposed_at: now_unix(),
        physical_confirm: a.has("physical-confirm"),
    };
    change::check_genesis(&g, &approvals, mode)?;
    let s = d.accept(&hub, g, approvals, None)?;
    println!(
        "{}",
        json!({"epoch": s.roster.epoch, "digest": hex::encode(&digest)})
    );
    Ok(0)
}

fn cmd_show(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    let r = current(&d)?;
    println!(
        "{}",
        serde_json::to_string_pretty(
            &json!({"digest": hex::encode(&r.digest()?), "roster": r.to_value()})
        )
        .unwrap_or_default()
    );
    Ok(0)
}

fn cmd_pending(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    let r = current(&d)?;
    let mut out = Vec::new();
    for dg in d.list_pending()? {
        let p = d.load_pending(&dg)?;
        let kind = match change::classify(&r, &p.roster) {
            Ok(change::ChangeKind::AddMembers) => "add-members",
            Ok(change::ChangeKind::RevokeMembers) => "revoke-members",
            Ok(change::ChangeKind::AdminSet) => "admin-set",
            Err(_) => "stale",
        };
        let n = admin::count_approvals(&r.admins, &p.roster.digest()?, &p.approvals);
        out.push(json!({
            "proposal": dg, "kind": kind, "approvals": n, "threshold": r.threshold,
            "admin_set_ready_at": if kind == "admin-set" { json!(p.proposed_at + change::ADMIN_CHANGE_PENDING_SECS) } else { Value::Null },
        }));
    }
    println!(
        "{}",
        serde_json::to_string_pretty(&Value::Array(out)).unwrap_or_default()
    );
    Ok(0)
}

fn propose(d: &DataDir, next: Roster) -> Result<String> {
    next.validate()?;
    d.save_pending(&Pending {
        roster: next,
        approvals: Vec::new(),
        holder_proof: None,
        proposed_at: now_unix(),
    })
}

fn cmd_propose(what: &str, a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    let r = current(&d)?;
    let mut next = r.successor()?;
    let mut new_handle = None;
    match what {
        "add-member" => {
            let kind = MemberKind::parse(a.req("kind")?)?;
            let class = CredentialClass::parse(a.req("class")?)?;
            let alg = match kind {
                MemberKind::Phone | MemberKind::Watch | MemberKind::Fob => MemberAlg::Es256,
                MemberKind::Fido2 => MemberAlg::Fido2Es256,
                MemberKind::Card => MemberAlg::Aes128Ev2,
            };
            let handle: [u8; 16] = rng::array()?;
            next.members.push(MemberEntry {
                handle,
                kind,
                class,
                alg,
                public_key: a.get("public-key").map(hex::decode).transpose()?,
                rp_id: a.get("rp-id").map(str::to_owned),
                credential_id: a.get("credential-id").map(hex::decode).transpose()?,
                not_after: a
                    .get("not-after")
                    .map(|s| {
                        s.parse::<u64>()
                            .map_err(|_| Error::Invalid("bad --not-after"))
                    })
                    .transpose()?,
            });
            new_handle = Some(handle);
        }
        "revoke" => {
            let h: [u8; 16] = hex::decode_array(a.req("handle")?)?;
            let before = next.members.len();
            next.members.retain(|m| m.handle != h);
            if next.members.len() == before {
                return Err(Error::Invalid("no such member"));
            }
        }
        "add-admin" => {
            let alg = AdminAlg::parse(a.get("alg").unwrap_or("ed25519"))?;
            let kind = AdminKind::parse(a.get("kind").unwrap_or("cli"))?;
            next.admins.push(AdminEntry {
                handle: hex::decode_array(a.req("handle")?)?,
                alg,
                kind,
                public_key: hex::decode(a.req("public-key")?)?,
                rp_id: a.get("rp-id").map(str::to_owned),
                credential_id: a.get("credential-id").map(hex::decode).transpose()?,
            });
        }
        "remove-admin" => {
            let h: [u8; 16] = hex::decode_array(a.req("handle")?)?;
            let before = next.admins.len();
            next.admins.retain(|x| x.handle != h);
            if next.admins.len() == before {
                return Err(Error::Invalid("no such admin"));
            }
            if next.threshold > next.admins.len() as u64 {
                next.threshold = next.admins.len() as u64;
            }
        }
        "set-threshold" => {
            next.threshold = a
                .req("threshold")?
                .parse()
                .map_err(|_| Error::Invalid("bad --threshold"))?;
        }
        _ => return Err(Error::Invalid("unknown proposal kind")),
    }
    next.sort();
    let kind = change::classify(&r, &next)?;
    let dg = propose(&d, next)?;
    let mut out = json!({"proposal": dg, "kind": format!("{kind:?}")});
    if let Some(h) = new_handle {
        out["new_member_handle"] = json!(hex::encode(&h));
    }
    println!("{out}");
    Ok(0)
}

fn read_approval(p: &Path) -> Result<Approval> {
    let raw = store::read_bounded(p, 8192)?;
    serde_json::from_slice(&raw)
        .map_err(|_| Error::Invalid("approval file does not match the schema"))
}

fn cmd_approve(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    let dg = a.req("proposal")?;
    let mut p = d.load_pending(dg)?;
    let digest = p.roster.digest()?;
    if let Some(f) = a.get("holder-proof-file") {
        // A member's own revocation proof (es256 / FIDO2), made elsewhere.
        p.holder_proof = Some(read_approval(Path::new(f))?);
    } else {
        // An ed25519 CLI key file, or an approval made elsewhere (an ES256
        // enclave key, a FIDO2 assertion) as {"handle","sig","authenticator_data"}.
        let ap = match a.get("approval-file") {
            Some(f) => read_approval(Path::new(f))?,
            None => {
                let k = load_admin_key(&a.path("admin-key")?)?;
                admin::sign_cli(&k.key, &k.handle, &digest)
            }
        };
        p.approvals.retain(|x| x.handle != ap.handle);
        p.approvals.push(ap);
    }
    d.save_pending(&p)?;
    let r = current(&d)?;
    let n = admin::count_approvals(&r.admins, &digest, &p.approvals);
    println!(
        "{}",
        json!({"proposal": dg, "approvals": n, "threshold": r.threshold})
    );
    Ok(0)
}

fn cmd_apply(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    let (kek, hub) = open_keys(a, &d)?;
    let dg = a.req("proposal")?.to_string();
    let p = d.load_pending(&dg)?;
    let r = current(&d)?;
    let confirm = a
        .get("physical-confirm")
        .is_some_and(|c| c.len() == 8 && dg.starts_with(c));
    let mode = Mode::Live {
        now_unix: now_unix(),
        proposed_at: p.proposed_at,
        physical_confirm: confirm,
    };
    let kind = change::check_change(&r, &p.roster, &p.approvals, p.holder_proof.as_ref(), mode)?;
    let s = d.accept(&hub, p.roster, p.approvals, p.holder_proof)?;
    d.remove_pending(&dg)?;
    // Proposals built on the old epoch can never apply now.
    for other in d.list_pending()? {
        if let Ok(o) = d.load_pending(&other) {
            if o.roster.epoch <= s.roster.epoch {
                let _ = d.remove_pending(&other);
            }
        }
    }
    if kind == change::ChangeKind::RevokeMembers {
        // Tokens are deliberately not linked to members, so the hub cannot
        // tell which pairing (if any) belonged to the revoked credential.
        eprintln!(
            "household_checker: LAN pairings are separate from the roster. If a revoked \
             credential's device was paired, run `household_checker pair --reset` and pair \
             the clients you keep again: an old token still reads the posture."
        );
    }
    let epoch = s.roster.epoch;
    match prune_cards(&d, &kek, &s.roster) {
        Ok(removed) => {
            println!("{}", json!({"epoch": epoch, "card_keys_removed": removed}));
            Ok(0)
        }
        Err(e) => {
            println!("{}", json!({"epoch": epoch}));
            eprintln!(
                "household_checker: epoch {epoch} is applied, but removing card keys no member \
                 or proposal names failed: {e}; fix the cause and run `household_checker card \
                 prune` to retry"
            );
            Ok(1)
        }
    }
}

fn load_cards(d: &DataDir, kek: &Kek) -> Result<CardStore> {
    let p = d.cards_path();
    if !p.exists() {
        return Ok(CardStore::new());
    }
    let b = store::read_bounded(&p, secrets::MAX_SEALED_BYTES)?;
    CardStore::open(&b, kek, &d.household()?)
}

/// Drop every sealed card key whose handle is neither a card member of
/// `roster` nor a card member of a pending proposal, so the hub does not keep
/// a revoked card's key (the only thing that can recognize or clone it).
fn prune_cards(d: &DataDir, kek: &Kek, roster: &Roster) -> Result<usize> {
    if !d.cards_path().exists() {
        return Ok(0);
    }
    let mut keep: std::collections::BTreeSet<[u8; 16]> = std::collections::BTreeSet::new();
    let mut add = |r: &Roster| {
        for m in r.members.iter().filter(|m| m.kind == MemberKind::Card) {
            keep.insert(m.handle);
        }
    };
    add(roster);
    for dg in d.list_pending()? {
        match d.load_pending(&dg) {
            Ok(p) => add(&p.roster),
            // A proposal file that does not load cannot apply either, so it
            // keeps no card key; say which file, so it can be removed.
            Err(e) => eprintln!(
                "household_checker: skipped pending/{dg}.json while pruning card keys: {e}"
            ),
        }
    }
    let mut cards = load_cards(d, kek)?;
    let removed = cards.retain(|h| keep.contains(h));
    if removed > 0 {
        store::write_atomic(&d.cards_path(), &cards.seal(kek, &d.household()?)?)?;
    }
    Ok(removed)
}

fn cmd_card(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    // Opening the hub keys proves the KEK is this household's, so a card is
    // never sealed under a KEK the server cannot open.
    let (kek, _) = open_keys(a, &d)?;
    let h: [u8; 16] = hex::decode_array(a.req("handle")?)?;
    let is_card = |r: &Roster| r.member(&h).is_some_and(|m| m.kind == MemberKind::Card);
    let known = d.current_roster()?.is_some_and(|r| is_card(&r))
        || d.list_pending()?
            .iter()
            .any(|dg| d.load_pending(dg).is_ok_and(|p| is_card(&p.roster)));
    if !known {
        return Err(Error::Invalid(
            "that handle is not a card member of the roster or a pending proposal",
        ));
    }
    // A key handed in comes from a file (or a pipe), never the command line,
    // which would put it in the shell history and in /proc/<pid>/cmdline.
    let (key, generated) = match a.get("key-file") {
        Some(f) => {
            check_key_file(Path::new(f), d.root())?;
            let raw = store::read_bounded_secret(Path::new(f), 128)?;
            let text =
                std::str::from_utf8(&raw).map_err(|_| Error::Invalid("key file is not text"))?;
            (hex::decode_secret_array::<16>(text.trim())?, false)
        }
        None => (Zeroizing::new(rng::array::<16>()?), true),
    };
    let mut cards = load_cards(&d, &kek)?;
    if cards.contains(&h) && !a.has("replace") {
        return Err(Error::Invalid(
            "that card already has a key; pass --replace to re-key it",
        ));
    }
    cards.insert(h, *key);
    store::write_atomic(&d.cards_path(), &cards.seal(&kek, &d.household()?)?)?;
    if generated {
        // Shown once, so it can be written to the card with NFC tooling (not
        // part of v0). It never touches a log, and the line is built in a
        // buffer that is wiped, not in a `serde_json::Value`.
        let shown = hex::encode_secret(&key[..]);
        let line = Zeroizing::new(format!("{{\"card_key\":\"{}\"}}", shown.as_str()));
        println!("{}", line.as_str());
    }
    Ok(0)
}

/// Refuse a card-key file that group or others can read, or one inside the
/// data directory: it holds a clone-capable key, so it gets the same rule as
/// the KEK. A pipe or FIFO (`--key-file /dev/stdin`, `<(...)`) is accepted;
/// delete a key file once the card is provisioned.
fn check_key_file(path: &Path, data_dir: &Path) -> Result<()> {
    let meta = std::fs::metadata(path).map_err(Error::io("stat the key file"))?;
    if !meta.is_file() {
        return Ok(());
    }
    if secrets::is_inside(path, data_dir)? {
        return Err(Error::Invalid(
            "the key file must not live inside the data directory",
        ));
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if meta.permissions().mode() & 0o077 != 0 {
            return Err(Error::Invalid(
                "the key file must not be readable by group or others (chmod 600, or pipe it in)",
            ));
        }
    }
    Ok(())
}

fn cmd_card_prune(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    let (kek, _) = open_keys(a, &d)?;
    let removed = prune_cards(&d, &kek, &current(&d)?)?;
    println!("{}", json!({"card_keys_removed": removed}));
    Ok(0)
}

fn cmd_pair(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    if a.has("reset") {
        match d.reset_bearers()? {
            Some(n) => eprintln!("unpaired {n} client(s); pair each again to use the LAN API"),
            None => eprintln!(
                "the pairing file did not read; it is now empty, so every client is unpaired"
            ),
        }
        return Ok(0);
    }
    let t = d.add_bearer()?;
    println!("{}", t.as_str());
    Ok(0)
}

fn cmd_serve(a: &Args) -> Result<i32> {
    let d = open_dir(a)?;
    let (kek, hub) = open_keys(a, &d)?;
    let roster_key = hub.roster.verifying_key();
    let roster = current(&d)?;
    let cards = load_cards(&d, &kek)?;
    let ttl: u64 = a
        .get("home-ttl")
        .map(|s| s.parse().map_err(|_| Error::Invalid("bad --home-ttl")))
        .transpose()?
        .unwrap_or(posture::DEFAULT_HOME_TTL_SECS);
    let checker = Checker::new(
        hub.posture,
        roster,
        cards,
        d.last_seq()?,
        ttl,
        Box::new(DirPersist(d.clone())),
        now_unix(),
    )?;
    let bind = a
        .get("bind")
        .unwrap_or(DEFAULT_ADDR)
        .parse()
        .map_err(|_| Error::Invalid("bad --bind address"))?;
    let cfg = ServerConfig {
        bind,
        allow_lan: a.has("allow-lan-plaintext"),
    };
    let reload = Box::new(DirReload::new(d.clone(), kek, roster_key));
    let srv = Server::bind(
        &cfg,
        checker,
        d.bearer_hashes()?,
        Some(reload),
        Arc::new(SystemClock::default()),
        server::stderr_log(),
    )?;
    let stop = AtomicBool::new(false);
    srv.run(&stop);
    Ok(0)
}

fn cmd_client_keygen(a: &Args) -> Result<i32> {
    let pk = client::keygen(&a.path("out")?)?;
    println!("{}", json!({"public_key": hex::encode(&pk)}));
    Ok(0)
}

fn read_token(a: &Args) -> Result<Zeroizing<String>> {
    let raw = store::read_bounded_secret(&a.path("token-file")?, 1024)?;
    let s = std::str::from_utf8(&raw).map_err(|_| Error::Invalid("token file is not text"))?;
    Ok(Zeroizing::new(s.trim().to_string()))
}

fn addr(a: &Args) -> Result<std::net::SocketAddr> {
    a.get("addr")
        .unwrap_or(DEFAULT_ADDR)
        .parse()
        .map_err(|_| Error::Invalid("bad --addr"))
}

fn cmd_client_command(a: &Args) -> Result<i32> {
    let binding = match a.get("fido2-rp-id") {
        Some(rp) => Binding::Fido2 {
            rp_id: rp.to_string(),
            uv: a.has("uv"),
        },
        None => Binding::Es256,
    };
    let cred = SoftCredential::load(
        &a.path("key")?,
        hex::decode_array(a.req("handle")?)?,
        binding,
    )?;
    let purpose = wire::Purpose::from_word(a.req("purpose")?)?;
    let (st, body) = client::set_posture(addr(a)?, &read_token(a)?, &cred, purpose)?;
    println!("{body}");
    Ok(if st == 200 { 0 } else { 1 })
}

fn cmd_client_posture(a: &Args) -> Result<i32> {
    let (st, body) = client::http(addr(a)?, "GET", "/v1/posture", Some(&read_token(a)?), None)?;
    println!("{body}");
    Ok(if st == 200 { 0 } else { 1 })
}
