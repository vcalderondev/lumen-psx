#!/bin/sh
# Wi-Fi and Bluetooth for Lumen, entirely from RAM: nothing on the console's eMMC changes,
# everything is gone at power-off. Run as root (Lumen runs as root).
#
#   net.sh caps                    wifi=<yes|load|no> bt=<yes|load|no> kernel=<stock|other>
#   net.sh wifi on|off|status|scan
#   net.sh wifi connect <ssid>     the password on stdin (empty: an open network)
#   net.sh wifi forget <ssid>
#   net.sh bt on|off|status
#   net.sh bt scan [seconds]       lines: <mac>\t<name>   (devices seen, paired or not)
#   net.sh bt devices              lines: <mac>\t<paired 0|1>\t<connected 0|1>\t<name>
#   net.sh bt pair <mac>           pair, trust, connect (controllers: no PIN)
#   net.sh bt remove <mac>
#   net.sh time                    set the clock over NTP (no RTC battery on the console)
#
# Pieces (psc-platform, in <home>/compositor/):
#   psc-net.tar.gz          wpa_supplicant, iw, curl, OpenSSL, ntpd, firmware -> /tmp/lumen-net
#   psc-bt.tar.gz           bluetoothd, bluetoothctl, GLib                    -> /tmp/lumen-bt
#   psc-netmod-stock.tar.gz mac80211 + Wi-Fi drivers, Bluetooth, USB Ethernet modules for
#                           Sony's stock kernel; loaded only on that exact build and only
#                           for what the running kernel lacks (any other kernel: what it has)
# The packages run from /tmp with their own LD_LIBRARY_PATH: the system's libraries stay.
# /etc is read-only: resolv.conf and the D-Bus policy are bind-mounted from /tmp.
# Kept on the stick (<home>/network/): wpa_supplicant.conf (networks, PSK hashes) and
# bluetooth.tar (pairings: BlueZ names files with ':', which FAT/exFAT refuse).
LUMEN_HOME="${LUMEN_HOME:-/media/lumen}"
PKG="${LUMEN_HOME}/compositor"
CONF="${LUMEN_HOME}/network"
NET=/tmp/lumen-net; BT=/tmp/lumen-bt; MOD=/tmp/lumen-netmod
RUN=/run/lumen-net
BTSTORE=/run/lumen-bt/lib/bluetooth          # BlueZ's compiled-in STORAGEDIR
LOGF="${LUMEN_HOME}/logs/net.log"
mkdir -p "${RUN}" "${CONF}" 2>/dev/null

log() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "${LOGF}" 2>/dev/null; }
die() { log "error: $*"; echo "error: $*" >&2; exit 1; }

# ---- kernel modules (stock build only, and only what is missing)
is_stock() { [ -f "${MOD}/netmod/KERNEL" ] && [ "$(uname -v)" = "$(cat "${MOD}/netmod/KERNEL")" ]; }
unpack_mod() {
  [ -d "${MOD}/netmod" ] && return 0
  [ -f "${PKG}/psc-netmod-stock.tar.gz" ] || return 1
  mkdir -p "${MOD}" && tar xzf "${PKG}/psc-netmod-stock.tar.gz" -C "${MOD}" || return 1
}
# load the modules of a set in load order, each once; the set lists what the feature needs
load_set() {  # module names...
  is_stock || return 1
  for m in $(cat "${MOD}/netmod/load-order.txt"); do
    case " $* " in *" $m "*) ;; *) continue ;; esac
    [ -d "/sys/module/$(echo "$m" | tr - _)" ] && continue
    if insmod "${MOD}/netmod/$m.ko" 2>>"${LOGF}"; then log "module: $m loaded"; else log "module: $m FAILED"; fi
  done
}
WIFI_SET="arc4 ecb ccm gcm ghash-generic cmac crc-ccitt mac80211 mt7601u rt2x00lib rt2800lib rt2x00usb rt2800usb rtl8xxxu
          libphy usbnet asix ax88179_178a cdc_ether cdc_ncm cdc_subset r8152 smsc95xx"
