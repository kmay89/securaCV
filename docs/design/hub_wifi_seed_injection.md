# Seeding the hub's Wi-Fi without mounting the card

**Status:** built. `hub_core::hub_fat` does the injection, `hub_io::seed`
drives it, and the mount/eject path it replaced is deleted. What follows is the
spec as written, **corrected where building it proved the spec wrong** — the
wrong guesses are left visible on purpose, because two of them would each have
been enough to make the finished code reject the real image.

## The problem, stated exactly

The hub flasher writes a verified HAOS image to an SD card, then **re-mounts
the card's boot partition** to drop one file into it:
`CONFIG/network/securacv-hub`, the NetworkManager keyfile that makes a
keyboard-less, screen-less Pi join a network on first boot.

That re-mount is unreliable. macOS's DiskArbitration frequently refuses to
mount a card it has just had raw-written, and the current code retries for
~20 s with `diskutil mountDisk` nudges before giving up:

```
couldn't mount the boot partition /dev/disk4s1: Volume on disk4s1 failed to
mount (tried for ~20 seconds; macOS wouldn't re-mount the freshly written card)
```

It has failed a real operator twice. Retrying usually works, which is exactly
what makes it insidious: it is reliable enough to look fine and unreliable
enough to strand someone.

**For a headless hub the Wi-Fi seed is not a convenience — it is the product.**
Without it the Pi boots to `wlan0: (No address)` and sits on the Home Assistant
landing page forever, because Core downloads itself on first boot and there is
no network to download over. That is the observed failure, photographed on
real hardware.

## What was already fixed (and what it did not fix)

Flasher 0.3.6 made the failure **loud**: the keyfile is written, read back, and
compared; the file's `fsync` is checked rather than discarded; and a Wi-Fi that
was requested but did not land now **fails the flash** with the reason instead
of reporting "Done — written, read back, and verified" over a card that can
never join a network.

That was the right fix for *silence*. It does not make the seed *reliable* — it
converts an invisible failure into a visible one. The operator still has to
retry.

## Why more retrying is the wrong answer

Lengthening the window or adding more `diskutil` nudges makes a flaky step
slower, not dependable, and it keeps a hard dependency on OS behavior we
neither control nor can test in CI. Every platform gets its own version of the
problem (`udisksctl` on Linux, drive letters on Windows), and each is a
separate source of "worked on my machine".

## The fix: put the file in the image, before it is written

**We already do this, on the other half of the product.** The ESP32 path never
mounts anything: `provisioning.rs::patch_factory_image()` finds the NVS
partition *inside the downloaded firmware image*, rewrites that region in
memory, and the ordinary verified write carries it onto the chip. It has never
failed for this reason, because there is no filesystem, no mount, and no OS
cooperation involved.

The hub is the same problem with a FAT32 filesystem instead of an NVS
partition:

1. Parse the HAOS image's partition table; find the boot volume.
2. Add `CONFIG/network/<stem>` to that filesystem, in place.
3. Write the image exactly as today.

### Two things this document got wrong

Both were measured from `haos_rpi5-64-18.1.img` before a line was written, and
either one alone would have produced an injector that refused the real image
while every test still passed:

- **The partition table is GPT, not MBR.** The MBR at LBA 0 is the *protective*
  one — a single type-`0xEE` entry spanning the disk. An MBR-only reader finds
  that one bogus partition and no filesystem at all. Both schemes are now
  handled, GPT first.
- **`hassos-boot` is FAT16, not FAT32.** 64 MiB, 512-byte sectors, 4
  sectors/cluster, 2 FATs of 128 sectors, a **fixed 512-entry root directory**,
  32695 clusters. FAT32 starts at 65525 clusters. The two formats differ in FAT
  entry width *and* in whether the root directory can grow at all, so this was
  not a detail. Both widths are supported, and the width is derived from the
  cluster count as the specification requires — never from the partition type
  byte or the `FAT16`/`FAT32` string in the boot sector, both of which lie.

The lesson is cheap to state and was nearly expensive: **the design note was
written from memory of how these images are usually laid out.** Ten minutes with
the actual file first would have cost nothing; finding out after the code was
written would have cost the session.

### What this buys

- **No mount, on any OS.** `mount_partition`, `eject`, and the whole
  platform-specific command glue in `hub-io/src/seed.rs` — the most fragile
  code in the crate, and the source of both real-world failures — get deleted.
