//! hub_fat — put a file into the boot filesystem *inside the image file*,
//! before it is ever written to a card. No mounting, on any OS.
//!
//! ## Why this exists
//!
//! The hub flasher used to write the image, then ask the operating system to
//! **re-mount the card's boot partition** so it could drop
//! `CONFIG/network/<id>` — the NetworkManager keyfile that lets a headless Pi
//! join Wi-Fi on first boot — into it. That re-mount is the single least
//! reliable step in the product. macOS's DiskArbitration routinely refuses to
//! mount a card it has just had raw-written, and when it refuses there is
//! nothing to retry into: the operator gets a hub that boots to
//! `wlan0: (No address)` and sits on the landing page forever, because Home
//! Assistant Core downloads itself on first boot and there is no network to
//! download over.
//!
//! For a headless hub that keyfile is not a convenience, it is the product.
//!
//! The other half of this product already solved the same problem the right
//! way: the ESP32 path never mounts anything — `provisioning.rs::
//! patch_factory_image()` rewrites the NVS partition *inside the downloaded
//! image* and lets the ordinary verified write carry it onto the chip. This
//! module is that idea for the Pi: edit the filesystem inside the `.img`, then
//! write the image exactly as before. The seed then inherits the write's own
//! byte-for-byte read-back verification instead of needing its own, failures
//! land in milliseconds before a single byte is written, and — because it is
//! pure computation over a byte range — it is testable on a headless runner
//! instead of only by a human holding a card reader.
//!
//! ## What the real image actually is
//!
//! Measured, not assumed, against `haos_rpi5-64-18.1.img` (both of these were
//! guessed wrong in the original design note, and either guess alone would have
//! made this module reject the real image):
//!
//! * The partition table is **GPT**, not MBR. The MBR at LBA 0 is the
//!   *protective* one — a single type-`0xEE` entry spanning the disk. Code that
//!   read only the MBR would find one bogus partition and no filesystem.
//! * `hassos-boot` (partition 1, 64 MiB) is **FAT16**, not FAT32:
//!   512-byte sectors, 4 sectors/cluster, 2 FATs of 128 sectors, a **fixed
//!   512-entry root directory**, 32695 clusters. FAT32 begins at 65525
//!   clusters; this is nowhere near it.
//!
//! So both partition schemes and both FAT widths are supported here, and the
//! FAT width is derived the way Microsoft's specification says it must be —
//! from the cluster count — rather than from the partition type byte, which
//! lies often enough to be worthless.
//!
//! ## Deliberate limits
//!
//! This is "add a small file to an existing volume", not a FAT driver. There is
//! no delete, no rename, no truncate, no in-place overwrite: creating a file
//! that already exists is an error rather than a silent half-edit. Everything
//! is bounds-checked against the partition and the cluster count, every walk of
//! a cluster chain is loop-guarded, and any inconsistency found on the way in is
//! an error rather than a guess. The failure mode we refuse to have is a card
//! that no longer boots — an *unseeded* card is recoverable, a *corrupted* one
//! costs the operator a re-flash and their confidence.
//!
//! Nothing here reads or writes the whole image: [`BlockIo`] is a
//! random-access, seek-based interface, and the largest buffer this module ever
//! holds is one FAT copy (64 KiB on the real image).

use std::io;

// ── the byte-range interface ────────────────────────────────────────────────

/// Random access to the bytes of an image (or, in tests, a `Vec<u8>`).
///
/// Deliberately *not* `Read + Seek`: this module touches a few kilobytes
/// scattered across a ~2.5 GB file, and the pipeline that calls it streams that
/// file to disk precisely so it never has to fit in RAM. An interface that
/// could only be driven by reading forward would invite someone to buffer the
/// whole image to satisfy it.
pub trait BlockIo {
    /// Fill `buf` from `offset`. Short reads are an error, not a partial fill.
    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> io::Result<()>;
    /// Write all of `buf` at `offset`.
    fn write_at(&mut self, offset: u64, buf: &[u8]) -> io::Result<()>;
}

impl<T: BlockIo + ?Sized> BlockIo for &mut T {
    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> io::Result<()> {
        (**self).read_at(offset, buf)
    }
    fn write_at(&mut self, offset: u64, buf: &[u8]) -> io::Result<()> {
        (**self).write_at(offset, buf)
    }
}

/// An in-memory image — what the tests drive, and the reason every edge case
/// below can be exercised without a card reader.
impl BlockIo for Vec<u8> {
    fn read_at(&mut self, offset: u64, buf: &mut [u8]) -> io::Result<()> {
        let start = usize::try_from(offset).map_err(|_| oob())?;
        let end = start.checked_add(buf.len()).ok_or_else(oob)?;
        if end > self.len() {
            return Err(oob());
        }
        buf.copy_from_slice(&self[start..end]);
        Ok(())
    }
    fn write_at(&mut self, offset: u64, buf: &[u8]) -> io::Result<()> {
        let start = usize::try_from(offset).map_err(|_| oob())?;
        let end = start.checked_add(buf.len()).ok_or_else(oob)?;
        if end > self.len() {
            return Err(oob());
        }
        self[start..end].copy_from_slice(buf);
        Ok(())
    }
}

fn oob() -> io::Error {
    io::Error::new(io::ErrorKind::UnexpectedEof, "past the end of the image")
}

// ── errors ──────────────────────────────────────────────────────────────────

