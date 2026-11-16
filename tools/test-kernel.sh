#!/bin/sh
# Offline test of scripts/kernel.sh on the console's own userland (ROOTFS1 under ARMv7
# emulation, docker image psc-rootfs:dump), with files standing in for the partitions:
#   BOOTIMG2  Sony's stock boot.img (from a LRECOVERY.EPB backup: md5 28ce5f…)
#   BOOTIMG1  Project Eris v47's kernel image, padded to 8 MiB
#   tools/test-kernel.sh <LRECOVERY.EPB> <project_eris_kernel_v47.img>
set -e
EPB=$1; ERIS=$2
[ -f "$EPB" ] && [ -f "$ERIS" ] || { echo "usage: $0 <LRECOVERY.EPB> <project_eris_kernel_v47.img>"; exit 2; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"
python3 -c "import zipfile,sys; open(sys.argv[2],'wb').write(zipfile.ZipFile(sys.argv[1]).read('boot.img'))" "$EPB" "$W/stock.img"
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(d + bytes(8388608 - len(d)))" "$ERIS" "$W/eris.img"
cp "$HERE/scripts/kernel.sh" "$W/"
docker run --rm --platform linux/arm/v7 -v "$W":/w psc-rootfs:dump /bin/sh -c '
export PATH=/bin:/sbin:/usr/bin:/usr/sbin LUMEN_HOME=/tmp/stick/lumen KERNEL_BOOT1=/tmp/b1 KERNEL_BOOT2=/tmp/b2
K="sh /w/kernel.sh"; pass=0; bad=0
ok() { if eval "$2"; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }
m() { md5sum < "$1" | cut -d" " -f1; }
reset() { rm -rf /tmp/stick; mkdir -p /tmp/stick/lumen; cp /w/eris.img /tmp/b1; cp /w/stock.img /tmp/b2; }
S=$(m /w/stock.img); E=$(m /w/eris.img)
echo "stock $S   eris(padded) $E"
reset
out=$($K status); ok "status: BOOTIMG2 recognised as stock" "echo \"\$out\" | grep -q boot2=stock"
out=$($K restore-stock); ok "restore-stock succeeds" "echo \"\$out\" | grep -q \"^ok \""
ok "  BOOTIMG1 now = stock" "[ \$(m /tmp/b1) = $S ]"
ok "  backup of the previous kernel kept and verified" "[ \$(m /tmp/stick/lumen/backup/kernel/BOOTIMG1-$E.img) = $E ]"
ok "  steps reported in order" "[ \"\$(echo \"\$out\" | grep ^step | tr \"\\n\" \" \")\" = \"step backup step write step verify \" ]"
out=$($K restore-stock || true); ok "restore-stock again: refused (same)" "echo \"\$out\" | grep -q \"error same\""
out=$($K restore-backup /tmp/stick/lumen/backup/kernel/BOOTIMG1-$E.img); ok "restore-backup: back to the previous kernel" "[ \$(m /tmp/b1) = $E ] && echo \"\$out\" | grep -q \"^ok \""
ok "  that also backed up the stock kernel first" "[ -f /tmp/stick/lumen/backup/kernel/BOOTIMG1-$S.img ]"
reset; out=$(KERNEL_FAULT=write $K restore-stock || true)
ok "fault in the write: reported (write), previous kernel written back" "echo \"\$out\" | grep -q \"error write\" && [ \$(m /tmp/b1) = $E ]"
reset; out=$(KERNEL_FAULT=revert $K restore-stock || true)
ok "fault in the write and in the write-back: reported (revert)" "echo \"\$out\" | grep -q \"error revert\""
reset; out=$(KERNEL_FAULT=backup $K restore-stock || true)
ok "backup does not verify: nothing written" "echo \"\$out\" | grep -q \"error backup\" && [ \$(m /tmp/b1) = $E ] && ! ls /tmp/stick/lumen/backup/kernel/*.img >/dev/null 2>&1"
reset; printf X | dd of=/tmp/b2 bs=1 seek=5000 conv=notrunc 2>/dev/null; out=$($K restore-stock || true)
ok "BOOTIMG2 not stock: refused, nothing written" "echo \"\$out\" | grep -q \"error boot2\" && [ \$(m /tmp/b1) = $E ]"
reset; head -c 8388608 /dev/zero > /tmp/notkernel.img; out=$($K restore-backup /tmp/notkernel.img || true)
ok "restore-backup of a non-kernel file: refused" "echo \"\$out\" | grep -q \"error image\" && [ \$(m /tmp/b1) = $E ]"
reset; mkdir -p /tmp/stick/lumen/backup/kernel; printf junk > /tmp/stick/lumen/backup/kernel/BOOTIMG1-$E.img
out=$($K restore-stock); ok "a bad file with the backup name is never overwritten" "[ \"\$(cat /tmp/stick/lumen/backup/kernel/BOOTIMG1-$E.img)\" = junk ] && ls /tmp/stick/lumen/backup/kernel/BOOTIMG1-$E-*.img >/dev/null"
echo "passed $pass, failed $bad"; [ $bad = 0 ]'
rc=$?; rm -rf "$W"; exit $rc
