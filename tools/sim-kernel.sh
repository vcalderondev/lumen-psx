#!/bin/sh
# A stand-in for scripts/kernel.sh in psc-sim: a console on Project Eris v47 whose restore
# goes through the three steps slowly enough to be seen. KSIM=fail: the write fails and is
# reverted. Nothing is written anywhere.
case "$1" in
  status) printf 'kernel=eris47\nboot1=56c733fbb2229172a95faf22cafb5cc0\nboot2=stock\nbackup=none\nuname=sim\n' ;;
  firmware-backup) for p in BOOTIMG1 BOOTIMG2 ROOTFS1 USRDATA; do echo "fw $p"; sleep 1; done; echo "firmware /media/lumen/backup/firmware" ;;
  restore-stock|restore-backup)
    echo "step firmware"; for p in BOOTIMG1 BOOTIMG2 ROOTFS1 USRDATA; do echo "fw $p"; sleep 1; done
    echo "step backup"; sleep 2; echo "backup /media/lumen/backup/kernel/BOOTIMG1-56c733fbb2229172a95faf22cafb5cc0.img"; echo "step write"; sleep 2
    if [ "${KSIM}" = fail ]; then echo "error write"; exit 1; fi
    echo "step verify"; sleep 1; echo "ok /media/lumen/backup/kernel/BOOTIMG1-56c733fbb2229172a95faf22cafb5cc0.img" ;;
esac