/// Why a file could not be put into the image. Every variant is phrased so the
/// operator-facing message can be `e.message()` verbatim: this runs *before*
/// the write, so every one of these is "we didn't touch your card", never
/// "we half-wrote it".
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FatError {
    Io(String),
    /// Neither a GPT nor a usable MBR partition table at the start of the image.
    NoPartitionTable,
    /// A partition table was read, but no partition in it holds a FAT volume.
    NoFatVolume,
    /// The boot sector is not a FAT boot sector, or a field in it is impossible.
    BadBootSector(&'static str),
    /// FAT12 — see [`FatError::message`] for why this is refused rather than
    /// implemented.
    Fat12Unsupported,
    /// The volume does not have room for the file.
    NoSpace {
        needed: u32,
        free: u32,
    },
    /// A FAT16 root directory is a fixed-size region and this one is full.
    RootDirectoryFull,
    /// A path component exists but is a file where a directory was needed.
    NotADirectory(String),
    /// The file already exists. This module never overwrites.
    AlreadyExists(String),
    /// A name that cannot be stored (empty, too long for a long-name entry, or
    /// made only of characters FAT reserves).
    UnusableName(String),
    /// A cluster chain that loops or leaves the volume — a corrupt filesystem.
    CorruptChain(String),
}

impl FatError {
    pub fn message(&self) -> String {
        match self {
            FatError::Io(e) => format!("couldn't read or write the image file: {e}"),
            FatError::NoPartitionTable => {
                "the downloaded image has no partition table we recognise (neither GPT nor MBR) \
                 — it may be truncated or not a disk image at all"
                    .to_string()
            }
            FatError::NoFatVolume => {
                "the downloaded image has no FAT boot partition to put the settings in — this \
                 build of Home Assistant OS is laid out differently than the flasher expects"
                    .to_string()
            }
            FatError::BadBootSector(why) => {
                format!("the image's boot partition doesn't look like a FAT filesystem ({why})")
            }
            FatError::Fat12Unsupported => {
                // FAT12 packs entries into 12 bits, so an entry can straddle a
                // sector boundary — a genuinely different (and easy to get
                // subtly wrong) code path. A FAT12 volume tops out at 4084
                // clusters, and the boot partition we target is 64 MiB with
                // 32695; a real HAOS boot partition cannot be FAT12. Refusing
                // loudly beats carrying an untested write path for a case that
                // does not occur.
                "the image's boot partition is FAT12, which the flasher doesn't write to"
                    .to_string()
            }
            FatError::NoSpace { needed, free } => format!(
                "the image's boot partition doesn't have room for the settings \
                 ({needed} clusters needed, {free} free)"
            ),
            FatError::RootDirectoryFull => {
                "the image's boot partition has a full root directory, so the settings folder \
                 can't be added"
                    .to_string()
            }
            FatError::NotADirectory(p) => {
                format!("{p} already exists in the image as a file, so settings can't go inside it")
            }
            FatError::AlreadyExists(p) => {
                format!("{p} already exists in the image — the flasher won't overwrite it")
            }
            FatError::UnusableName(n) => {
                format!("{n:?} can't be stored as a filename on the boot partition")
            }
            FatError::CorruptChain(why) => {
                format!("the image's boot filesystem is inconsistent ({why}) — the download may be damaged")
            }
        }
    }
}

impl From<io::Error> for FatError {
    fn from(e: io::Error) -> Self {
        FatError::Io(e.to_string())
    }
}

type R<T> = Result<T, FatError>;

// ── little-endian field reads ───────────────────────────────────────────────

fn u16le(b: &[u8], o: usize) -> u32 {
    u16::from_le_bytes([b[o], b[o + 1]]) as u32
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn u64le(b: &[u8], o: usize) -> u64 {
    let mut v = [0u8; 8];
    v.copy_from_slice(&b[o..o + 8]);
    u64::from_le_bytes(v)
}

// ── finding the partition ───────────────────────────────────────────────────

/// A partition located in the image, in bytes from the start of the image.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PartitionRef {
    /// 1-based index in the partition table, as a human would name it.
    pub number: usize,
    pub offset: u64,
    pub size: u64,
    /// The GPT partition name (`hassos-boot`), when the table is GPT.
    pub name: Option<String>,
    /// `"GPT"` or `"MBR"` — for diagnostics only; the FAT parse is what decides.
    pub scheme: &'static str,
}

/// Sector size assumed when reading a partition table. Disk images are authored
/// at 512 regardless of the media they end up on, and both table formats place
/// their structures by LBA.
const LBA: u64 = 512;

/// Find the first partition in the image that holds a FAT volume the flasher
/// can write to, and parse it.
///
/// Both schemes are tried because HAOS uses GPT *behind a protective MBR*: an
/// MBR-first reader sees one type-`0xEE` entry covering the whole disk and, if
/// it trusted it, would try to parse a filesystem at LBA 1. The check for that
/// entry is what makes MBR parsing safe as a fallback.
///
/// "First FAT partition" rather than "the one named `hassos-boot`": HA renames
/// things between board families, and a volume that parses as FAT and sits
/// first in the table is what every bootloader in this class boots from. The
/// name is carried in [`PartitionRef`] for the log, not used as a gate.
pub fn find_fat_partition(io: &mut impl BlockIo, image_len: u64) -> R<(PartitionRef, FatVolume)> {
    let mut first = vec![0u8; 512];
    io.read_at(0, &mut first)?;
    if first[510] != 0x55 || first[511] != 0xaa {
        return Err(FatError::NoPartitionTable);
    }

    let mut candidates: Vec<PartitionRef> = Vec::new();

    // GPT lives at LBA 1 and is authoritative when present, protective MBR and
    // all.
    let mut gpt_header = vec![0u8; 512];
    if io.read_at(LBA, &mut gpt_header).is_ok() && &gpt_header[0..8] == b"EFI PART" {
        let entries_lba = u64le(&gpt_header, 72);
        let count = u32le(&gpt_header, 80).min(512);
        let entry_size = u32le(&gpt_header, 84);
        if (128..=4096).contains(&entry_size) {
            let mut entry = vec![0u8; entry_size as usize];
            for i in 0..count as u64 {
                let at = entries_lba * LBA + i * entry_size as u64;
                if io.read_at(at, &mut entry).is_err() {
                    break;
                }
                // An all-zero type GUID means the slot is unused.
                if entry[0..16].iter().all(|b| *b == 0) {
                    continue;
                }
                let first_lba = u64le(&entry, 32);
                let last_lba = u64le(&entry, 40);
                if last_lba < first_lba {
                    continue;
                }
                let name_u16: Vec<u16> = entry[56..128]
                    .chunks_exact(2)
                    .map(|c| u16::from_le_bytes([c[0], c[1]]))
                    .take_while(|c| *c != 0)
                    .collect();
                candidates.push(PartitionRef {
                    number: i as usize + 1,
                    offset: first_lba * LBA,
                    size: (last_lba - first_lba + 1) * LBA,
                    name: Some(String::from_utf16_lossy(&name_u16)),
                    scheme: "GPT",
                });
            }
        }
    }

    if candidates.is_empty() {
        for i in 0..4usize {
            let e = &first[446 + 16 * i..446 + 16 * i + 16];
            let kind = e[4];
            // 0x00 empty; 0xEE protective MBR in front of a GPT we already
            // failed to read; 0x05/0x0F extended containers hold no filesystem
            // of their own.
            if matches!(kind, 0x00 | 0xee | 0x05 | 0x0f) {
                continue;
            }
            let start = u32le(e, 8) as u64;
            let sectors = u32le(e, 12) as u64;
            if start == 0 || sectors == 0 {
                continue;
            }
            candidates.push(PartitionRef {
                number: i + 1,
                offset: start * LBA,
                size: sectors * LBA,
                name: None,
                scheme: "MBR",
            });
        }
    }

    if candidates.is_empty() {
        return Err(FatError::NoPartitionTable);
    }

    for part in candidates {
        if part.offset >= image_len || part.offset + part.size > image_len {
            continue;
        }
        if let Ok(vol) = FatVolume::open(io, part.offset, part.size) {
            return Ok((part, vol));
        }
    }
    Err(FatError::NoFatVolume)
}

// ── the volume ──────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FatKind {
    Fat16,
    Fat32,
}

