#!/bin/sh
# The recovery path of a PlayStation Classic, from a Mac: Sony's bootloader has a fastboot
# mode (CyberCX, "PlayStation Classic Hacking", 2020: two pads on SIDE A of the board,
# next to the "LM-11" print, bridged with tweezers at power-on; the console's micro-USB
# to the computer powers it and carries fastboot; USB ID 0bb4:0c01 "MediaTek / Yocto").
#
#   tools/psc-fastboot.sh check                 is a console in fastboot? (read only)
#   tools/psc-fastboot.sh info                  getvar all -> psc-fastboot-<date>.txt (read only)
#   tools/psc-fastboot.sh flash-kernel <img>    write a kernel backup to BOOTIMG1 (8 MiB file
#                                               made by Lumen: lumen/backup/kernel/*.img or
#                                               lumen/backup/firmware/BOOTIMG1.img)
#   tools/psc-fastboot.sh reboot
#
# flash-kernel checks the image first (exactly 8 MiB, FIT magic d00dfeed, its .md5 or the
# firmware MANIFEST when there is one) and asks before writing. A drill that proves the
# path without changing anything: flash the backup of the kernel that is in BOOTIMG1 now.
set -e
FB="${FASTBOOT:-fastboot}"
command -v "$FB" >/dev/null || { echo "fastboot not found (brew install --cask android-platform-tools)"; exit 2; }
need_device() {
  n=$("$FB" devices 2>/dev/null | grep -c fastboot || true)
  [ "$n" = 1 ] || { echo "no console in fastboot mode ($n devices). Bridge the two SIDE A pads while plugging the micro-USB into the Mac."; exit 1; }
}
case "$1" in
  check) "$FB" devices -l; need_device; echo "OK: a device answers in fastboot mode." ;;
  info)
    need_device; out="psc-fastboot-$(date +%Y%m%d-%H%M%S).txt"
    "$FB" getvar all > "$out" 2>&1 || true
    for v in product partition-type:BOOTIMG1 partition-size:BOOTIMG1 partition-size:BOOTIMG2 unlocked secure max-download-size; do
      printf '%-28s %s\n' "$v" "$("$FB" getvar "$v" 2>&1 | sed -n "s/^$v: //p" | head -1)"
    done | tee -a "$out"
    echo "saved: $out" ;;
  flash-kernel)
    img=$2; [ -f "$img" ] || { echo "usage: $0 flash-kernel <8 MiB kernel image>"; exit 2; }
    size=$(stat -f %z "$img" 2>/dev/null || stat -c %s "$img")
    [ "$size" = 8388608 ] || { echo "not a kernel partition image: $size bytes, expected 8388608"; exit 1; }
    [ "$(head -c 4 "$img" | xxd -p)" = d00dfeed ] || { echo "not a kernel image: no FIT magic d00dfeed"; exit 1; }
    m=$(md5 -q "$img")
    if [ -f "$img.md5" ]; then [ "$(cut -d' ' -f1 "$img.md5")" = "$m" ] || { echo "$img does not match $img.md5"; exit 1; }; echo "md5 matches $img.md5"
    elif [ -f "$(dirname "$img")/MANIFEST" ]; then grep -q "^$m " "$(dirname "$img")/MANIFEST" || { echo "$img does not match MANIFEST"; exit 1; }; echo "md5 matches MANIFEST"
    else echo "no .md5 / MANIFEST next to it: md5 $m (check it yourself)"; fi
    need_device
    printf 'Write %s (md5 %s) to BOOTIMG1? Type yes: ' "$(basename "$img")" "$m"; read -r a
    [ "$a" = yes ] || { echo "nothing written"; exit 1; }
    "$FB" flash BOOTIMG1 "$img"
    echo "written. Next: $0 reboot" ;;
  reboot) need_device; "$FB" reboot ;;
  *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
