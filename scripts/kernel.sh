#!/bin/sh
# The console's kernel: see which one runs, and put Sony's stock kernel back in BOOTIMG1
# (or the one that was there before). The ONLY part of Lumen that writes to the console's
# internal memory, and only when the user confirms it in Settings, About, Kernel.
#
#   kernel.sh status         kernel=<stock|eris47|mmc2019|other> boot1=<md5> boot2=<stock|bad>
#                            backup=<newest backup file or none> uname=<uname -v>
#   kernel.sh restore-stock  back up BOOTIMG1 to the stick, then BOOTIMG2 (Sony's, kept
#                            intact by every console for recovery) -> BOOTIMG1
#   kernel.sh restore-backup <file>   write a backup made by restore-stock back to BOOTIMG1
#
# Progress on stdout, one line per step: "step backup|write|verify" ("backup <file>" once the
# copy is verified), then "ok <file>" or
# "error <code>" (and exit 1). Codes: boot2 (BOOTIMG2 is not Sony's), same (nothing to do),
# space, backup (the copy on the stick did not verify), image (a backup file is not a
# kernel partition), write (written but did not verify: the backup was written back and
# verified), revert (that write-back failed too: the console needs the recovery path).
#
# Gates, in order, before anything is written:
#   - BOOTIMG2 is byte for byte Sony's stock kernel partition (md5 28ce5f…)
#   - BOOTIMG1 differs from what is about to be written
#   - BOOTIMG1 copied to <home>/backup/kernel/BOOTIMG1-<md5>.img on the stick, synced, read
#     back and compared (an existing copy with that name is checked, never overwritten)
# Write: dd conv=fsync of the whole 8 MiB partition, then read back and compare. On a
# mismatch the backup is written back the same way at once and verified.
# The window where a power cut leaves no bootable BOOTIMG1 is the write itself (8 MiB, well
# under a second on the eMMC). Then the console needs the external recovery path with the
# backup above, which is why it is copied first and kept.
# Tests: KERNEL_BOOT1 / KERNEL_BOOT2 point at files instead of the partitions,
# KERNEL_STOCK_MD5 replaces Sony's md5, KERNEL_FAULT=write|revert|backup inject a failure.
LUMEN_HOME="${LUMEN_HOME:-/media/lumen}"
BOOT1="${KERNEL_BOOT1:-/dev/disk/by-partlabel/BOOTIMG1}"
BOOT2="${KERNEL_BOOT2:-/dev/disk/by-partlabel/BOOTIMG2}"
STOCK_MD5="${KERNEL_STOCK_MD5:-28ce5f6de4981764411393310311b517}"
SIZE=8388608
BK="${LUMEN_HOME}/backup/kernel"
LOGF="${LUMEN_HOME}/logs/kernel.log"
STOCK_UNAME="#1 SMP PREEMPT Sun Sep 9 15:21:14 UTC 2018"
ERIS47_UNAME="#1 SMP PREEMPT Sun Feb 23 19:09:17 UTC 2020"
MMC2019_UNAME="#1 SMP PREEMPT Fri Aug 16 08:38:35 UTC 2019"

mkdir -p "$(dirname "${LOGF}")" 2>/dev/null || LOGF=/dev/null   # every dd logs its errors here
log() { echo "$(date -u '+%F %T') $*" >> "${LOGF}" 2>/dev/null; }
fail() { log "error $1: $2"; echo "error $1"; exit 1; }
md5_of() { md5sum < "$1" 2>/dev/null | cut -d' ' -f1; }
# the first SIZE bytes (a partition reads as exactly that; a file may be shorter)
md5_part() { dd if="$1" bs=4096 count=$((SIZE / 4096)) 2>/dev/null | md5sum | cut -d' ' -f1; }

kernel_name() {
  case "$(uname -v)" in
    "${STOCK_UNAME}") echo stock ;; "${ERIS47_UNAME}") echo eris47 ;;
    "${MMC2019_UNAME}") echo mmc2019 ;; *) echo other ;;
  esac
}

# write a SIZE-byte image to BOOTIMG1 and check it; $1 image, $2 its md5
write_verify() {
  dd if="$1" of="${BOOT1}" bs=4096 count=$((SIZE / 4096)) conv=fsync,notrunc 2>>"${LOGF}" || return 1
  sync
  [ "${KERNEL_FAULT}" = write ] && { printf 'X' | dd of="${BOOT1}" bs=1 seek=100 conv=notrunc 2>/dev/null; KERNEL_FAULT=; }
  [ "${KERNEL_FAULT}" = revert ] && printf 'Y' | dd of="${BOOT1}" bs=1 seek=200 conv=notrunc 2>/dev/null   # both writes
  [ "$(md5_part "${BOOT1}")" = "$2" ]
}