/// A parsed FAT boot sector, with every derived value the writer needs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FatVolume {
    /// Byte offset of the partition within the image.
    pub part_offset: u64,
    pub bytes_per_sector: u32,
    pub sectors_per_cluster: u32,
    pub reserved_sectors: u32,
    pub num_fats: u32,
    pub fat_size_sectors: u32,
    /// Entries in the fixed root directory (FAT16). Zero on FAT32.
    pub root_entry_count: u32,
    pub total_sectors: u32,
    /// First cluster of the root directory (FAT32). Zero on FAT16.
    pub root_cluster: u32,
    /// FSInfo sector number (FAT32), or 0 for none.
    pub fs_info_sector: u32,
    pub first_data_sector: u32,
    pub cluster_count: u32,
    pub kind: FatKind,
    pub volume_label: String,
}

/// Cap on how much of a directory this module will read at once. A directory is
/// 32 bytes per entry; a megabyte is 32768 entries, orders of magnitude past
/// anything on a boot partition. Its real job is to stop a corrupt chain from
/// turning into an unbounded allocation.
const MAX_DIR_BYTES: usize = 1 << 20;

/// Cap on a single injected file. The keyfile is well under a kilobyte; the
/// account seed is a few kilobytes. Anything approaching this is a bug in the
/// caller, and refusing early beats filling a boot partition.
pub const MAX_FILE_BYTES: usize = 4 << 20;

impl FatVolume {
    /// Parse the boot sector of the partition at `part_offset`.
    ///
    /// The FAT width is derived from the **cluster count**, per Microsoft's
    /// specification — "there is no other way" is not a figure of speech there;
    /// the partition type byte and the `FAT16`/`FAT32` ASCII in the boot sector
    /// are both routinely wrong and neither is consulted here.
    pub fn open(io: &mut impl BlockIo, part_offset: u64, part_size: u64) -> R<FatVolume> {
        let mut bs = vec![0u8; 512];
        io.read_at(part_offset, &mut bs)?;
        if bs[510] != 0x55 || bs[511] != 0xaa {
            return Err(FatError::BadBootSector("no 0x55AA boot signature"));
        }
        // A FAT boot sector begins with a jump instruction. This is the cheapest
        // way to reject, say, an ext4 superblock area that happens to end in
        // 0x55AA.
        if !(bs[0] == 0xeb && bs[2] == 0x90) && bs[0] != 0xe9 {
            return Err(FatError::BadBootSector("no x86 jump at the start"));
        }

        let bytes_per_sector = u16le(&bs, 0x0b);
        if !matches!(bytes_per_sector, 512 | 1024 | 2048 | 4096) {
            return Err(FatError::BadBootSector("impossible bytes-per-sector"));
        }
        let sectors_per_cluster = bs[0x0d] as u32;
        if sectors_per_cluster == 0 || !sectors_per_cluster.is_power_of_two() {
            return Err(FatError::BadBootSector(
                "sectors-per-cluster not a power of two",
            ));
        }
        let reserved_sectors = u16le(&bs, 0x0e);
        if reserved_sectors == 0 {
            return Err(FatError::BadBootSector("zero reserved sectors"));
        }
        let num_fats = bs[0x10] as u32;
        if num_fats == 0 || num_fats > 4 {
            return Err(FatError::BadBootSector("implausible FAT count"));
        }
        let root_entry_count = u16le(&bs, 0x11);

        // TotSec16/FATSz16 are zero exactly when the 32-bit fields are in use;
        // that is the documented discriminator and it works before the FAT width
        // is known.
        let total_sectors = match u16le(&bs, 0x13) {
            0 => u32le(&bs, 0x20),
            n => n,
        };
        let fat_size_sectors = match u16le(&bs, 0x16) {
            0 => u32le(&bs, 0x24),
            n => n,
        };
        if total_sectors == 0 || fat_size_sectors == 0 {
            return Err(FatError::BadBootSector("zero total sectors or FAT size"));
        }

        let root_dir_sectors = (root_entry_count * 32).div_ceil(bytes_per_sector);
        let first_data_sector = reserved_sectors
            .checked_add(
                num_fats
                    .checked_mul(fat_size_sectors)
                    .ok_or(FatError::BadBootSector("FAT region overflows"))?,
            )
            .and_then(|v| v.checked_add(root_dir_sectors))
            .ok_or(FatError::BadBootSector("data region overflows"))?;
        if first_data_sector >= total_sectors {
            return Err(FatError::BadBootSector("no data region"));
        }
        let cluster_count = (total_sectors - first_data_sector) / sectors_per_cluster;
        if cluster_count == 0 {
            return Err(FatError::BadBootSector("no clusters"));
        }

        // Everything the volume claims must fit inside the partition it sits in.
        if (total_sectors as u64) * (bytes_per_sector as u64) > part_size {
            return Err(FatError::BadBootSector(
                "the filesystem claims more sectors than the partition holds",
            ));
        }

        let kind = if cluster_count < 4085 {
            return Err(FatError::Fat12Unsupported);
        } else if cluster_count < 65525 {
            FatKind::Fat16
        } else {
            FatKind::Fat32
        };

        let (root_cluster, fs_info_sector, label_at) = match kind {
            FatKind::Fat16 => {
                if root_entry_count == 0 {
                    return Err(FatError::BadBootSector("FAT16 with no root directory"));
                }
                (0, 0, 0x2b)
            }
            FatKind::Fat32 => {
                if root_entry_count != 0 {
                    return Err(FatError::BadBootSector("FAT32 with a fixed root directory"));
                }
                let rc = u32le(&bs, 0x2c);
                if rc < 2 || rc >= cluster_count + 2 {
                    return Err(FatError::BadBootSector("root cluster outside the volume"));
                }
                (rc, u16le(&bs, 0x30), 0x47)
            }
        };

        let volume_label = String::from_utf8_lossy(&bs[label_at..label_at + 11])
            .trim_end()
            .to_string();

        Ok(FatVolume {
            part_offset,
            bytes_per_sector,
            sectors_per_cluster,
            reserved_sectors,
            num_fats,
            fat_size_sectors,
            root_entry_count,
            total_sectors,
            root_cluster,
            fs_info_sector,
            first_data_sector,
            cluster_count,
            kind,
            volume_label,
        })
    }

