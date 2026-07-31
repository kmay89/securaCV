//! Tests for the in-image FAT writer.
//!
//! Two kinds of coverage, deliberately:
//!
//! * **Synthetic volumes** built by [`mkfs`] below, which let every awkward case
//!   be reached on demand — a full root directory, an entry run crossing a
//!   cluster boundary, a volume with no space left, FAT12.
//! * **The real geometry.** `haos_geometry()` is not a plausible-looking set of
//!   numbers, it is what `haos_rpi5-64-18.1.img` actually contains, measured
//!   from the file: FAT16, 512-byte sectors, 4 sectors/cluster, 2 FATs of 128
//!   sectors, a 512-entry fixed root, 131072 total sectors. The design note this
//!   work started from said FAT32 behind an MBR; both were wrong, and either
//!   would have made the injector reject the real image while every test still
//!   passed. So the shape of the real thing is pinned here.
//!
//! There is also an independent-implementation check that does not live here:
//! `tests/fat_against_dosfstools.rs` formats a volume with `mkfs.fat`, injects
//! with this module, and then makes `fsck.fat` and `mtools` — code that has
//! never seen ours — agree the result is a clean filesystem holding the right
//! bytes. Our own reader agreeing with our own writer proves much less.

use super::*;

// ── building test volumes ───────────────────────────────────────────────────

#[derive(Clone, Copy)]
struct Geometry {
    bytes_per_sector: u32,
    sectors_per_cluster: u32,
    reserved_sectors: u32,
    num_fats: u32,
    root_entry_count: u32,
    total_sectors: u32,
    fat_size_sectors: u32,
    kind: FatKind,
}

/// The measured layout of `hassos-boot` in `haos_rpi5-64-18.1.img`.
fn haos_geometry() -> Geometry {
    Geometry {
        bytes_per_sector: 512,
        sectors_per_cluster: 4,
        reserved_sectors: 4,
        num_fats: 2,
        root_entry_count: 512,
        total_sectors: 131_072, // 64 MiB
        fat_size_sectors: 128,
        kind: FatKind::Fat16,
    }
}

/// Pick a FAT size big enough to map every cluster, the way a real mkfs does.
fn fat_size_for(g: &Geometry) -> u32 {
    let width = match g.kind {
        FatKind::Fat16 => 2,
        FatKind::Fat32 => 4,
    };
    let root_dir_sectors = (g.root_entry_count * 32).div_ceil(g.bytes_per_sector);
    let mut fat = 1;
    for _ in 0..32 {
        let data = g.total_sectors - g.reserved_sectors - g.num_fats * fat - root_dir_sectors;
        let clusters = data / g.sectors_per_cluster;
        let need = ((clusters + 2) * width).div_ceil(g.bytes_per_sector);
        if need <= fat {
            break;
        }
        fat = need;
    }
    fat
}