# copy BOOTIMG1 to the stick and verify the copy; prints its path
backup_boot1() {
  m=$(md5_part "${BOOT1}"); f="${BK}/BOOTIMG1-${m}.img"
  mkdir -p "${BK}" || return 1
  if [ -f "${f}" ] && [ "$(md5_of "${f}")" = "${m}" ]; then echo "${f}"; return 0; fi   # already there and good
  [ -f "${f}" ] && f="${BK}/BOOTIMG1-${m}-$(date +%s).img"   # never overwrite a file
  dd if="${BOOT1}" of="${f}.part" bs=4096 count=$((SIZE / 4096)) 2>>"${LOGF}" || return 1
  sync
  [ "${KERNEL_FAULT}" = backup ] && printf 'Z' | dd of="${f}.part" bs=1 seek=300 conv=notrunc 2>/dev/null
  [ "$(md5_of "${f}.part")" = "${m}" ] || { rm -f "${f}.part"; return 1; }
  mv "${f}.part" "${f}" && echo "${m}  $(basename "${f}")" > "${f}.md5" && sync && echo "${f}"
}

# the image must look like a kernel partition: SIZE bytes and the FIT magic d00dfeed
check_image() {
  [ "$(stat -c %s "$1" 2>/dev/null)" = "${SIZE}" ] || return 1
  [ "$(dd if="$1" bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')" = d00dfeed ]
}

install_image() {  # image md5 what
  img=$1; want=$2
  # POWER waits while this exists (power.sh suspend); gone however this script ends
  : > /tmp/lumen-kernel-writing; trap 'rm -f /tmp/lumen-kernel-writing' EXIT INT TERM
  [ "$(md5_part "${BOOT1}")" = "${want}" ] && fail same "BOOTIMG1 already is ${3}"
  avail=$(df -k "${LUMEN_HOME}" 2>/dev/null | awk 'NR == 2 {print $4}')
  [ -n "${avail}" ] && [ "${avail}" -lt 20480 ] && fail space "${avail} KiB free on the stick"
  echo "step backup"
  bk=$(backup_boot1) || fail backup "the copy of BOOTIMG1 did not verify"
  log "backup ${bk} ($(md5_of "${bk}"))"
  echo "backup ${bk}"   # reported at once: if anything after fails, this file saves the console
  echo "step write"
  log "writing ${3} (${want}) to BOOTIMG1"
  if write_verify "${img}" "${want}"; then
    echo "step verify"; log "BOOTIMG1 = ${want}, verified"; echo "ok ${bk}"; return 0
  fi
  log "BOOTIMG1 did not verify after the write: writing the backup back"
  if write_verify "${bk}" "$(md5_of "${bk}")"; then fail write "write did not verify; the previous kernel was written back and verified"; fi
  fail revert "write did not verify and the write-back failed too: use the recovery path with ${bk}"
}

case "$1" in
  status)
    b2=bad; [ -e "${BOOT2}" ] && [ "$(md5_part "${BOOT2}")" = "${STOCK_MD5}" ] && b2=stock
    last=$(ls -t "${BK}"/BOOTIMG1-*.img 2>/dev/null | head -1)
    echo "kernel=$(kernel_name)"; echo "boot1=$(md5_part "${BOOT1}" 2>/dev/null)"; echo "boot2=${b2}"
    echo "backup=${last:-none}"; echo "uname=$(uname -v)" ;;
  restore-stock)
    [ "$(id -u)" = 0 ] || fail root "not root"
    [ "$(md5_part "${BOOT2}")" = "${STOCK_MD5}" ] || fail boot2 "BOOTIMG2 is not Sony's stock kernel partition"
    # from a RAM copy: the partition is not read twice while it is written elsewhere
    tmp=/tmp/lumen-bootimg2.img
    dd if="${BOOT2}" of="${tmp}" bs=4096 count=$((SIZE / 4096)) 2>>"${LOGF}" && [ "$(md5_of "${tmp}")" = "${STOCK_MD5}" ] ||
      { rm -f "${tmp}"; fail boot2 "BOOTIMG2 did not read back as Sony's"; }
    install_image "${tmp}" "${STOCK_MD5}" "Sony's stock kernel"; rc=$?
    rm -f "${tmp}"; exit ${rc} ;;
  restore-backup)
    [ "$(id -u)" = 0 ] || fail root "not root"
    f=$2; [ -f "${f}" ] || fail image "no such backup ${f}"
    check_image "${f}" || fail image "${f} is not a kernel partition image"
    m=$(md5_of "${f}")
    [ -f "${f}.md5" ] && [ "$(cut -d' ' -f1 "${f}.md5")" != "${m}" ] && fail image "${f} does not match its .md5"
    install_image "${f}" "${m}" "the backup $(basename "${f}")" ;;
  *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