    pub fn cluster_bytes(&self) -> u32 {
        self.sectors_per_cluster * self.bytes_per_sector
    }

    /// Bytes-from-image-start of a sector number relative to the partition.
    fn sector_at(&self, sector: u32) -> u64 {
        self.part_offset + sector as u64 * self.bytes_per_sector as u64
    }

    fn cluster_at(&self, cluster: u32) -> u64 {
        self.sector_at(self.first_data_sector + (cluster - 2) * self.sectors_per_cluster)
    }

    fn valid_cluster(&self, c: u32) -> bool {
        c >= 2 && c < self.cluster_count + 2
    }

    /// End-of-chain marker for this width. Any value at or above the low mark
    /// terminates a chain; we write the canonical all-ones form.
    fn eoc(&self) -> u32 {
        match self.kind {
            FatKind::Fat16 => 0xffff,
            FatKind::Fat32 => 0x0fff_ffff,
        }
    }

    fn is_eoc(&self, v: u32) -> bool {
        match self.kind {
            FatKind::Fat16 => v >= 0xfff8,
            FatKind::Fat32 => v >= 0x0fff_fff8,
        }
    }

    fn fat_entry_width(&self) -> u32 {
        match self.kind {
            FatKind::Fat16 => 2,
            FatKind::Fat32 => 4,
        }
    }

    /// Read one FAT copy whole. On the real image that is 64 KiB — small enough
    /// to scan for free clusters in one pass, and the only buffer in this module
    /// that scales with the volume rather than with the file being written.
    fn read_fat(&self, io: &mut impl BlockIo, which: u32) -> R<Vec<u8>> {
        let len = self.fat_size_sectors as usize * self.bytes_per_sector as usize;
        let mut fat = vec![0u8; len];
        io.read_at(
            self.sector_at(self.reserved_sectors + which * self.fat_size_sectors),
            &mut fat,
        )?;
        Ok(fat)
    }

    fn fat_get_in(&self, fat: &[u8], cluster: u32) -> u32 {
        let o = (cluster * self.fat_entry_width()) as usize;
        match self.kind {
            FatKind::Fat16 => u16le(fat, o),
            FatKind::Fat32 => u32le(fat, o) & 0x0fff_ffff,
        }
    }

    /// Set one FAT entry in **every** FAT copy. Writing FAT 1 and forgetting the
    /// mirror is the classic way to produce a filesystem that passes a casual
    /// look and gets "repaired" — destructively — by the first `fsck` that reads
    /// the other copy, so there is deliberately no single-copy variant of this.
    fn fat_set(&self, io: &mut impl BlockIo, cluster: u32, value: u32) -> R<()> {
        let width = self.fat_entry_width();
        let byte_in_fat = (cluster * width) as u64;
        let mut buf = [0u8; 4];
        let bytes: &[u8] = match self.kind {
            FatKind::Fat16 => {
                buf[..2].copy_from_slice(&(value as u16).to_le_bytes());
                &buf[..2]
            }
            FatKind::Fat32 => {
                // The top 4 bits of a FAT32 entry are reserved and must be
                // preserved rather than zeroed.
                let at = self.sector_at(self.reserved_sectors) + byte_in_fat;
                let mut cur = [0u8; 4];
                io.read_at(at, &mut cur)?;
                let merged = (u32::from_le_bytes(cur) & 0xf000_0000) | (value & 0x0fff_ffff);
                buf.copy_from_slice(&merged.to_le_bytes());
                &buf[..4]
            }
        };
        for f in 0..self.num_fats {
            let at =
                self.sector_at(self.reserved_sectors + f * self.fat_size_sectors) + byte_in_fat;
            io.write_at(at, bytes)?;
        }
        Ok(())
    }

    /// Walk a cluster chain, refusing to loop forever on a corrupt one.
    fn chain(&self, io: &mut impl BlockIo, first: u32) -> R<Vec<u32>> {
        let fat = self.read_fat(io, 0)?;
        let mut out = Vec::new();
        let mut c = first;
        while self.valid_cluster(c) {
            out.push(c);
            if out.len() > self.cluster_count as usize {
                return Err(FatError::CorruptChain("a cluster chain loops".into()));
            }
            let next = self.fat_get_in(&fat, c);
            if self.is_eoc(next) {
                return Ok(out);
            }
            if !self.valid_cluster(next) {
                return Err(FatError::CorruptChain(format!(
                    "cluster {c} points at {next}, which is outside the volume"
                )));
            }
            c = next;
        }
        Err(FatError::CorruptChain(format!(
            "chain starts at {first}, which is outside the volume"
        )))
    }

    /// Allocate `n` free clusters and link them into a chain, terminated with
    /// end-of-chain. Returns them in order.
    fn allocate(&self, io: &mut impl BlockIo, n: usize) -> R<Vec<u32>> {
        if n == 0 {
            return Ok(Vec::new());
        }
        let fat = self.read_fat(io, 0)?;
        let mut got = Vec::with_capacity(n);
        let mut free_total = 0u32;
        for c in 2..self.cluster_count + 2 {
            if self.fat_get_in(&fat, c) == 0 {
                free_total += 1;
                if got.len() < n {
                    got.push(c);
                }
            }
        }
        if got.len() < n {
            return Err(FatError::NoSpace {
                needed: n as u32,
                free: free_total,
            });
        }
        for i in 0..got.len() {
            let value = if i + 1 == got.len() {
                self.eoc()
            } else {
                got[i + 1]
            };
            self.fat_set(io, got[i], value)?;
        }
        let next_free = got.last().map_or(2, |c| c + 1).min(self.cluster_count + 1);
        self.update_fs_info(io, free_total - got.len() as u32, next_free)?;
        Ok(got)
    }

