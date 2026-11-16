# USB transfer

Copy files between the Lumen stick and a second USB drive with the controller, without a
computer. Made for sticks formatted as ext4, which Windows and macOS cannot write: put the
games on any FAT32 stick, plug it into the console's other port and copy them across.
Source: `src/transfer.c`; design: lumen-design `mockups/usb-transfer*.html`.

## Using it

Plug a second USB drive in while Lumen is on the Home. A pill above the help bar offers
the transfer for 8 seconds: **Square** opens it. Nothing opens by itself. It is also in
**Settings, Library, Transfer files** at any time.

The card has one pane per drive: the second USB on the left, the Lumen USB on the right
(opened at `games/`).

| Button | Does |
|---|---|
| Up / Down, L1 / R1 | move, a page at a time |
| Left / Right | change pane |
| Cross | open a folder; mark a file |
| Square | mark a file or a folder and step down |
| Start | copy what is marked (or the highlighted entry) to the other drive |
| Circle | up one folder; close at the top |
| Triangle | eject the second drive |

Start asks where to copy: the folder open in the other pane, or (towards the Lumen USB)
the games folder of any system declared in `systems/`, created if it does not exist. Then
the copy runs with a progress card; Circle asks before stopping it. When it ends the card
says how many files were copied; after a copy to the Lumen USB the library is rescanned,
so new games appear at once.

**Eject before pulling the drive out.** "Eject USB" (in the result, or Triangle) unmounts
it and says when it can be removed.

## What a copy does

- Folders are merged. A file already on the destination with the same size is skipped, so
  a copy that was stopped or interrupted can simply be started again. A file with another
  size is replaced.
- Free space is checked before anything is written (data, one block of slack per file and
  8 MB). FAT32 cannot hold a file of 4 GB or more: the copy says which one.
- Each file is written as `<name>.lumen-part`, flushed every 4 MB and at its end, and only
  then renamed, so a half file never has the name of a whole one. On an error, a stop or a
  lost drive the part is removed. The whole filesystem is synced before the result shows.
- Not copied: links and devices, `System Volume Information`, `$RECYCLE.BIN`, macOS's
  `._*`, `.DS_Store`, `.Spotlight-V100`, `.fseventsd`, `.Trashes`, `lost+found`.
- Nothing is ever deleted or moved.

## The second drive

- Detection: once a second, `/sys/block/sd*` is checked for a disk that is not the one
  Lumen runs from (found in `/proc/mounts`) and has media. Its first partition is used, or
  the disk itself when it has no partition table. Only the first such disk.
- Mount: read-only on `/tmp/lumen-usb2` with the system's `mount`, so whatever the console
  can mount works: FAT32 and ext2/3/4 with the stock kernel; exFAT and NTFS only on
  consoles that have the FUSE helpers (`/sbin/mount.exfat`, `/sbin/mount.ntfs`: those
  patched by Project Eris / BleemSync). A drive that cannot be mounted gets a toast.
  FAT names are mounted as UTF-8 unless the Lumen stick itself is FAT mounted with the
  kernel's default, so that names mean the same bytes on both.
- It becomes writable (`mount -o remount,rw`) only while it is the destination, and
  read-only again after the final sync. A FUSE mount cannot be flipped: it is mounted
  again writable and stays so until it is ejected.
- It is unmounted when it is ejected and when the frontend exits (a game starts, the
  console switches off). Back from a game it is mounted again without offering twice.
- If it disappears (pulled out, or the port's power dropped) a running copy stops at the
  next block with "The USB drive was disconnected", and the mount point is released.
- The Lumen stick is never unmounted, remounted or probed by any of this.

## Testing without hardware

| Variable | Effect |
|---|---|
| `LUMEN_USB2=<folder>` | the folder plays the second drive: "plugged in" while it exists |
| `LUMEN_USB2_FS=<type>` | filesystem it pretends to be (default `vfat`) |
| `LUMEN_USB2_KBPS=<n>` | limits the copy to n KB/s, to see the progress and stop it |

psc-sim: `LUMEN_SIM_ENV="LUMEN_USB2=/out/usb2 LUMEN_USB2_KBPS=3000" bin/psc-sim run lumen ...`
with the drive's contents in `sim/out/usb2`; rename the folder away during the run to
unplug it.
