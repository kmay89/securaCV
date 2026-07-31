# Seeding the hub's Wi-Fi without mounting the card

**Status:** designed, not built. This is the spec for the work; nothing here
ships yet.

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
slower, not dependable, and it keeps a hard dependency on OS behaviour we
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

1. Parse the HAOS image's MBR partition table; find partition 1 (the FAT32
   boot volume).
2. In memory, add `CONFIG/network/<stem>` to that filesystem.
3. Write the image exactly as today.

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
- The image is ~1 GB and lives in memory during patching; do not copy it more
  times than necessary.

**Tests:** synthetic FAT32 fixtures for the edge cases (existing `CONFIG`
directory, absent `CONFIG`, entry crossing a cluster boundary, FAT mirror
consistency), plus a round-trip against the **real HAOS image** the flasher
already downloads and verifies — inject, then re-read the volume and confirm
the file is exactly the bytes `wifi_keyfile()` produced.

**Keep 0.3.6's posture:** a Wi-Fi that was requested and could not be injected
still fails the flash, with the reason. Injection makes that check cheaper and
earlier; it does not make it optional.

**Carry or drop the account seed:** the EXPERIMENTAL Home Assistant account
pre-seed (`CONFIG/.storage/…`) uses the same mount path. Either move it to
injection with the Wi-Fi or leave it behind deliberately — and say which, in
the PR, rather than letting it quietly keep the mount code alive.

## The general lesson

This is the same shape as several other bugs found the same evening: **a step
that can silently no-op will eventually no-op silently.** 0.3.6 fixed the
silence. This fixes the step.

And the deeper one: when two halves of a product solve the same problem and one
of them never fails, copy the one that works. The ESP32 path had the answer
already.