    /// Keep FAT32's FSInfo hints in step with what we just allocated.
    ///
    /// The free-cluster count is only a hint — `0xFFFFFFFF` means "unknown" and
    /// is legal — but `fsck.fat` reports an uninitialised summary as something
    /// it would repair, and an operator who runs a disk check on their card
    /// should see a clean bill, not a warning we chose to leave behind. The
    /// count is exact rather than estimated: `allocate` has just scanned the
    /// whole FAT, so the true free total is already known.
    fn update_fs_info(&self, io: &mut impl BlockIo, free: u32, next_free: u32) -> R<()> {
        if self.kind != FatKind::Fat32 || self.fs_info_sector == 0 {
            return Ok(());
        }
        let at = self.sector_at(self.fs_info_sector);
        let mut sig = [0u8; 4];
        io.read_at(at, &mut sig)?;
        if u32::from_le_bytes(sig) != 0x4161_5252 {
            // No FSInfo where the boot sector said there is one. Leave it be —
            // the counts are advisory and inventing a structure here would be a
            // worse guess than writing nothing.
            return Ok(());
        }
        io.write_at(at + 488, &free.to_le_bytes())?;
        io.write_at(at + 492, &next_free.to_le_bytes())?;
        Ok(())
    }

    fn zero_cluster(&self, io: &mut impl BlockIo, cluster: u32) -> R<()> {
        let zeros = vec![0u8; self.cluster_bytes() as usize];
        io.write_at(self.cluster_at(cluster), &zeros)?;
        Ok(())
    }
}

// ── directories ─────────────────────────────────────────────────────────────

/// Where a directory's entries live. FAT16's root is a fixed region that cannot
/// grow; everything else is an extendable cluster chain, and that difference is
/// the only thing this enum exists to keep straight.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DirLoc {
    FixedRoot,
    Chain(u32),
}

/// A directory read into memory, with the offsets needed to write it back.
struct DirImage {
    loc: DirLoc,
    clusters: Vec<u32>,
    bytes: Vec<u8>,
}

const ENTRY: usize = 32;
const ATTR_VOLUME_ID: u8 = 0x08;
const ATTR_DIRECTORY: u8 = 0x10;
const ATTR_ARCHIVE: u8 = 0x20;
const ATTR_LFN: u8 = 0x0f;
const ENTRY_FREE: u8 = 0xe5;
const ENTRY_END: u8 = 0x00;

/// A fixed timestamp for everything written: 1980-01-01 00:00:00, the start of
/// the FAT epoch. `hub-core` has no clock and no dependencies by design, and a
/// constant keeps the injector byte-reproducible — the same image plus the same
/// keyfile is the same bytes, which is what makes the tests exact.
const FAT_DATE_1980: u16 = 0x0021; // year 0, month 1, day 1
const FAT_TIME_ZERO: u16 = 0x0000;

impl FatVolume {
    fn root_dir(&self) -> DirLoc {
        match self.kind {
            FatKind::Fat16 => DirLoc::FixedRoot,
            FatKind::Fat32 => DirLoc::Chain(self.root_cluster),
        }
    }

    fn read_directory(&self, io: &mut impl BlockIo, loc: DirLoc) -> R<DirImage> {
        match loc {
            DirLoc::FixedRoot => {
                let len = self.root_entry_count as usize * ENTRY;
                let mut bytes = vec![0u8; len];
                io.read_at(
                    self.sector_at(self.reserved_sectors + self.num_fats * self.fat_size_sectors),
                    &mut bytes,
                )?;
                Ok(DirImage {
                    loc,
                    clusters: Vec::new(),
                    bytes,
                })
            }
            DirLoc::Chain(first) => {
                let clusters = self.chain(io, first)?;
                let cb = self.cluster_bytes() as usize;
                if clusters.len() * cb > MAX_DIR_BYTES {
                    return Err(FatError::CorruptChain(
                        "a directory is implausibly large".into(),
                    ));
                }
                let mut bytes = vec![0u8; clusters.len() * cb];
                for (i, c) in clusters.iter().enumerate() {
                    io.read_at(self.cluster_at(*c), &mut bytes[i * cb..(i + 1) * cb])?;
                }
                Ok(DirImage {
                    loc,
                    clusters,
                    bytes,
                })
            }
        }
    }

    /// Byte offset in the image of directory entry `index`.
    fn dir_entry_at(&self, dir: &DirImage, index: usize) -> u64 {
        match dir.loc {
            DirLoc::FixedRoot => {
                self.sector_at(self.reserved_sectors + self.num_fats * self.fat_size_sectors)
                    + (index * ENTRY) as u64
            }
            DirLoc::Chain(_) => {
                let per = self.cluster_bytes() as usize / ENTRY;
                let cluster = dir.clusters[index / per];
                self.cluster_at(cluster) + ((index % per) * ENTRY) as u64
            }
        }
    }

    /// Append one zeroed cluster to a directory's chain so more entries fit.
    fn grow_directory(&self, io: &mut impl BlockIo, dir: &mut DirImage) -> R<()> {
        let DirLoc::Chain(_) = dir.loc else {
            // FAT16's root directory is a fixed-size region between the FATs and
            // the data area; there is nowhere for it to grow into.
            return Err(FatError::RootDirectoryFull);
        };
        let new = self.allocate(io, 1)?[0];
        self.zero_cluster(io, new)?;
        let last = *dir
            .clusters
            .last()
            .ok_or_else(|| FatError::CorruptChain("a directory with no clusters".into()))?;
        self.fat_set(io, last, new)?;
        dir.clusters.push(new);
        dir.bytes
            .resize(dir.bytes.len() + self.cluster_bytes() as usize, 0);
        Ok(())
    }
}

/// One directory entry as this module cares about it.
struct Listed {
    name: String,
    attr: u8,
    first_cluster: u32,
    size: u32,
    short_name: [u8; 11],
}

/// Read a directory's live entries, resolving long names.
///
/// Long-name entries precede their short entry and are stored in reverse order;
/// each carries a checksum of the short name it belongs to, and a mismatch means
/// the set is orphaned (a driver crashed mid-write) and must be ignored rather
/// than trusted. Getting that wrong here would mean matching `CONFIG` against
/// the wrong entry, so the checksum is verified rather than assumed.
fn list_directory(bytes: &[u8]) -> Vec<Listed> {
    let mut out = Vec::new();
    let mut lfn: Vec<LongPart> = Vec::new();
    for e in bytes.chunks_exact(ENTRY) {
        match e[0] {
            ENTRY_END => break,
            ENTRY_FREE => {
                lfn.clear();
                continue;
            }
            _ => {}
        }
        let attr = e[11];
        if attr & ATTR_LFN == ATTR_LFN {
            let mut chars = [0u16; 13];
            for (i, o) in LFN_CHAR_SLOTS.iter().enumerate() {
                chars[i] = u16::from_le_bytes([e[*o], e[*o + 1]]);
            }
            lfn.push(LongPart {
                order: e[0],
                checksum: e[13],
                chars,
            });
            continue;
        }
        if attr & ATTR_VOLUME_ID != 0 {
            // The volume label lives in a root-directory entry and is not a file.
            lfn.clear();
            continue;
        }
        let mut short_name = [0u8; 11];
        short_name.copy_from_slice(&e[0..11]);
        let checksum = short_checksum(&short_name);
        let long = assemble_long_name(&lfn, checksum);
        lfn.clear();
        let name = long.unwrap_or_else(|| short_name_to_string(&short_name, e[12]));
        out.push(Listed {
            name,
            attr,
            first_cluster: (u16le(e, 20) << 16) | u16le(e, 26),
            size: u32le(e, 28),
            short_name,
        });
    }
    out
}