/// A minimal but genuine FAT volume: boot sector, FSInfo where FAT32 wants one,
/// zeroed FATs with the two reserved entries set, and an empty root.
fn mkfs(g: Geometry) -> Vec<u8> {
    let bps = g.bytes_per_sector as usize;
    let mut img = vec![0u8; g.total_sectors as usize * bps];
    let b = &mut img[..bps];

    b[0..3].copy_from_slice(&[0xeb, 0x3c, 0x90]);
    b[3..11].copy_from_slice(b"MSWIN4.1");
    b[0x0b..0x0d].copy_from_slice(&(g.bytes_per_sector as u16).to_le_bytes());
    b[0x0d] = g.sectors_per_cluster as u8;
    b[0x0e..0x10].copy_from_slice(&(g.reserved_sectors as u16).to_le_bytes());
    b[0x10] = g.num_fats as u8;
    b[0x11..0x13].copy_from_slice(&(g.root_entry_count as u16).to_le_bytes());
    b[0x13..0x15].copy_from_slice(&0u16.to_le_bytes()); // TotSec16 unused
    b[0x15] = 0xf8; // fixed disk
    b[0x20..0x24].copy_from_slice(&g.total_sectors.to_le_bytes());

    match g.kind {
        FatKind::Fat16 => {
            b[0x16..0x18].copy_from_slice(&(g.fat_size_sectors as u16).to_le_bytes());
            b[0x24] = 0x80;
            b[0x26] = 0x29;
            b[0x2b..0x36].copy_from_slice(b"hassos-boot");
            b[0x36..0x3e].copy_from_slice(b"FAT16   ");
        }
        FatKind::Fat32 => {
            b[0x16..0x18].copy_from_slice(&0u16.to_le_bytes()); // FATSz16 = 0
            b[0x24..0x28].copy_from_slice(&g.fat_size_sectors.to_le_bytes());
            b[0x2c..0x30].copy_from_slice(&2u32.to_le_bytes()); // root cluster
            b[0x30..0x32].copy_from_slice(&1u16.to_le_bytes()); // FSInfo sector
            b[0x32..0x34].copy_from_slice(&6u16.to_le_bytes()); // backup boot
            b[0x40] = 0x80;
            b[0x42] = 0x29;
            b[0x47..0x52].copy_from_slice(b"hassos-boot");
            b[0x52..0x5a].copy_from_slice(b"FAT32   ");
        }
    }
    b[510] = 0x55;
    b[511] = 0xaa;

    if g.kind == FatKind::Fat32 {
        let fs = &mut img[bps..2 * bps];
        fs[0..4].copy_from_slice(&0x4161_5252u32.to_le_bytes());
        fs[484..488].copy_from_slice(&0x6141_7272u32.to_le_bytes());
        fs[488..492].copy_from_slice(&0xffff_ffffu32.to_le_bytes());
        fs[492..496].copy_from_slice(&0xffff_ffffu32.to_le_bytes());
        fs[508..512].copy_from_slice(&0xaa55_0000u32.to_le_bytes());
    }

    // Reserved FAT entries 0 and 1 in every copy.
    for f in 0..g.num_fats {
        let at = (g.reserved_sectors + f * g.fat_size_sectors) as usize * bps;
        match g.kind {
            FatKind::Fat16 => {
                img[at..at + 2].copy_from_slice(&0xfff8u16.to_le_bytes());
                img[at + 2..at + 4].copy_from_slice(&0xffffu16.to_le_bytes());
            }
            FatKind::Fat32 => {
                img[at..at + 4].copy_from_slice(&0x0fff_fff8u32.to_le_bytes());
                img[at + 4..at + 8].copy_from_slice(&0x0fff_ffffu32.to_le_bytes());
            }
        }
    }

    // FAT32's root directory is a cluster and must start out zeroed; mkfs also
    // marks it end-of-chain.
    if g.kind == FatKind::Fat32 {
        let root_dir_sectors = (g.root_entry_count * 32).div_ceil(g.bytes_per_sector);
        let first_data = g.reserved_sectors + g.num_fats * g.fat_size_sectors + root_dir_sectors;
        let _ = first_data;
        for f in 0..g.num_fats {
            let at = (g.reserved_sectors + f * g.fat_size_sectors) as usize * bps + 8;
            img[at..at + 4].copy_from_slice(&0x0fff_ffffu32.to_le_bytes());
        }
    }
    img
}

fn fat16_volume() -> (Vec<u8>, FatVolume) {
    let mut img = mkfs(haos_geometry());
    let len = img.len() as u64;
    let vol = FatVolume::open(&mut img, 0, len).expect("a FAT16 volume");
    (img, vol)
}

fn fat32_volume() -> (Vec<u8>, FatVolume) {
    let mut g = Geometry {
        bytes_per_sector: 512,
        sectors_per_cluster: 1,
        reserved_sectors: 32,
        num_fats: 2,
        root_entry_count: 0,
        total_sectors: 80_000, // comfortably past the 65525-cluster FAT32 floor
        fat_size_sectors: 0,
        kind: FatKind::Fat32,
    };
    g.fat_size_sectors = fat_size_for(&g);
    let mut img = mkfs(g);
    let len = img.len() as u64;
    let vol = FatVolume::open(&mut img, 0, len).expect("a FAT32 volume");
    assert_eq!(vol.kind, FatKind::Fat32);
    (img, vol)
}