BT_SET="ecb cmac bluetooth btbcm btintel btrtl btusb hidp"
has_mod() { [ -d "/sys/module/$1" ]; }
cap_of() {  # module: yes (in the kernel), load (in our stock set), no
  if has_mod "$1"; then echo yes
  elif unpack_mod && is_stock && [ -f "${MOD}/netmod/$1.ko" ]; then echo load
  else echo no; fi
}

# ---- packages
unpack_pkg() {  # name dir
  [ -f "$2/.ok" ] && return 0
  [ -f "${PKG}/$1" ] || return 1
  rm -rf "$2" && mkdir -p "$2" && tar xzf "${PKG}/$1" -C "$2" && : > "$2/.ok"
}
netenv() { LD_LIBRARY_PATH="${NET}/lib" SSL_CERT_FILE="${NET}/lib/psc-net/ssl/cert.pem" \
           CURL_CA_BUNDLE="${NET}/lib/psc-net/ssl/cert.pem" "$@"; }
btenv() { LD_LIBRARY_PATH="${BT}/lib" "$@"; }

# ---- Wi-Fi
wlan_if() { for d in /sys/class/net/*; do [ -d "$d/wireless" ] || [ -e "$d/phy80211" ] && { basename "$d"; return 0; }; done; return 1; }
wpa_cli() { netenv "${NET}/sbin/wpa_cli" -p "${RUN}/wpa" -i "$(wlan_if)" "$@"; }
resolv_on() {
  [ -f "${RUN}/resolv.conf" ] || : > "${RUN}/resolv.conf"
  grep -q " /etc/resolv.conf " /proc/mounts || mount --bind "${RUN}/resolv.conf" /etc/resolv.conf
}
dhcp_script() {  # busybox udhcpc: address, route, DNS into the bind-mounted resolv.conf
  cat > "${RUN}/udhcpc.sh" <<'SH'
#!/bin/sh
case "$1" in
  deconfig) ip addr flush dev "$interface" 2>/dev/null; ip link set "$interface" up ;;
  bound|renew)
    ip addr flush dev "$interface"
    ip addr add "$ip/${mask:-24}" ${broadcast:+broadcast $broadcast} dev "$interface"
    [ -n "$router" ] && { ip route del default 2>/dev/null; for r in $router; do ip route add default via "$r" dev "$interface"; break; done; }
    : > /run/lumen-net/resolv.conf
    for d in $dns; do echo "nameserver $d" >> /run/lumen-net/resolv.conf; done
    echo "$ip" > /run/lumen-net/ip ;;
esac
SH
  chmod 755 "${RUN}/udhcpc.sh"
}
wifi_on() {
  unpack_pkg psc-net.tar.gz "${NET}" || die "psc-net.tar.gz missing"
  unpack_mod; load_set ${WIFI_SET}
  has_mod mac80211 || has_mod cfg80211 || die "this kernel has no Wi-Fi stack"
  # firmware from RAM (the drivers ask when a dongle is plugged in or the interface comes up)
  [ -w /sys/module/firmware_class/parameters/path ] && printf '%s' "${NET}/lib/firmware" > /sys/module/firmware_class/parameters/path
  i=0; while ! wlan_if >/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
  IF=$(wlan_if) || die "no Wi-Fi adapter (plug in a USB Wi-Fi dongle)"
  [ -f "${CONF}/wpa_supplicant.conf" ] || printf 'update_config=1\n' > "${CONF}/wpa_supplicant.conf"
  # the stick copy holds the networks; wpa_supplicant runs on a RAM copy with its control socket
  { echo "ctrl_interface=${RUN}/wpa"; grep -v '^ctrl_interface=' "${CONF}/wpa_supplicant.conf"; } > "${RUN}/wpa.conf"
  ip link set "${IF}" up
  pidof wpa_supplicant >/dev/null || netenv "${NET}/sbin/wpa_supplicant" -B -D nl80211,wext -i "${IF}" \
      -c "${RUN}/wpa.conf" -P "${RUN}/wpa.pid" -f "${RUN}/wpa.log" || die "wpa_supplicant did not start"
  resolv_on; dhcp_script
  [ -f "${RUN}/udhcpc.pid" ] && kill -0 "$(cat "${RUN}/udhcpc.pid")" 2>/dev/null ||
    udhcpc -b -i "${IF}" -s "${RUN}/udhcpc.sh" -p "${RUN}/udhcpc.pid" -t 0 -T 3 -A 5 >/dev/null 2>&1
  log "wifi on (${IF})"; echo "on ${IF}"
}
wifi_off() {
  [ -f "${RUN}/udhcpc.pid" ] && kill "$(cat "${RUN}/udhcpc.pid")" 2>/dev/null
  [ -f "${RUN}/wpa.pid" ] && kill "$(cat "${RUN}/wpa.pid")" 2>/dev/null
  IF=$(wlan_if) && { ip addr flush dev "${IF}"; ip link set "${IF}" down; }
  rm -f "${RUN}/ip" "${RUN}/udhcpc.pid" "${RUN}/wpa.pid"; log "wifi off"; echo off
}
wifi_status() {
  IF=$(wlan_if) || { echo "state=noadapter"; return 0; }
  [ -f "${RUN}/wpa.pid" ] && kill -0 "$(cat "${RUN}/wpa.pid")" 2>/dev/null || { echo "state=off"; return 0; }
  wpa_cli status 2>/dev/null | sed -n 's/^wpa_state=/state=/p; s/^ssid=/ssid=/p'
  echo "ip=$(cat "${RUN}/ip" 2>/dev/null)"
  wpa_cli signal_poll 2>/dev/null | sed -n 's/^RSSI=/signal=/p'
}
wifi_scan() {  # signal \t security \t ssid, strongest first, one line per name
  wpa_cli scan >/dev/null 2>&1; sleep 4
  wpa_cli scan_results 2>/dev/null | awk -F'\t' 'NR > 1 && $5 != "" {
      sec = ($4 ~ /SAE/) ? "wpa3" : ($4 ~ /WPA/) ? "wpa" : ($4 ~ /WEP/) ? "wep" : "open"
      if (!($5 in best) || $3 > best[$5]) { best[$5] = $3; s[$5] = sec } }
    END { for (n in best) printf "%s\t%s\t%s\n", best[n], s[n], n }' | sort -t"$(printf '\t')" -k1,1nr
}
# keep the stick copy in step with wpa_supplicant's (save_config writes the RAM copy)
wifi_save() { grep -v '^ctrl_interface=' "${RUN}/wpa.conf" > "${CONF}/wpa_supplicant.conf.tmp" &&
              mv -f "${CONF}/wpa_supplicant.conf.tmp" "${CONF}/wpa_supplicant.conf"; sync; }
wifi_connect() {  # ssid; password on stdin
  ssid=$1; [ -n "${ssid}" ] || die "no network name"
  IFS= read -r pass || true
  wifi_forget_ids "${ssid}"
  id=$(wpa_cli add_network | tail -1)
  hex=$(printf '%s' "${ssid}" | od -An -tx1 | tr -d ' \n')
  wpa_cli set_network "${id}" ssid "${hex}" >/dev/null
  if [ -z "${pass}" ]; then wpa_cli set_network "${id}" key_mgmt NONE >/dev/null
  else
    # WPA2 with the PSK hash (the password is never written); WPA3-SAE needs the
    # password itself, so SAE stays off unless the network offers nothing else
    psk=$(netenv "${NET}/sbin/wpa_passphrase" "${ssid}" "${pass}" | sed -n 's/^[[:space:]]*psk=\([0-9a-f]\{64\}\)$/\1/p')
    [ -n "${psk}" ] || die "invalid password (8 to 63 characters)"
    wpa_cli set_network "${id}" psk "${psk}" >/dev/null
    wpa_cli set_network "${id}" key_mgmt "WPA-PSK WPA-PSK-SHA256" >/dev/null
    wpa_cli set_network "${id}" ieee80211w 1 >/dev/null
  fi
  wpa_cli enable_network "${id}" >/dev/null; wpa_cli select_network "${id}" >/dev/null
  wpa_cli save_config >/dev/null; wifi_save
  i=0; while [ $i -lt 20 ]; do
    st=$(wpa_cli status 2>/dev/null | sed -n 's/^wpa_state=//p')
    [ "${st}" = COMPLETED ] && break; sleep 1; i=$((i + 1)); done
  [ "${st}" = COMPLETED ] || { log "wifi: ${ssid}: ${st}"; echo "failed ${st}"; return 1; }
  i=0; while [ ! -s "${RUN}/ip" ] && [ $i -lt 15 ]; do sleep 1; i=$((i + 1)); done
  log "wifi: connected to ${ssid} ($(cat "${RUN}/ip" 2>/dev/null))"; echo "connected $(cat "${RUN}/ip" 2>/dev/null)"
  ( time_sync ) >/dev/null 2>&1 &
}
wifi_forget_ids() {  # remove every saved network with this name
  hex=$(printf '%s' "$1" | od -An -tx1 | tr -d ' \n')
  for id in $(wpa_cli list_networks 2>/dev/null | awk -F'\t' 'NR > 1 {print $1}'); do
    s=$(wpa_cli get_network "${id}" ssid 2>/dev/null | tail -1)
    [ "${s}" = "\"$1\"" ] || [ "${s}" = "${hex}" ] && wpa_cli remove_network "${id}" >/dev/null
  done
}
wifi_forget() { wifi_forget_ids "$1"; wpa_cli save_config >/dev/null; wifi_save; echo forgotten; }

time_sync() {
  unpack_pkg psc-net.tar.gz "${NET}" || return 1
  "${NET}/lib/psc-net/busybox" ntpd -n -q -p pool.ntp.org -p time.google.com >/dev/null 2>&1 &&
    { log "time: $(date -u '+%F %T') UTC"; echo "$(date -u '+%F %T')"; }
}

# ---- Bluetooth
bt_ctl() { btenv "${BT}/bin/bluetoothctl" "$@" 2>/dev/null; }
bt_save() { [ -d "${BTSTORE}" ] && tar cf "${CONF}/bluetooth.tar.tmp" -C "$(dirname "${BTSTORE}")" bluetooth &&
            mv -f "${CONF}/bluetooth.tar.tmp" "${CONF}/bluetooth.tar" && sync; }
bt_on() {
  unpack_pkg psc-bt.tar.gz "${BT}" || die "psc-bt.tar.gz missing"
  unpack_pkg psc-net.tar.gz "${NET}"   # firmware of Bluetooth dongles (rtl_bt/, intel/, ath3k)
  unpack_mod; load_set ${BT_SET}
  has_mod bluetooth || die "this kernel has no Bluetooth stack"
  [ -w /sys/module/firmware_class/parameters/path ] && [ -d "${NET}/lib/firmware" ] &&
    printf '%s' "${NET}/lib/firmware" > /sys/module/firmware_class/parameters/path
  # pairings back from the stick
  mkdir -p "${BTSTORE}"; [ -f "${CONF}/bluetooth.tar" ] && tar xf "${CONF}/bluetooth.tar" -C "$(dirname "${BTSTORE}")"
  chmod 700 "${BTSTORE}"
  # D-Bus policy: the bus reads /usr/share/dbus-1/system.d (empty on the console)
  grep -q " /usr/share/dbus-1/system.d " /proc/mounts ||
    mount --bind "${BT}/share/dbus-1/system.d" /usr/share/dbus-1/system.d
  dbus-send --system --type=method_call --dest=org.freedesktop.DBus / org.freedesktop.DBus.ReloadConfig 2>/dev/null
  pidof bluetoothd >/dev/null || { btenv "${BT}/libexec/bluetooth/bluetoothd" -n -f "${BT}/lib/psc-bt/main.conf" \
      >> "${LUMEN_HOME}/logs/bluetoothd.log" 2>&1 & }
  i=0; while [ $i -lt 30 ]; do bt_ctl show 2>/dev/null | grep -q "Powered:" && break; sleep 0.2; i=$((i + 1)); done
  bt_ctl show | grep -q "Controller" || die "no Bluetooth adapter (plug in a USB Bluetooth dongle)"
  bt_ctl power on >/dev/null; bt_ctl pairable on >/dev/null
  log "bt on"; echo on
}
bt_off() { bt_ctl power off >/dev/null; bt_save; killall bluetoothd 2>/dev/null; log "bt off"; echo off; }
bt_status() {
  pidof bluetoothd >/dev/null || { echo "state=off"; return 0; }
  bt_ctl show | sed -n 's/^\s*Powered: \(.*\)/powered=\1/p; s/^Controller \([0-9A-F:]*\).*/adapter=\1/p'
}
bt_scan() {  # seconds
  bt_ctl --timeout "${1:-10}" scan on >/dev/null
  bt_ctl devices | sed -n 's/^Device \([0-9A-F:]\{17\}\) \(.*\)$/\1\t\2/p'
}
bt_devices() {
  bt_ctl devices | sed -n 's/^Device \([0-9A-F:]\{17\}\) \(.*\)$/\1 \2/p' | while read -r mac name; do
    info=$(bt_ctl info "${mac}")
    p=$(echo "${info}" | grep -q "Paired: yes" && echo 1 || echo 0)
    c=$(echo "${info}" | grep -q "Connected: yes" && echo 1 || echo 0)
    printf '%s\t%s\t%s\t%s\n' "${mac}" "$p" "$c" "${name}"
  done
}
bt_pair() {  # mac: controllers pair with no PIN (NoInputNoOutput agent)
  mac=$1; echo "${mac}" | grep -qE '^([0-9A-F]{2}:){5}[0-9A-F]{2}$' || die "bad address ${mac}"
  bt_ctl --agent NoInputNoOutput --timeout 25 pair "${mac}" | grep -qiE "Pairing successful|Already|AlreadyExists" ||
    { bt_ctl info "${mac}" | grep -q "Paired: yes" || { log "bt: pair ${mac} failed"; echo failed; return 1; }; }
  bt_ctl trust "${mac}" >/dev/null; bt_ctl --timeout 10 connect "${mac}" >/dev/null
  bt_save; log "bt: paired ${mac}"; echo paired
}
bt_remove() { bt_ctl remove "$1" >/dev/null; bt_save; log "bt: removed $1"; echo removed; }

case "$1 $2" in
  "caps "*) unpack_mod
            k=other; is_stock && k=stock
            echo "wifi=$(cap_of mac80211) bt=$(cap_of bluetooth) kernel=$k" ;;
  "wifi on") wifi_on ;;       "wifi off") wifi_off ;;
  "wifi status") wifi_status ;; "wifi scan") wifi_scan ;;
  "wifi connect") wifi_connect "$3" ;; "wifi forget") wifi_forget "$3" ;;
  "bt on") bt_on ;;           "bt off") bt_off ;;         "bt status") bt_status ;;
  "bt scan") bt_scan "$3" ;;  "bt devices") bt_devices ;;
  "bt pair") bt_pair "$3" ;;  "bt remove") bt_remove "$3" ;;
  "time "*) time_sync ;;
  *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