/// Where the 13 name characters sit inside a long-name entry: five at byte 1,
/// six at 14, two at 28, with the attribute, checksum and a must-be-zero
/// cluster field wedged between them.
const LFN_CHAR_SLOTS: [usize; 13] = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];

struct LongPart {
    order: u8,
    checksum: u8,
    chars: [u16; 13],
}

fn assemble_long_name(parts: &[LongPart], checksum: u8) -> Option<String> {
    if parts.is_empty() {
        return None;
    }
    // Every long entry carries a checksum of the short name it belongs to. A
    // mismatch means these entries are orphans left by a driver that died
    // mid-write, and reading a name out of them would attach it to an unrelated
    // file — so they are discarded, not trusted.
    if parts.iter().any(|p| p.checksum != checksum) {
        return None;
    }
    // The set is stored last-first: the entry flagged 0x40 is the tail of the
    // name and appears first on disk.
    let mut units: Vec<u16> = Vec::new();
    for (n, p) in parts.iter().rev().enumerate() {
        if p.order & 0x3f != n as u8 + 1 {
            return None; // a gap in the sequence — orphaned entries
        }
        if (p.order & 0x40 != 0) != (n + 1 == parts.len()) {
            return None; // the "last" flag is not on the last entry
        }
        units.extend_from_slice(&p.chars[..]);
    }
    while matches!(units.last(), Some(0) | Some(0xffff)) {
        units.pop();
    }
    if units.is_empty() {
        return None;
    }
    String::from_utf16(&units).ok()
}

fn short_name_to_string(raw: &[u8; 11], nt_res: u8) -> String {
    let mut base: Vec<u8> = raw[0..8].to_vec();
    // 0x05 in the first byte stands in for a real 0xE5, which would otherwise
    // read as "deleted".
    if base[0] == 0x05 {
        base[0] = 0xe5;
    }
    while base.last() == Some(&b' ') {
        base.pop();
    }
    let mut ext: Vec<u8> = raw[8..11].to_vec();
    while ext.last() == Some(&b' ') {
        ext.pop();
    }
    let mut s = String::from_utf8_lossy(&base).to_string();
    if nt_res & 0x08 != 0 {
        s = s.to_ascii_lowercase();
    }
    if !ext.is_empty() {
        let mut e = String::from_utf8_lossy(&ext).to_string();
        if nt_res & 0x10 != 0 {
            e = e.to_ascii_lowercase();
        }
        s.push('.');
        s.push_str(&e);
    }
    s
}

/// The first index of `n` consecutive free slots, if the directory has such a
/// run. A long-name set and its short entry must be contiguous, which is why
/// this looks for a run rather than filling gaps one slot at a time.
fn find_run(bytes: &[u8], n: usize) -> Option<usize> {
    let mut run = 0usize;
    let mut start = 0usize;
    for i in 0..bytes.len() / ENTRY {
        let first = bytes[i * ENTRY];
        if first == ENTRY_END || first == ENTRY_FREE {
            if run == 0 {
                start = i;
            }
            run += 1;
            if run == n {
                return Some(start);
            }
        } else {
            run = 0;
        }
    }
    None
}

/// The one-byte checksum tying a long-name set to its short entry.
fn short_checksum(short: &[u8; 11]) -> u8 {
    let mut sum: u8 = 0;
    for b in short {
        sum = ((sum & 1) << 7).wrapping_add(sum >> 1).wrapping_add(*b);
    }
    sum
}

// ── names ───────────────────────────────────────────────────────────────────

/// Characters legal in a short (8.3) name, beyond A–Z and 0–9.
const SHORT_OK: &[u8] = b"$%'-_@~`!(){}^#&";

fn short_char(c: u8) -> Option<u8> {
    if c.is_ascii_alphanumeric() {
        Some(c.to_ascii_uppercase())
    } else if SHORT_OK.contains(&c) {
        Some(c)
    } else {
        None
    }
}

/// Can this name be stored as a short entry alone — no long-name entries?
///
/// Only when it is *already* the exact 8.3 form: uppercase, legal characters, at
/// most one dot, 8 and 3. Anything else gets long-name entries, and that is not
/// cosmetic: HAOS looks for the directory `CONFIG/network` in lower case, and a
/// short-only entry would store `NETWORK`. A volume where the name reads back
/// uppercase is a hub that silently ignores the keyfile — the exact
/// nothing-happened failure this whole module exists to end.
fn fits_short(name: &str) -> Option<[u8; 11]> {
    if name.is_empty() || name.len() > 12 || name.starts_with('.') {
        return None;
    }
    let (base, ext) = match name.rfind('.') {
        Some(i) => (&name[..i], &name[i + 1..]),
        None => (name, ""),
    };
    if base.is_empty() || base.len() > 8 || ext.len() > 3 || ext.contains('.') {
        return None;
    }
    let mut raw = [b' '; 11];
    for (i, c) in base.bytes().enumerate() {
        match short_char(c) {
            Some(u) if u == c => raw[i] = u,
            _ => return None, // lowercase or illegal → needs a long name
        }
    }
    for (i, c) in ext.bytes().enumerate() {
        match short_char(c) {
            Some(u) if u == c => raw[8 + i] = u,
            _ => return None,
        }
    }
    Some(raw)
}