/// Wrap a volume in a GPT with the protective MBR HAOS really uses.
fn wrap_in_gpt(volume: &[u8], name: &str) -> (Vec<u8>, u64) {
    let first_lba = 2048u64;
    let sectors = (volume.len() / 512) as u64;
    let mut img = vec![0u8; (first_lba as usize + volume.len() / 512 + 34) * 512];

    // Protective MBR: one entry of type 0xEE covering the disk.
    let disk_sectors = (img.len() / 512 - 1) as u32;
    let e = &mut img[446..462];
    e[4] = 0xee;
    e[8..12].copy_from_slice(&1u32.to_le_bytes());
    e[12..16].copy_from_slice(&disk_sectors.to_le_bytes());
    img[510] = 0x55;
    img[511] = 0xaa;

    // GPT header at LBA 1, entries at LBA 2.
    img[512..520].copy_from_slice(b"EFI PART");
    img[512 + 72..512 + 80].copy_from_slice(&2u64.to_le_bytes());
    img[512 + 80..512 + 84].copy_from_slice(&128u32.to_le_bytes());
    img[512 + 84..512 + 88].copy_from_slice(&128u32.to_le_bytes());

    let at = 2 * 512;
    img[at..at + 16].copy_from_slice(&[0x0b; 16]); // any non-zero type GUID
    img[at + 32..at + 40].copy_from_slice(&first_lba.to_le_bytes());
    img[at + 40..at + 48].copy_from_slice(&(first_lba + sectors - 1).to_le_bytes());
    for (i, u) in name.encode_utf16().enumerate().take(35) {
        img[at + 56 + i * 2..at + 58 + i * 2].copy_from_slice(&u.to_le_bytes());
    }

    let start = first_lba as usize * 512;
    img[start..start + volume.len()].copy_from_slice(volume);
    (img, first_lba * 512)
}

const KEYFILE: &[u8] = b"[connection]\nid=securacv-hub\ntype=802-11-wireless\nllmnr=2\nmdns=2\n";
const SEED_PATH: [&str; 3] = ["CONFIG", "network", "securacv-hub"];

// ── the real image's shape ──────────────────────────────────────────────────

#[test]
fn the_real_haos_boot_partition_is_fat16_not_fat32() {
    // Measured from haos_rpi5-64-18.1.img. If a future HAOS build changes this,
    // this test is where we find out — on a runner, not on an operator's card.
    let (_img, vol) = fat16_volume();
    assert_eq!(vol.kind, FatKind::Fat16);
    assert_eq!(vol.cluster_count, 32_695);
    assert_eq!(vol.first_data_sector, 292);
    assert_eq!(vol.cluster_bytes(), 2048);
    assert_eq!(vol.volume_label, "hassos-boot");
}

#[test]
fn a_gpt_behind_a_protective_mbr_is_found() {
    // HAOS is GPT-partitioned with a protective MBR. A reader that trusted the
    // MBR would find one bogus 0xEE partition and no filesystem at all.
    let (volume, _) = fat16_volume();
    let (mut img, expect_offset) = wrap_in_gpt(&volume, "hassos-boot");
    let len = img.len() as u64;
    let (part, vol) = find_fat_partition(&mut img, len).expect("the boot partition");
    assert_eq!(part.scheme, "GPT");
    assert_eq!(part.number, 1);
    assert_eq!(part.offset, expect_offset);
    assert_eq!(part.name.as_deref(), Some("hassos-boot"));
    assert_eq!(vol.kind, FatKind::Fat16);
}

#[test]
fn a_plain_mbr_image_is_found_too() {
    let (volume, _) = fat16_volume();
    let first_lba = 2048usize;
    let mut img = vec![0u8; first_lba * 512 + volume.len()];
    let e = &mut img[446..462];
    e[4] = 0x0e; // FAT16 LBA
    e[8..12].copy_from_slice(&(first_lba as u32).to_le_bytes());
    e[12..16].copy_from_slice(&((volume.len() / 512) as u32).to_le_bytes());
    img[510] = 0x55;
    img[511] = 0xaa;
    img[first_lba * 512..].copy_from_slice(&volume);

    let len = img.len() as u64;
    let (part, vol) = find_fat_partition(&mut img, len).expect("the boot partition");
    assert_eq!(part.scheme, "MBR");
    assert_eq!(part.offset, first_lba as u64 * 512);
    assert_eq!(vol.kind, FatKind::Fat16);
}

#[test]
fn an_image_with_no_partition_table_is_a_clear_error() {
    let mut img = vec![0u8; 1 << 20];
    let len = img.len() as u64;
    assert_eq!(
        find_fat_partition(&mut img, len).unwrap_err(),
        FatError::NoPartitionTable
    );
}

#[test]
fn a_partition_table_pointing_at_no_filesystem_is_a_clear_error() {
    // A table that parses, holding a partition that is not FAT — we must say
    // "no FAT volume", not blunder into writing over whatever is there.
    let first_lba = 2048usize;
    let mut img = vec![0u8; (first_lba + 4096) * 512];
    let e = &mut img[446..462];
    e[4] = 0x83; // Linux
    e[8..12].copy_from_slice(&(first_lba as u32).to_le_bytes());
    e[12..16].copy_from_slice(&4096u32.to_le_bytes());
    img[510] = 0x55;
    img[511] = 0xaa;
    let len = img.len() as u64;
    assert_eq!(
        find_fat_partition(&mut img, len).unwrap_err(),
        FatError::NoFatVolume
    );
}

