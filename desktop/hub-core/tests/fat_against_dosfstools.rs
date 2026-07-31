//! Prove the in-image FAT writer against implementations that are not ours.
//!
//! `hub_fat`'s unit tests read back what `hub_fat` wrote. That catches a lot,
//! but it cannot catch a shared misunderstanding: if the writer and the reader
//! agree on a wrong layout, every test passes and the card still doesn't boot.
//! The Wi-Fi seed is the whole product for a headless hub, and the filesystem it
//! lands in is one the operator's Pi — not us — has to read.
//!
//! So this suite hands the result to three programs that have never seen our
//! code:
//!
//! * **`mkfs.fat`** creates the volume, so we are writing into a real
//!   filesystem rather than one our own test helper made up.
//! * **`fsck.fat -n`** audits it afterwards. It reads *both* FAT copies, checks
//!   every cluster chain, and verifies directory structure — it is precisely the
//!   tool that catches a FAT mirror we forgot to update or a directory entry we
//!   placed badly, and it reports those as errors rather than silently coping.
//! * **`mtools`** reads the file back out with an independent FAT
//!   implementation, so the bytes are proven retrievable by someone else's
//!   parser, under the name we intended, with its case intact.
//!
//! Set `HUB_FAT_REAL_IMAGE` to a decompressed `haos_*.img` (or a prefix of one
//! that covers the boot partition) to run the same checks against the genuine
//! article, partition table and all.
//!
//! Every test **skips with a printed note** when the tools are missing, so this
//! file is never the reason a contributor's `cargo test` fails on a machine
//! without dosfstools. CI installs them, so in CI these do run — see
//! `.github/workflows/desktop-hub-core.yml`.

use hub_core::hub_fat::{find_fat_partition, insert_file, read_file, FatKind, FatVolume};
use std::path::{Path, PathBuf};
use std::process::Command;

const KEYFILE: &[u8] = b"[connection]\n\
id=securacv-hub\n\
uuid=6b1a9f6e-1f3f-4a0e-9d6c-0f7c2a5b8e11\n\
type=802-11-wireless\n\
llmnr=2\n\
mdns=2\n\
\n\
[802-11-wireless]\n\
mode=infrastructure\n\
ssid=77;121;32;78;101;116;\n\
\n\
[802-11-wireless-security]\n\
auth-alg=open\n\
key-mgmt=wpa-psk\n\
psk=supersecret\n\
\n\
[ipv4]\n\
method=auto\n\
\n\
[ipv6]\n\
addr-gen-mode=stable-privacy\n\
method=auto\n";