/// Build a short name to sit under a long name, avoiding collisions with what
/// is already in the directory (`NETWOR~1`, `NETWOR~2`, …).
fn generate_short(name: &str, taken: &[[u8; 11]]) -> R<[u8; 11]> {
    let cleaned: String = name
        .trim_matches(|c: char| c == '.' || c == ' ')
        .to_string();
    let (base_src, ext_src) = match cleaned.rfind('.') {
        Some(i) if i > 0 => (&cleaned[..i], &cleaned[i + 1..]),
        _ => (cleaned.as_str(), ""),
    };
    let squash = |s: &str, n: usize| -> Vec<u8> {
        s.bytes()
            .filter(|b| *b != b' ' && *b != b'.')
            .map(|b| short_char(b).unwrap_or(b'_'))
            .take(n)
            .collect()
    };
    let base = squash(base_src, 6);
    let ext = squash(ext_src, 3);
    if base.is_empty() {
        return Err(FatError::UnusableName(name.to_string()));
    }
    for n in 1..=999_999u32 {
        let tail = format!("~{n}");
        let keep = 8usize.saturating_sub(tail.len()).min(base.len());
        let mut raw = [b' '; 11];
        raw[..keep].copy_from_slice(&base[..keep]);
        raw[keep..keep + tail.len()].copy_from_slice(tail.as_bytes());
        raw[8..8 + ext.len()].copy_from_slice(&ext);
        if !taken.contains(&raw) {
            return Ok(raw);
        }
    }
    Err(FatError::UnusableName(name.to_string()))
}

/// The long-name entries for `name`, in the reversed on-disk order, to be
/// written immediately before the short entry.
fn long_entries(name: &str, checksum: u8) -> R<Vec<[u8; ENTRY]>> {
    let units: Vec<u16> = name.encode_utf16().collect();
    // 20 entries × 13 characters is the format's ceiling.
    if units.is_empty() || units.len() > 255 {
        return Err(FatError::UnusableName(name.to_string()));
    }
    let count = units.len().div_ceil(13);
    let mut out = Vec::with_capacity(count);
    for k in 0..count {
        let mut e = [0u8; ENTRY];
        e[0] = (k as u8) + 1;
        if k + 1 == count {
            e[0] |= 0x40; // last entry of the set
        }
        e[11] = ATTR_LFN;
        e[12] = 0;
        e[13] = checksum;
        // Bytes 26..28 are a first-cluster field that must be zero here.
        for (i, o) in LFN_CHAR_SLOTS.iter().enumerate() {
            let idx = k * 13 + i;
            let v = match idx.cmp(&units.len()) {
                std::cmp::Ordering::Less => units[idx],
                // Exactly one NUL terminates the name; the rest is 0xFFFF pad.
                std::cmp::Ordering::Equal => 0x0000,
                std::cmp::Ordering::Greater => 0xffff,
            };
            e[*o..*o + 2].copy_from_slice(&v.to_le_bytes());
        }
        out.push(e);
    }
    out.reverse();
    Ok(out)
}

fn short_entry(short: &[u8; 11], attr: u8, first_cluster: u32, size: u32) -> [u8; ENTRY] {
    let mut e = [0u8; ENTRY];
    e[0..11].copy_from_slice(short);
    e[11] = attr;
    e[14..16].copy_from_slice(&FAT_TIME_ZERO.to_le_bytes());
    e[16..18].copy_from_slice(&FAT_DATE_1980.to_le_bytes());
    e[18..20].copy_from_slice(&FAT_DATE_1980.to_le_bytes());
    e[20..22].copy_from_slice(&((first_cluster >> 16) as u16).to_le_bytes());
    e[22..24].copy_from_slice(&FAT_TIME_ZERO.to_le_bytes());
    e[24..26].copy_from_slice(&FAT_DATE_1980.to_le_bytes());
    e[26..28].copy_from_slice(&((first_cluster & 0xffff) as u16).to_le_bytes());
    e[28..32].copy_from_slice(&size.to_le_bytes());
    e
}

impl FatVolume {
    /// Place a run of entries in a directory, extending it if there is no room.
    ///
    /// A long-name set and its short entry must be **contiguous**, so this looks
    /// for a run of that length rather than filling gaps one at a time. The run
    /// may straddle a cluster boundary — a directory is one logical stream over
    /// its chain, so that is fine — but it may not run off the end of the
    /// stream, which is where extending comes in.
    fn place_entries(
        &self,
        io: &mut impl BlockIo,
        dir: &mut DirImage,
        entries: &[[u8; ENTRY]],
    ) -> R<()> {
        self.ensure_room(io, dir, entries.len())?;
        let start = find_run(&dir.bytes, entries.len()).expect("ensure_room just made room");
        for (k, e) in entries.iter().enumerate() {
            let at = self.dir_entry_at(dir, start + k);
            io.write_at(at, e)?;
            dir.bytes[(start + k) * ENTRY..(start + k + 1) * ENTRY].copy_from_slice(e);
        }
        Ok(())
    }

    /// Make sure `n` contiguous entry slots exist, growing the directory if it
    /// is the growable kind.
    ///
    /// Callers run this **before** allocating clusters for the thing being
    /// named. Otherwise a directory that turns out to be full — a FAT16 root
    /// cannot grow at all — leaves the file's data already written into
    /// clusters already marked in use, with no entry pointing at them: a leak
    /// that `fsck` reports as lost sectors on a card the operator is about to
    /// boot. Checking first means the failure changes nothing.
    fn ensure_room(&self, io: &mut impl BlockIo, dir: &mut DirImage, n: usize) -> R<()> {
        while find_run(&dir.bytes, n).is_none() {
            self.grow_directory(io, dir)?;
        }
        Ok(())
    }

    /// Plan the on-disk naming of one item: the long-name entries (empty when
    /// the name is already an exact 8.3) and the short name they check against.
    ///
    /// Split out from writing so callers can learn how many entry slots they
    /// will need before committing to anything.
    fn plan_name(&self, name: &str, taken: &[[u8; 11]]) -> R<(Vec<[u8; ENTRY]>, [u8; 11])> {
        if name.is_empty() || name == "." || name == ".." || name.contains('/') {
            return Err(FatError::UnusableName(name.to_string()));
        }
        if let Some(short) = fits_short(name) {
            if !taken.contains(&short) {
                return Ok((Vec::new(), short));
            }
        }
        let short = generate_short(name, taken)?;
        let lfn = long_entries(name, short_checksum(&short))?;
        Ok((lfn, short))
    }