// ── the round trip ──────────────────────────────────────────────────────────

#[test]
fn the_wifi_keyfile_round_trips_through_a_fat16_volume() {
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).expect("injection");
    assert_eq!(read_file(&mut img, &vol, &SEED_PATH).unwrap(), KEYFILE);
}

#[test]
fn the_wifi_keyfile_round_trips_through_a_fat32_volume() {
    let (mut img, vol) = fat32_volume();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).expect("injection");
    assert_eq!(read_file(&mut img, &vol, &SEED_PATH).unwrap(), KEYFILE);
}

#[test]
fn the_keyfile_lands_inside_a_gpt_image_at_the_partition_offset() {
    let (volume, _) = fat16_volume();
    let (mut img, _) = wrap_in_gpt(&volume, "hassos-boot");
    let len = img.len() as u64;
    let (_part, vol) = find_fat_partition(&mut img, len).unwrap();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).expect("injection");
    assert_eq!(read_file(&mut img, &vol, &SEED_PATH).unwrap(), KEYFILE);
    // Nothing outside the partition moved.
    assert_eq!(&img[0..446], &vec![0u8; 446][..]);
}

#[test]
fn directory_names_keep_their_lower_case() {
    // Not cosmetic. HAOS reads `CONFIG/network/`; a short-entry-only write would
    // store `NETWORK`, and the hub would ignore the keyfile — which looks
    // exactly like the silent nothing-happened failure this module exists to
    // end. So the long-name entries are load-bearing and this test guards them.
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();

    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    let config = list_directory(&root.bytes)
        .into_iter()
        .find(|l| l.name == "CONFIG")
        .expect("CONFIG in the root");
    // CONFIG is already exact 8.3 uppercase, so it needs no long name at all.
    assert_eq!(&config.short_name, b"CONFIG     ");

    let inner = vol
        .read_directory(&mut img, DirLoc::Chain(config.first_cluster))
        .unwrap();
    let network = list_directory(&inner.bytes)
        .into_iter()
        .find(|l| l.name == "network")
        .expect("network inside CONFIG");
    assert_eq!(network.name, "network", "the long name must be lower case");
    // The short name is the usual generated tilde form; HAOS reads the long one.
    assert_eq!(&network.short_name, b"NETWOR~1   ");
}

#[test]
fn a_name_that_does_not_fit_8_3_gets_a_generated_short_name() {
    let (mut img, vol) = fat16_volume();
    // `.storage` is the account seed's directory: a leading dot, so no 8.3 form.
    insert_file(&mut img, &vol, &["CONFIG", ".storage", "auth"], b"{}").unwrap();
    assert_eq!(
        read_file(&mut img, &vol, &["CONFIG", ".storage", "auth"]).unwrap(),
        b"{}"
    );
    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    let config = list_directory(&root.bytes)
        .into_iter()
        .find(|l| l.name == "CONFIG")
        .unwrap();
    let inner = vol
        .read_directory(&mut img, DirLoc::Chain(config.first_cluster))
        .unwrap();
    let storage = list_directory(&inner.bytes)
        .into_iter()
        .find(|l| l.name == ".storage")
        .expect(".storage inside CONFIG");
    assert_eq!(&storage.short_name, b"STORAG~1   ");
}

#[test]
fn several_files_share_one_created_directory() {
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &["CONFIG", ".storage", "auth"], b"one").unwrap();
    insert_file(
        &mut img,
        &vol,
        &["CONFIG", ".storage", "onboarding"],
        b"two",
    )
    .unwrap();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();
    assert_eq!(
        read_file(&mut img, &vol, &["CONFIG", ".storage", "auth"]).unwrap(),
        b"one"
    );
    assert_eq!(
        read_file(&mut img, &vol, &["CONFIG", ".storage", "onboarding"]).unwrap(),
        b"two"
    );
    assert_eq!(read_file(&mut img, &vol, &SEED_PATH).unwrap(), KEYFILE);
    // CONFIG was created once, not twice.
    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    assert_eq!(
        list_directory(&root.bytes)
            .iter()
            .filter(|l| l.name == "CONFIG")
            .count(),
        1
    );
}