fn have(tool: &str) -> bool {
    Command::new("sh")
        .arg("-c")
        .arg(format!("command -v {tool}"))
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

/// Skip (loudly) rather than fail when the external tool isn't installed.
///
/// The marker is deliberately distinct from the optional-real-image skip below:
/// CI greps for `SKIP(tooling)` and fails the job, because a missing tool means
/// these tests proved nothing while still reporting green. Not having a
/// multi-gigabyte HAOS image on the runner is expected and is not that.
macro_rules! need {
    ($($tool:literal),+) => {
        $(if !have($tool) {
            println!("SKIP(tooling): {} is not installed", $tool);
            return;
        })+
    };
}

fn workdir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("hub-fat-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("a work directory");
    dir
}

fn run(cmd: &mut Command) -> (bool, String) {
    let out = cmd.output().expect("the tool runs");
    let text = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    (out.status.success(), text)
}

/// `fsck.fat -n` — read-only, so it audits without "fixing" anything into
/// looking correct.
fn fsck_clean(path: &Path) -> String {
    let (ok, text) = run(Command::new("fsck.fat").arg("-n").arg("-v").arg(path));
    assert!(ok, "fsck.fat rejected the filesystem:\n{text}");
    // A zero exit is necessary but not sufficient: fsck.fat reports some
    // findings (an uninitialised free-cluster summary, for one) without failing.
    // These are its actual complaint phrases — each one names damage we could
    // plausibly do, so matching them is the point. They must be specific enough
    // not to collide with `-v` progress lines like "Checking free cluster
    // summary.", which is why "free cluster" alone is not in the list.
    for bad in [
        "Dirty bit",
        "FATs differ",
        "differ from each other",
        "Cluster chain loop",
        "Lost cluster",
        "lost cluster",
        "orphan",
        "Free cluster summary wrong",
        "Free cluster summary uninitialized",
        "Bad file name",
        "Bad short file name",
        "has a bad start",
        "start does point to",
        "Truncating",
        "Reclaiming",
        "is not a directory",
        "Deleting",
    ] {
        assert!(
            !text.contains(bad),
            "fsck.fat complained about {bad:?}:\n{text}"
        );
    }
    text
}

/// Read a file back out of the volume with mtools' FAT implementation.
fn mtools_read(image: &Path, offset: u64, path: &str) -> Vec<u8> {
    let spec = if offset == 0 {
        image.display().to_string()
    } else {
        format!("{}@@{offset}", image.display())
    };
    let out = Command::new("mcopy")
        .env("MTOOLS_SKIP_CHECK", "1")
        .arg("-n")
        .arg("-i")
        .arg(&spec)
        .arg(format!("::{path}"))
        .arg("-")
        .output()
        .expect("mcopy runs");
    assert!(
        out.status.success(),
        "mcopy could not read {path}:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    out.stdout
}

fn mtools_list(image: &Path, offset: u64, path: &str) -> String {
    let spec = if offset == 0 {
        image.display().to_string()
    } else {
        format!("{}@@{offset}", image.display())
    };
    let (ok, text) = run(Command::new("mdir")
        .env("MTOOLS_SKIP_CHECK", "1")
        .arg("-i")
        .arg(&spec)
        .arg(format!("::{path}")));
    assert!(ok, "mdir could not list {path}:\n{text}");
    text
}

/// Format a volume with the real `mkfs.fat`, inject, and hand it to fsck+mtools.
fn round_trip_via_dosfstools(name: &str, mkfs_args: &[&str], blocks_1k: u32, expect: FatKind) {
    need!("mkfs.fat", "fsck.fat", "mcopy", "mdir");

    let dir = workdir(name);
    let img = dir.join("boot.img");
    // `-C <file> <blocks>`: create the image file at this size, in 1 KiB blocks.
    let (ok, text) = run(Command::new("mkfs.fat")
        .args(mkfs_args)
        .arg("-C")
        .arg(&img)
        .arg(blocks_1k.to_string()));
    assert!(ok, "mkfs.fat failed:\n{text}");

    let mut bytes = std::fs::read(&img).expect("the formatted volume");
    let len = bytes.len() as u64;
    let vol = FatVolume::open(&mut bytes, 0, len).expect("our parser reads mkfs.fat's output");
    assert_eq!(vol.kind, expect, "mkfs.fat made a different FAT width");

    insert_file(
        &mut bytes,
        &vol,
        &["CONFIG", "network", "securacv-hub"],
        KEYFILE,
    )
    .unwrap_or_else(|e| panic!("injection failed: {}", e.message()));
    // A second file in the same tree, to exercise a directory that already
    // exists rather than only the create-everything path.
    insert_file(
        &mut bytes,
        &vol,
        &["CONFIG", "network", "spare.nmconnection"],
        b"x=1",
    )
    .unwrap_or_else(|e| panic!("second injection failed: {}", e.message()));

    std::fs::write(&img, &bytes).expect("write the patched volume back");

    fsck_clean(&img);

    assert_eq!(
        mtools_read(&img, 0, "/CONFIG/network/securacv-hub"),
        KEYFILE,
        "mtools read back different bytes than we wrote"
    );
    assert_eq!(
        mtools_read(&img, 0, "/CONFIG/network/spare.nmconnection"),
        b"x=1"
    );

    // The lower-case directory name has to survive: HAOS looks for
    // `CONFIG/network`, and an uppercase `NETWORK` is a hub that ignores the
    // keyfile and sits on the landing page forever.
    let listing = mtools_list(&img, 0, "/CONFIG");
    assert!(
        listing.contains("network"),
        "the lower-case directory name did not survive:\n{listing}"
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn fat16_with_the_real_haos_geometry_survives_fsck_and_mtools() {
    // -F 16 -s 4 -R 4 -r 512, 64 MiB: exactly what haos_rpi5-64-18.1.img has.
    round_trip_via_dosfstools(
        "fat16",
        &[
            "-F",
            "16",
            "-s",
            "4",
            "-R",
            "4",
            "-r",
            "512",
            "-n",
            "hassos-boot",
        ],
        64 * 1024,
        FatKind::Fat16,
    );
}

#[test]
fn fat32_survives_fsck_and_mtools_too() {
    // Not what the Pi image uses today, but HA's other boards and any future
    // re-format could, and the FAT32 paths (cluster-chained root, FSInfo,
    // 32-bit entries) are otherwise only exercised by our own reader.
    round_trip_via_dosfstools(
        "fat32",
        &["-F", "32", "-s", "1", "-n", "hassos-boot"],
        64 * 1024,
        FatKind::Fat32,
    );
}

#[test]
fn a_directory_that_outgrows_one_cluster_still_passes_fsck() {
    // Growing a directory means allocating a cluster, chaining it in both FATs,
    // and letting an entry run straddle the boundary. Done wrong this is exactly
    // the corruption fsck was written to find.
    need!("mkfs.fat", "fsck.fat", "mcopy");

    let dir = workdir("grow");
    let img = dir.join("boot.img");
    let (ok, text) = run(Command::new("mkfs.fat")
        .args(["-F", "16", "-s", "4", "-R", "4", "-r", "512", "-C"])
        .arg(&img)
        .arg("65536"));
    let _ = &text;
    assert!(ok, "mkfs.fat failed:\n{text}");

    let mut bytes = std::fs::read(&img).unwrap();
    let len = bytes.len() as u64;
    let vol = FatVolume::open(&mut bytes, 0, len).unwrap();
    let per_cluster = vol.cluster_bytes() as usize / 32;

    let names: Vec<String> = (0..per_cluster + 30)
        .map(|i| format!("connection-number-{i:03}.nmconnection"))
        .collect();
    for (i, n) in names.iter().enumerate() {
        insert_file(
            &mut bytes,
            &vol,
            &["CONFIG", n],
            format!("id={i}").as_bytes(),
        )
        .unwrap_or_else(|e| panic!("inserting {n}: {}", e.message()));
    }
    std::fs::write(&img, &bytes).unwrap();

    fsck_clean(&img);
    for (i, n) in names.iter().enumerate() {
        assert_eq!(
            mtools_read(&img, 0, &format!("/CONFIG/{n}")),
            format!("id={i}").as_bytes(),
            "{n} did not survive the directory growing"
        );
    }
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_real_haos_image_takes_the_seed() {
    need!("fsck.fat", "mcopy", "mdir");
    let Ok(src) = std::env::var("HUB_FAT_REAL_IMAGE") else {
        println!("SKIP(optional): set HUB_FAT_REAL_IMAGE to a decompressed haos_*.img to run this");
        return;
    };

    let dir = workdir("real");
    let img = dir.join("haos.img");
    std::fs::copy(&src, &img).expect("copy the image");
    let mut bytes = std::fs::read(&img).expect("read the image");
    let len = bytes.len() as u64;

    let (part, vol) = find_fat_partition(&mut bytes, len)
        .unwrap_or_else(|e| panic!("finding the boot partition: {}", e.message()));
    println!(
        "found {} partition {} ({:?}) at {} — {:?}, {} clusters of {} bytes",
        part.scheme,
        part.number,
        part.name,
        part.offset,
        vol.kind,
        vol.cluster_count,
        vol.cluster_bytes()
    );

    insert_file(
        &mut bytes,
        &vol,
        &["CONFIG", "network", "securacv-hub"],
        KEYFILE,
    )
    .unwrap_or_else(|e| panic!("injection failed: {}", e.message()));
    std::fs::write(&img, &bytes).unwrap();

    // Our own reader, then two that are not ours.
    assert_eq!(
        read_file(&mut bytes, &vol, &["CONFIG", "network", "securacv-hub"]).unwrap(),
        KEYFILE
    );
    assert_eq!(
        mtools_read(&img, part.offset, "/CONFIG/network/securacv-hub"),
        KEYFILE
    );
    let listing = mtools_list(&img, part.offset, "/CONFIG");
    assert!(listing.contains("network"), "case lost:\n{listing}");

    // fsck.fat has no offset option, so audit the partition on its own.
    let part_file = dir.join("boot-partition.img");
    let end = (part.offset + part.size) as usize;
    std::fs::write(
        &part_file,
        &bytes[part.offset as usize..end.min(bytes.len())],
    )
    .unwrap();
    fsck_clean(&part_file);

    // Everything HAOS shipped in that partition is still there.
    let root = mtools_list(&img, part.offset, "/");
    for expected in ["CONFIG", "cmdline", "config"] {
        assert!(
            root.to_ascii_lowercase()
                .contains(&expected.to_ascii_lowercase()),
            "{expected} vanished from the boot partition:\n{root}"
        );
    }
    let _ = std::fs::remove_dir_all(&dir);
}