    /// Find `name` in `dir`, creating it as a subdirectory if it isn't there.
    fn open_or_create_dir(
        &self,
        io: &mut impl BlockIo,
        parent: DirLoc,
        name: &str,
        display_path: &str,
    ) -> R<DirLoc> {
        let mut dir = self.read_directory(io, parent)?;
        let listed = list_directory(&dir.bytes);
        if let Some(hit) = listed.iter().find(|l| l.name.eq_ignore_ascii_case(name)) {
            if hit.attr & ATTR_DIRECTORY == 0 {
                return Err(FatError::NotADirectory(display_path.to_string()));
            }
            if !self.valid_cluster(hit.first_cluster) {
                return Err(FatError::CorruptChain(format!(
                    "{display_path} points at cluster {}, which is outside the volume",
                    hit.first_cluster
                )));
            }
            return Ok(DirLoc::Chain(hit.first_cluster));
        }

        // Create it: one zeroed cluster holding the mandatory "." and ".."
        // entries, then an entry for it in the parent. Room in the parent is
        // reserved first — see `ensure_room`.
        let taken: Vec<[u8; 11]> = listed.iter().map(|l| l.short_name).collect();
        let (lfn, short) = self.plan_name(name, &taken)?;
        self.ensure_room(io, &mut dir, lfn.len() + 1)?;

        let cluster = self.allocate(io, 1)?[0];
        self.zero_cluster(io, cluster)?;
        let parent_cluster = match parent {
            // ".." in a directory whose parent is the root is defined to be 0,
            // on FAT32 as well as FAT16 — not the actual root cluster number.
            DirLoc::FixedRoot => 0,
            DirLoc::Chain(c) if self.kind == FatKind::Fat32 && c == self.root_cluster => 0,
            DirLoc::Chain(c) => c,
        };
        let mut dot = [b' '; 11];
        dot[0] = b'.';
        let mut dotdot = [b' '; 11];
        dotdot[0] = b'.';
        dotdot[1] = b'.';
        let at = self.cluster_at(cluster);
        io.write_at(at, &short_entry(&dot, ATTR_DIRECTORY, cluster, 0))?;
        io.write_at(
            at + ENTRY as u64,
            &short_entry(&dotdot, ATTR_DIRECTORY, parent_cluster, 0),
        )?;

        let mut entries = lfn;
        entries.push(short_entry(&short, ATTR_DIRECTORY, cluster, 0));
        self.place_entries(io, &mut dir, &entries)?;
        Ok(DirLoc::Chain(cluster))
    }
}

/// Put `contents` at `path` (e.g. `["CONFIG", "network", "securacv-hub"]`)
/// inside `vol`, creating any missing parent directories.
///
/// Pure computation over `io`; nothing outside the partition is touched, and an
/// error means nothing was written that a FAT driver will see — the worst case
/// is a few clusters marked in use with no directory entry pointing at them,
/// which is a leak on a throwaway temp file, not damage to a card.
pub fn insert_file(
    io: &mut impl BlockIo,
    vol: &FatVolume,
    path: &[&str],
    contents: &[u8],
) -> R<()> {
    let Some((file_name, dirs)) = path.split_last() else {
        return Err(FatError::UnusableName(String::new()));
    };
    if contents.len() > MAX_FILE_BYTES {
        return Err(FatError::UnusableName(format!(
            "{file_name} ({} bytes is too large to seed)",
            contents.len()
        )));
    }

    let mut loc = vol.root_dir();
    let mut walked = String::new();
    for d in dirs {
        walked.push_str(d);
        loc = vol.open_or_create_dir(io, loc, d, &walked)?;
        walked.push('/');
    }
    walked.push_str(file_name);

    let mut dir = vol.read_directory(io, loc)?;
    let listed = list_directory(&dir.bytes);
    if listed
        .iter()
        .any(|l| l.name.eq_ignore_ascii_case(file_name))
    {
        return Err(FatError::AlreadyExists(walked));
    }

    // Reserve the directory slots BEFORE allocating any data clusters, so a
    // directory that can't hold the entry fails having changed nothing.
    let taken: Vec<[u8; 11]> = listed.iter().map(|l| l.short_name).collect();
    let (lfn, short) = vol.plan_name(file_name, &taken)?;
    vol.ensure_room(io, &mut dir, lfn.len() + 1)?;

    // Allocate, fill, and zero the slack in the final cluster so the file never
    // trails whatever was in that free space before.
    let cluster_bytes = vol.cluster_bytes() as usize;
    let needed = contents.len().div_ceil(cluster_bytes.max(1));
    let clusters = vol.allocate(io, needed)?;
    for (i, c) in clusters.iter().enumerate() {
        let start = i * cluster_bytes;
        let end = (start + cluster_bytes).min(contents.len());
        let mut chunk = vec![0u8; cluster_bytes];
        chunk[..end - start].copy_from_slice(&contents[start..end]);
        io.write_at(vol.cluster_at(*c), &chunk)?;
    }

    let mut entries = lfn;
    entries.push(short_entry(
        &short,
        ATTR_ARCHIVE,
        clusters.first().copied().unwrap_or(0),
        contents.len() as u32,
    ));
    vol.place_entries(io, &mut dir, &entries)?;
    Ok(())
}

/// Read a file back out of the volume — the other half of the round trip, used
/// to prove an injection landed and by the tests to check it byte-for-byte.
pub fn read_file(io: &mut impl BlockIo, vol: &FatVolume, path: &[&str]) -> R<Vec<u8>> {
    let Some((file_name, dirs)) = path.split_last() else {
        return Err(FatError::UnusableName(String::new()));
    };
    let mut loc = vol.root_dir();
    let mut walked = String::new();
    for d in dirs {
        walked.push_str(d);
        let dir = vol.read_directory(io, loc)?;
        let listed = list_directory(&dir.bytes);
        let hit = listed
            .iter()
            .find(|l| l.name.eq_ignore_ascii_case(d))
            .ok_or_else(|| FatError::CorruptChain(format!("{walked} is not in the image")))?;
        if hit.attr & ATTR_DIRECTORY == 0 {
            return Err(FatError::NotADirectory(walked.clone()));
        }
        loc = DirLoc::Chain(hit.first_cluster);
        walked.push('/');
    }
    walked.push_str(file_name);
    let dir = vol.read_directory(io, loc)?;
    let listed = list_directory(&dir.bytes);
    let hit = listed
        .iter()
        .find(|l| l.name.eq_ignore_ascii_case(file_name))
        .ok_or_else(|| FatError::CorruptChain(format!("{walked} is not in the image")))?;
    let size = hit.size as usize;
    if size == 0 {
        return Ok(Vec::new());
    }
    let clusters = vol.chain(io, hit.first_cluster)?;
    let cb = vol.cluster_bytes() as usize;
    let mut out = vec![0u8; clusters.len() * cb];
    for (i, c) in clusters.iter().enumerate() {
        io.read_at(vol.cluster_at(*c), &mut out[i * cb..(i + 1) * cb])?;
    }
    out.truncate(size);
    Ok(out)
}

#[cfg(test)]
mod tests;