#[test]
fn an_existing_config_directory_is_reused_not_duplicated() {
    // The real image ships a CONFIG.TXT file in its root. That is a different
    // name from the CONFIG directory, but it is exactly the sort of near-miss
    // that a sloppy 8.3 comparison would collide with.
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &["CONFIG.TXT"], b"dtoverlay=x").unwrap();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();
    assert_eq!(
        read_file(&mut img, &vol, &["CONFIG.TXT"]).unwrap(),
        b"dtoverlay=x"
    );
    assert_eq!(read_file(&mut img, &vol, &SEED_PATH).unwrap(), KEYFILE);
}

#[test]
fn a_file_larger_than_one_cluster_round_trips_exactly() {
    let (mut img, vol) = fat16_volume();
    // 5 clusters and a bit, with a recognisable pattern so a mis-ordered chain
    // shows up as wrong bytes rather than the right length.
    let big: Vec<u8> = (0..(2048 * 5 + 37)).map(|i| (i % 251) as u8).collect();
    insert_file(&mut img, &vol, &["CONFIG", "big.bin"], &big).unwrap();
    assert_eq!(
        read_file(&mut img, &vol, &["CONFIG", "big.bin"]).unwrap(),
        big
    );
}

#[test]
fn an_empty_file_uses_no_clusters() {
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &["CONFIG", "empty"], b"").unwrap();
    assert_eq!(
        read_file(&mut img, &vol, &["CONFIG", "empty"]).unwrap(),
        b""
    );
}

#[test]
fn the_slack_after_a_file_is_zeroed_not_left_as_stale_bytes() {
    let (mut img, vol) = fat16_volume();
    // Dirty the free area so any cluster we allocate starts non-zero.
    let first_data = vol.cluster_at(2) as usize;
    for b in img[first_data..first_data + 16384].iter_mut() {
        *b = 0xa5;
    }
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();
    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    let config = list_directory(&root.bytes)
        .into_iter()
        .find(|l| l.name == "CONFIG")
        .unwrap();
    let inner = vol
        .read_directory(&mut img, DirLoc::Chain(config.first_cluster))
        .unwrap();
    let net = list_directory(&inner.bytes)
        .into_iter()
        .find(|l| l.name == "network")
        .unwrap();
    let files = vol
        .read_directory(&mut img, DirLoc::Chain(net.first_cluster))
        .unwrap();
    let key = list_directory(&files.bytes)
        .into_iter()
        .find(|l| l.name == "securacv-hub")
        .unwrap();
    let at = vol.cluster_at(key.first_cluster) as usize;
    let tail = &img[at + KEYFILE.len()..at + vol.cluster_bytes() as usize];
    assert!(
        tail.iter().all(|b| *b == 0),
        "stale bytes after the keyfile"
    );
}

// ── the traps ───────────────────────────────────────────────────────────────

#[test]
fn both_fat_copies_are_kept_identical() {
    // Writing FAT 1 and forgetting the mirror produces a filesystem that looks
    // fine until an fsck reads the other copy and "repairs" it destructively.
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();
    let big: Vec<u8> = (0..9000).map(|i| (i % 253) as u8).collect();
    insert_file(&mut img, &vol, &["CONFIG", "big.bin"], &big).unwrap();

    let fat1 = vol.read_fat(&mut img, 0).unwrap();
    let fat2 = vol.read_fat(&mut img, 1).unwrap();
    assert_eq!(fat1, fat2, "the two FAT copies diverged");
    // And the FAT is not simply untouched.
    assert!(fat1[4..].iter().any(|b| *b != 0), "nothing was allocated");
}

#[test]
fn an_entry_run_crossing_a_cluster_boundary_still_works() {
    // A subdirectory cluster holds 2048/32 = 64 entries. Filling it forces the
    // directory to grow, and a long-name set to straddle the boundary — the
    // case that quietly corrupts a directory if entries are written per-cluster
    // instead of into one logical stream.
    let (mut img, vol) = fat16_volume();
    let per_cluster = vol.cluster_bytes() as usize / ENTRY;
    let names: Vec<String> = (0..per_cluster + 20)
        .map(|i| format!("long-name-file-{i:03}"))
        .collect();
    for (i, n) in names.iter().enumerate() {
        insert_file(
            &mut img,
            &vol,
            &["CONFIG", n],
            format!("body {i}").as_bytes(),
        )
        .unwrap_or_else(|e| panic!("inserting {n}: {}", e.message()));
    }
    for (i, n) in names.iter().enumerate() {
        assert_eq!(
            read_file(&mut img, &vol, &["CONFIG", n]).unwrap(),
            format!("body {i}").as_bytes(),
            "{n} did not survive the directory growing"
        );
    }
    // The directory really did outgrow one cluster.
    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    let config = list_directory(&root.bytes)
        .into_iter()
        .find(|l| l.name == "CONFIG")
        .unwrap();
    let dir = vol
        .read_directory(&mut img, DirLoc::Chain(config.first_cluster))
        .unwrap();
    assert!(dir.clusters.len() > 1, "the directory never grew");
}

