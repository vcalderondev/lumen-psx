#!/bin/sh
# A stand-in for scripts/net.sh in psc-sim (no adapters, no root there): answers every
# command the way the real one does, with example networks and controllers, so the
# Connections screens can be driven and screenshot.
#   LUMEN_SIM_ENV="LUMEN_NET_SH=/src/tools/sim-net.sh" bin/psc-sim run lumen ...
# State between calls lives in /tmp/sim-net (connected network, paired controllers).
S=/tmp/sim-net; mkdir -p "$S"
[ -f "$S/ssid" ] || printf 'CasaPérez' > "$S/ssid"
[ -f "$S/paired" ] || printf 'A0:5A:5C:11:22:33\nE4:17:D8:44:55:66\n' > "$S/paired"
case "$1 $2" in
  "caps "*) echo "wifi=load bt=load eth=load kernel=stock" ;;
  "wifi on") echo "on wlan0" ;;
  "wifi off") echo off ;;
  "wifi status") printf 'state=COMPLETED\nssid=%s\nip=192.168.1.42\nsignal=-48\n' "$(cat "$S/ssid")" ;;
  "wifi scan") sleep 1; printf -- '-48\twpa\tCasaPérez\n-52\twpa3\tCasaPérez 5G\n-60\twpa\tOficina\n-66\twpa\tVecinos_2.4\n-78\topen\tCafé Central\n-82\twpa\tDIRECT-PRINTER\n' ;;
  "wifi saved") printf 'CasaPérez\nOficina\n' ;;
  "wifi connect"|"wifi select") read -r p; sleep 2; printf '%s' "$3" > "$S/ssid"; echo "connected 192.168.1.42" ;;
  "wifi forget") echo forgotten ;;
  "eth on") echo noadapter ;;
  "eth status") echo "state=noadapter" ;;
  "bt on") echo on ;;
  "bt off") echo off ;;
  "bt scan") sleep 2 ;;
  "bt devices")
    while read -r m; do
      case "$m" in A0:5A:5C:11:22:33) printf '%s\t1\t1\tDualSense Wireless Controller\n' "$m" ;;
                   E4:17:D8:44:55:66) printf '%s\t1\t0\tPro Controller\n' "$m" ;;
                   *) printf '%s\t1\t1\tWireless Controller\n' "$m" ;; esac
    done < "$S/paired"
    grep -q 4C:B9:9B "$S/paired" || printf '4C:B9:9B:77:88:99\t0\t0\tWireless Controller\n'
    printf 'E4:17:D8:AA:BB:CC\t0\t0\t8BitDo SN30 Pro\n' ;;
  "bt pair") sleep 2; echo "$3" >> "$S/paired"; echo paired ;;
  "bt remove") grep -v "$3" "$S/paired" > "$S/p2"; mv "$S/p2" "$S/paired"; echo removed ;;
  "time "*) date -u '+%F %T' ;;
  *) exit 2 ;;
esac