- **The seed inherits the image's verification.** The write is already read
  back byte-for-byte; once the keyfile is *in* the image, proving the image
  proves the keyfile. The bespoke read-back added in 0.3.6 becomes redundant.
- **Failures move earlier and get cheaper.** A keyfile that cannot be injected
  is a pure-computation error surfaced in milliseconds, before a single byte is
  written — instead of after a multi-GB write and a 20-second mount timeout.
- **It becomes testable in CI.** Pure computation over a byte buffer runs on a
  headless Linux runner. The current mount path can only be tested by a human
  with a card reader.

## Implementation notes

**Where:** `hub-core` — pure, dependency-free, host-tested, beside
`hub_seed::wifi_keyfile` which already renders the keyfile text. Explicitly
*not* `hub-io`: the point of the change is that this stops being I/O.

**Scope:** "add a small file to an existing FAT32 volume", not a FAT driver.
No deletion, no rename, no fragmentation handling beyond appending. The
keyfile is under 1 KB and the HAOS boot partition has ample free space.

**Steps:** read the BPB (sector size, sectors per cluster, reserved sectors,
FAT count/size, root cluster); walk the root directory for `CONFIG`, creating
it if absent, then `network` inside it; allocate free clusters from the FAT;
write the file's data; add the directory entry; update **both** FAT copies.

**Correctness traps to respect:**

- Both FATs must agree. Writing one and not the mirror is a corruption a
  cursory test will not catch.
- `CONFIG` and `network` fit 8.3, but the entries must still be written the way
  a real driver expects, and the keyfile stem (`securacv-hub`) does not fit 8.3
  — it needs LFN entries with correct checksums.
- Directory entries must not be appended past a cluster boundary without
  allocating and chaining the next cluster.
- **Never hold the image in memory.** The pipeline streams the ~2.5 GB raw
  image to a temp file precisely so it does not have to fit in RAM, and a
  whole-image buffer would exhaust a 4 GB machine before the write began. The
  injector takes a seek-based `BlockIo` for this reason; the largest buffer it
  holds is one FAT copy (64 KiB on the real image).

**Tests:** synthetic fixtures for the edge cases (existing `CONFIG` directory,
absent `CONFIG`, an entry run crossing a cluster boundary, FAT mirror
consistency, a full FAT16 root, a full volume, FAT12), plus a round-trip against
the **real HAOS image**.

The load-bearing part is that **our reader is not the judge**. A writer and a
reader that share a wrong idea of the layout agree with each other perfectly and
still produce a card the Pi cannot read. So `mkfs.fat` creates the volume,
`fsck.fat -n` audits the result — both FAT copies, every cluster chain, every
directory entry — and `mtools` reads the file back with an implementation that
has never seen ours. CI installs those tools and **fails if the tests skip**,
because a skipped test that reports green is the same silent no-op this whole
change exists to remove.

Result against the real image: the keyfile round-trips byte-for-byte, `network`
keeps its lower case, HAOS's own `cmdline.txt` / `config.txt` / `SLOT-A` are
untouched, and `fsck.fat` reports a clean 387-file volume.

**Keep 0.3.6's posture:** a Wi-Fi that was requested and could not be injected
still fails the flash, with the reason. Injection makes that check cheaper and
earlier; it does not make it optional.

**The account seed came along.** The EXPERIMENTAL Home Assistant account
pre-seed (`CONFIG/.storage/…`) and the provisioning bundle
(`CONFIG/securacv/…`, four levels deep) both use the same path, so both are
injected now and no mount code survives anywhere. They keep their old severity,
though: Wi-Fi failing is fatal, and the account/bundle seed failing is a note —
it is opt-in and HA's own setup wizard is a fine fallback, so it must not sink a
flash that would otherwise work.

**Lower case is load-bearing.** HAOS reads `CONFIG/network`, and a FAT short
entry stores `NETWORK`. Long-name entries are therefore not cosmetic: without
them the keyfile is written correctly and ignored completely — the same
nothing-happened failure in a new costume. There is a test for exactly this.

## The general lesson

This is the same shape as several other bugs found the same evening: **a step
that can silently no-op will eventually no-op silently.** 0.3.6 fixed the
silence. This fixes the step.

And the deeper one: when two halves of a product solve the same problem and one
of them never fails, copy the one that works. The ESP32 path had the answer
already.