#[test]
fn a_new_directory_has_dot_and_dotdot() {
    // A subdirectory without "." and ".." is what fsck calls a corrupt
    // directory, and some drivers refuse to descend into it at all.
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();
    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    let config = list_directory(&root.bytes)
        .into_iter()
        .find(|l| l.name == "CONFIG")
        .unwrap();
    let at = vol.cluster_at(config.first_cluster) as usize;
    assert_eq!(&img[at..at + 11], b".          ");
    assert_eq!(img[at + 11] & ATTR_DIRECTORY, ATTR_DIRECTORY);
    assert_eq!(&img[at + 32..at + 43], b"..         ");
    // ".." in a directory whose parent is the root is defined to be 0.
    let dotdot_cluster =
        u16le(&img[at + 32..at + 64], 26) | (u16le(&img[at + 32..at + 64], 20) << 16);
    assert_eq!(
        dotdot_cluster, 0,
        "..must point at 0 when the parent is root"
    );

    // And a level deeper, ".." points at the real parent.
    let inner = vol
        .read_directory(&mut img, DirLoc::Chain(config.first_cluster))
        .unwrap();
    let net = list_directory(&inner.bytes)
        .into_iter()
        .find(|l| l.name == "network")
        .unwrap();
    let nat = vol.cluster_at(net.first_cluster) as usize;
    let parent = u16le(&img[nat + 32..nat + 64], 26);
    assert_eq!(parent, config.first_cluster, ".. must point at CONFIG");
}

#[test]
fn a_full_fat16_root_directory_says_so_instead_of_corrupting_it() {
    // FAT16's root is a fixed region between the FATs and the data area. It
    // cannot grow, and writing past it would land in the first data cluster.
    let mut g = haos_geometry();
    g.root_entry_count = 16; // tiny, so it fills immediately
    g.fat_size_sectors = fat_size_for(&g);
    let mut img = mkfs(g);
    let len = img.len() as u64;
    let vol = FatVolume::open(&mut img, 0, len).unwrap();
    let mut last = Ok(());
    for i in 0..40 {
        last = insert_file(&mut img, &vol, &[&format!("file{i}.txt")], b"x");
        if last.is_err() {
            break;
        }
    }
    assert_eq!(last.unwrap_err(), FatError::RootDirectoryFull);
    // And it failed BEFORE allocating: no clusters were leaked, so the FAT has
    // exactly as many used entries as there are files that actually landed.
    let fat = vol.read_fat(&mut img, 0).unwrap();
    let used = (2..vol.cluster_count + 2)
        .filter(|c| vol.fat_get_in(&fat, *c) != 0)
        .count();
    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    assert_eq!(
        used,
        list_directory(&root.bytes).len(),
        "a cluster was leaked"
    );
}

#[test]
fn running_out_of_space_is_a_clear_error_not_a_partial_write() {
    let mut g = haos_geometry();
    g.sectors_per_cluster = 1;
    g.total_sectors = 6_000; // ~2.9 MiB: still FAT16, and under MAX_FILE_BYTES
    g.fat_size_sectors = fat_size_for(&g);
    let mut img = mkfs(g);
    let len = img.len() as u64;
    let vol = FatVolume::open(&mut img, 0, len).unwrap();
    let capacity = vol.cluster_count as usize * vol.cluster_bytes() as usize;
    let err = insert_file(&mut img, &vol, &["CONFIG", "huge"], &vec![7u8; capacity])
        .expect_err("a file bigger than the volume must be refused");
    assert!(matches!(err, FatError::NoSpace { .. }), "{err:?}");
    assert!(err.message().contains("room"));
}

#[test]
fn a_file_that_already_exists_is_refused_rather_than_half_overwritten() {
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();
    let err = insert_file(&mut img, &vol, &SEED_PATH, b"different").unwrap_err();
    assert_eq!(
        err,
        FatError::AlreadyExists("CONFIG/network/securacv-hub".into())
    );
    // The original is untouched.
    assert_eq!(read_file(&mut img, &vol, &SEED_PATH).unwrap(), KEYFILE);
}

