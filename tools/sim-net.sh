#!/bin/sh
# A stand-in for scripts/net.sh in psc-sim (no adapters, no root there): answers every
# command the way the real one does, with example networks and controllers, so the
# Connections screens can be driven and screenshot.
#   LUMEN_SIM_ENV="LUMEN_NET_SH=/src/tools/sim-net.sh" bin/psc-sim run lumen ...
# LUMEN_SIM_REL=<dir> serves a release for the software update (release.json, the zip...).
# SIM_BT_LATE=<s> makes "bt on" fail for the first <s> seconds (a dongle plugged in late).
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
  # SIM_BT_LATE=<s>: the Bluetooth dongle "is plugged in" only <s> seconds after the first call
  # (bt on fails until then, like a console switched on before its dongle)
  "bt on") [ -f "$S/t0" ] || date +%s > "$S/t0"
           if [ -n "${SIM_BT_LATE}" ] && [ $(( $(date +%s) - $(cat "$S/t0") )) -lt "${SIM_BT_LATE}" ]; then
             echo "error: no Bluetooth adapter (plug in a USB Bluetooth dongle)" >&2; exit 1; fi
           echo on ;;
  "bt off") echo off ;;
  "bt scan") sleep 2 ;;
  "bt devices")
    while read -r m; do
      case "$m" in A0:5A:5C:11:22:33) printf '%s\t1\t1\tDualSense Wireless Controller\tinput-gaming\n' "$m" ;;
                   E4:17:D8:44:55:66) printf '%s\t1\t0\tPro Controller\tinput-gaming\n' "$m" ;;
                   *) printf '%s\t1\t1\tWireless Controller\tinput-gaming\n' "$m" ;; esac
    done < "$S/paired"
    grep -q 4C:B9:9B "$S/paired" || printf '4C:B9:9B:77:88:99\t0\t0\tWireless Controller\tinput-gaming\n'
    grep -q 00:1A:7D "$S/paired" || printf '00:1A:7D:12:34:56\t0\t0\tWH-1000XM4\taudio-headset\n'
    grep -q 34:88:5D "$S/paired" || printf '34:88:5D:AB:CD:EF\t0\t0\tK380 Keyboard\tinput-keyboard\n'
    grep -q E4:17:D8:AA "$S/paired" || printf 'E4:17:D8:AA:BB:CC\t0\t0\t8BitDo SN30 Pro\tinput-gaming\n' ;;
  "bt pair") sleep 2; echo "$3" >> "$S/paired"; echo paired ;;
  "bt remove") grep -v "$3" "$S/paired" > "$S/p2"; mv "$S/p2" "$S/paired"; echo removed ;;
  "time "*) date -u '+%F %T' ;;
  # software update: releases from $LUMEN_SIM_REL when set (release.json for lpsx.dev,
  # other files by name; offline), else the host's curl. Same contract as net.sh http.
  "http "*) rm -f "$3"
            if [ -n "${LUMEN_SIM_REL}" ]; then
              case "$2" in https://lpsx.dev/services/releases) src="${LUMEN_SIM_REL}/release.json" ;; *) src="${LUMEN_SIM_REL}/${2##*/}" ;; esac
              [ -f "${src}" ] || exit 1
              if [ -n "$4" ]; then f=${4%-*}; t=${4#*-}; tail -c +$((f + 1)) "${src}" | head -c $((t - f + 1)) > "$3"; else cp "${src}" "$3"; fi
            elif [ -n "$4" ]; then curl -fsSL -r "$4" -o "$3" "$2" || { rm -f "$3"; exit 1; }
              [ "$(wc -c < "$3")" -eq $(( ${4#*-} - ${4%-*} + 1 )) ] || { rm -f "$3"; exit 1; }
            else curl -fsSL -o "$3" "$2" || { rm -f "$3"; exit 1; }; fi
            echo ok ;;
  *) exit 2 ;;
esac