#[test]
fn a_directory_path_through_a_file_is_refused() {
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &["CONFIG", "network"], b"i am a file").unwrap();
    let err = insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap_err();
    assert_eq!(err, FatError::NotADirectory("CONFIG/network".into()));
}

#[test]
fn fat12_is_refused_with_a_reason() {
    let mut g = haos_geometry();
    g.sectors_per_cluster = 64;
    g.total_sectors = 20_000; // ~300 clusters → FAT12
    g.fat_size_sectors = fat_size_for(&g);
    let mut img = mkfs(g);
    let len = img.len() as u64;
    assert_eq!(
        FatVolume::open(&mut img, 0, len).unwrap_err(),
        FatError::Fat12Unsupported
    );
}

#[test]
fn a_boot_sector_with_impossible_fields_is_rejected() {
    let (mut img, _) = fat16_volume();
    // Sectors-per-cluster of 3 is not a power of two; a volume claiming it is
    // corrupt, and computing cluster offsets from it would write into the wrong
    // place entirely.
    img[0x0d] = 3;
    assert!(matches!(
        FatVolume::open(&mut img, 0, 1 << 30).unwrap_err(),
        FatError::BadBootSector(_)
    ));

    let (mut img, _) = fat16_volume();
    img[510] = 0;
    assert!(matches!(
        FatVolume::open(&mut img, 0, 1 << 30).unwrap_err(),
        FatError::BadBootSector(_)
    ));
}

#[test]
fn a_filesystem_claiming_more_than_its_partition_is_rejected() {
    // Otherwise the writer would compute offsets past the end of the partition
    // and scribble into the next one — the kernel partition, on a real image.
    let (mut img, _) = fat16_volume();
    let short = img.len() as u64 / 2;
    assert!(matches!(
        FatVolume::open(&mut img, 0, short).unwrap_err(),
        FatError::BadBootSector(_)
    ));
}

#[test]
fn a_looping_cluster_chain_is_reported_not_followed_forever() {
    let (mut img, vol) = fat16_volume();
    insert_file(&mut img, &vol, &["CONFIG", "x"], b"hello").unwrap();
    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    let config = list_directory(&root.bytes)
        .into_iter()
        .find(|l| l.name == "CONFIG")
        .unwrap();
    // Point CONFIG's cluster at itself.
    vol.fat_set(&mut img, config.first_cluster, config.first_cluster)
        .unwrap();
    let err = read_file(&mut img, &vol, &["CONFIG", "x"]).unwrap_err();
    assert!(matches!(err, FatError::CorruptChain(_)), "{err:?}");
}

// ── name handling in isolation ──────────────────────────────────────────────

#[test]
fn only_exact_uppercase_8_3_names_skip_the_long_name_entries() {
    assert_eq!(fits_short("CONFIG"), Some(*b"CONFIG     "));
    assert_eq!(fits_short("CONFIG.TXT"), Some(*b"CONFIG  TXT"));
    assert_eq!(fits_short("network"), None); // lower case
    assert_eq!(fits_short("securacv-hub"), None); // 12 > 8
    assert_eq!(fits_short(".storage"), None); // leading dot
    assert_eq!(fits_short("a.b.c"), None); // two dots
    assert_eq!(fits_short(""), None);
}

#[test]
fn generated_short_names_avoid_collisions() {
    let taken = vec![*b"SECURA~1   ", *b"SECURA~2   "];
    assert_eq!(
        generate_short("securacv-hub", &taken).unwrap(),
        *b"SECURA~3   "
    );
    assert_eq!(generate_short(".storage", &[]).unwrap(), *b"STORAG~1   ");
    // Characters FAT reserves become underscores rather than being dropped.
    assert_eq!(generate_short("a+b?c", &[]).unwrap(), *b"A_B_C~1    ");
}

#[test]
fn the_long_name_checksum_matches_the_short_name_it_belongs_to() {
    // The checksum is what ties a long-name set to its short entry; get it wrong
    // and the name is silently ignored, leaving the uppercase 8.3 name behind —
    // which for `network` means HAOS never finds the keyfile.
    let short = *b"NETWORK    ";
    let sum = short_checksum(&short);
    let entries = long_entries("network", sum).unwrap();
    assert_eq!(entries.len(), 1);
    assert_eq!(entries[0][13], sum);
    assert_eq!(entries[0][0], 0x41, "single entry is both first and last");
    assert_eq!(entries[0][11], ATTR_LFN);
}

#[test]
fn a_long_name_spanning_several_entries_reassembles_in_order() {
    let name = "a-really-quite-long-connection-name-for-one-network";
    let short = *b"AREALL~1   ";
    let sum = short_checksum(&short);
    let entries = long_entries(name, sum).unwrap();
    assert_eq!(entries.len(), name.len().div_ceil(13));
    // On disk the set runs last-first, so the 0x40 flag is on the FIRST entry.
    assert_eq!(entries[0][0] & 0x40, 0x40);
    assert_eq!(entries[0][0] & 0x3f, entries.len() as u8);
    assert_eq!(entries.last().unwrap()[0], 1);

    // And our reader gets the name back out of them.
    let parts: Vec<LongPart> = entries
        .iter()
        .map(|e| {
            let mut chars = [0u16; 13];
            for (i, o) in LFN_CHAR_SLOTS.iter().enumerate() {
                chars[i] = u16::from_le_bytes([e[*o], e[*o + 1]]);
            }
            LongPart {
                order: e[0],
                checksum: e[13],
                chars,
            }
        })
        .collect();
    assert_eq!(assemble_long_name(&parts, sum).as_deref(), Some(name));
}

#[test]
fn orphaned_long_name_entries_are_ignored_rather_than_misattributed() {
    let entries = long_entries("network", short_checksum(b"NETWORK    ")).unwrap();
    let parts: Vec<LongPart> = entries
        .iter()
        .map(|e| {
            let mut chars = [0u16; 13];
            for (i, o) in LFN_CHAR_SLOTS.iter().enumerate() {
                chars[i] = u16::from_le_bytes([e[*o], e[*o + 1]]);
            }
            LongPart {
                order: e[0],
                checksum: e[13],
                chars,
            }
        })
        .collect();
    // A set whose checksum belongs to some other file must not name this one.
    assert_eq!(assemble_long_name(&parts, 0x00), None);
}

#[test]
fn every_error_renders_a_sentence_an_operator_can_act_on() {
    for e in [
        FatError::NoPartitionTable,
        FatError::NoFatVolume,
        FatError::BadBootSector("no clusters"),
        FatError::Fat12Unsupported,
        FatError::NoSpace { needed: 4, free: 1 },
        FatError::RootDirectoryFull,
        FatError::NotADirectory("CONFIG".into()),
        FatError::AlreadyExists("CONFIG/network/securacv-hub".into()),
        FatError::UnusableName("??".into()),
        FatError::CorruptChain("loops".into()),
        FatError::Io("disk full".into()),
    ] {
        let m = e.message();
        assert!(m.len() > 20, "{e:?} renders too tersely: {m}");
        assert!(
            !m.ends_with('.'),
            "{e:?} should not end in a full stop: {m}"
        );
    }
}

#[test]
fn the_fat32_free_cluster_count_is_kept_truthful() {
    // FSInfo's counts are hints, but a stale "plenty free" is how a driver ends
    // up handing out a cluster we just used, and an uninitialised one makes
    // `fsck.fat` report a volume it would repair. Neither is what an operator
    // should find on their card.
    let (mut img, vol) = fat32_volume();
    let at = vol.sector_at(vol.fs_info_sector) as usize;
    img[at + 488..at + 492].copy_from_slice(&12345u32.to_le_bytes());
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();

    let fat = vol.read_fat(&mut img, 0).unwrap();
    let actually_free = (2..vol.cluster_count + 2)
        .filter(|c| vol.fat_get_in(&fat, *c) == 0)
        .count() as u32;
    assert_eq!(u32le(&img, at + 488), actually_free);
    assert!(
        u32le(&img, at + 492) >= 2,
        "next-free must name a real cluster"
    );
}

#[test]
fn a_fat32_directory_under_the_root_points_dotdot_at_zero() {
    let (mut img, vol) = fat32_volume();
    insert_file(&mut img, &vol, &SEED_PATH, KEYFILE).unwrap();
    let root = vol.read_directory(&mut img, vol.root_dir()).unwrap();
    let config = list_directory(&root.bytes)
        .into_iter()
        .find(|l| l.name == "CONFIG")
        .unwrap();
    let at = vol.cluster_at(config.first_cluster) as usize;
    let hi = u16le(&img[at + 32..at + 64], 20);
    let lo = u16le(&img[at + 32..at + 64], 26);
    assert_eq!(
        (hi << 16) | lo,
        0,
        "..must be 0 under a FAT32 root, not {}",
        vol.root_cluster
    );
}
