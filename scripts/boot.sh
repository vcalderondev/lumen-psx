#!/bin/sh
# Lumen boot entry for the PlayStation Classic.
#
# Called once at power-on, after the USB stick (label SONY) is mounted on /media
# and the console's own UI has been stopped. It must never simply return: the
# caller reboots the console when this script ends.
#
#  1. stop the Sony UI processes and make sure Weston (Wayland) is running
#  2. run a Lumen session (frontend <-> RetroArch loop, see run.sh)
#  3. when the session ends (Select+Start) or Lumen fails, hand the console to
#     the stock Sony menu so it always stays usable; the power button works there.
LUMEN_HOME="/media/lumen"
LOG="${LUMEN_HOME}/logs"
# POWER test path (<home>/debug/power-suspend). Sony's power manager unmounts the stick
# before its suspend to RAM, and after the resume the stick may be a new device: whatever
# has to live through a suspend must not run from the stick or hold a file open on it
# (a shell reading its script from it, a binary paged in from it, an open log). With the
# switch this script, run.sh, power.sh and lumen-buttons run from copies in RAM
# (/tmp/lumen-run) and log there; logsync.sh appends those logs to the stick. Without
# the switch nothing changes: everything runs from the stick and POWER is ignored.
RUN=/tmp/lumen-run
if [ -f "${LUMEN_HOME}/debug/power-suspend" ] && [ "${LUMEN_RAM}" != 1 ]; then
  rm -rf "${RUN}"; mkdir -p "${RUN}"
  if cp "${LUMEN_HOME}/boot.sh" "${LUMEN_HOME}/run.sh" "${LUMEN_HOME}/power.sh" "${LUMEN_HOME}/lumen-buttons" "${RUN}/" 2>/dev/null &&
     chmod 755 "${RUN}/lumen-buttons"; then
    LUMEN_RAM=1; export LUMEN_RAM
    cd /
    exec sh "${RUN}/boot.sh" "$@"
  fi
  rm -rf "${RUN}"   # no room or no files: the usual way, POWER stays ignored
fi
mkdir -p "${LOG}"
: > /tmp/lumen-usb-writers.pids   # processes that write to the stick (power.sh pauses them)
if [ "${LUMEN_RAM}" = 1 ]; then
  SCRIPTS="${RUN}"; BUTTONS_LOG="${RUN}/buttons.log"
  exec >> "${RUN}/boot.log" 2>&1
  # RAM logs -> the stick every 2 s, one open-append-close per file. The offset of a log
  # only moves after its new part was appended, so a pass cut short (stick gone, this
  # process paused by power.sh) is simply taken again
  cat > "${RUN}/logsync.sh" <<'LOGSYNC'
#!/bin/sh
L=/media/lumen/logs
while :; do
  for n in boot buttons suspend; do
    f=/tmp/lumen-run/$n.log
    [ -f "$f" ] || continue
    off=$(cat "$f.off" 2>/dev/null || echo 0); size=$(wc -c < "$f")
    [ "$size" -gt "$off" ] || continue
    grep -q ' /media ' /proc/mounts || continue
    if tail -c +$((off + 1)) "$f" | head -c $((size - off)) >> "$L/$n.log" 2>/dev/null; then echo "$size" > "$f.off"; fi
  done
  [ "$1" = once ] && exit 0
  sleep 2
done
LOGSYNC
  sh "${RUN}/logsync.sh" &
  echo $! >> /tmp/lumen-usb-writers.pids
else
  SCRIPTS="${LUMEN_HOME}"; BUTTONS_LOG="${LOG}/buttons.log"
  exec >> "${LOG}/boot.log" 2>&1
fi
echo "=== lumen boot $(date) ($(cut -d' ' -f1 /proc/uptime) s since power-on)"
# CPU / GPU frequency policy, for the record (read only): governor, current and
# available frequencies of cpu0, and of every devfreq device (the GPU's, if any)
for f in scaling_governor scaling_cur_freq scaling_min_freq scaling_max_freq scaling_available_governors scaling_available_frequencies; do
  [ -r "/sys/devices/system/cpu/cpu0/cpufreq/$f" ] && echo "cpu0 $f: $(cat "/sys/devices/system/cpu/cpu0/cpufreq/$f" 2>/dev/null)"
done
for d in /sys/class/devfreq/*; do
  [ -d "$d" ] || continue
  for f in governor cur_freq min_freq max_freq available_governors available_frequencies; do
    [ -r "$d/$f" ] && echo "devfreq $(basename "$d") $f: $(cat "$d/$f" 2>/dev/null)"
  done
done
[ -d /sys/class/devfreq ] || echo "devfreq: none (no /sys/class/devfreq)"

export XDG_RUNTIME_DIR=/run/user/0
export HOME=/home/root
cd /home/root

# 1. take over the display
killall -s KILL showLogo sonyapp ui_menu auto_dimmer pcsx 2>/dev/null

# Boot splash. The boot payload shows it (a process named sdl_display) on Sony's Weston
# before this script runs: the logo with its letters dark, which is also the first frame
# of Lumen's own splash, and "Loading" in the language of settings.ini. It stays up for
# as long as its compositor lives, so the screen is black only while the compositor is
# swapped: splash_off right before a compositor stops, splash_on once the session's
# compositor is up (the payload leaves /tmp/lumen-splash.sh to start it again; without
# it, an older payload, the screen stays black as before), and it goes for good when
# Lumen has drawn its own splash (below, before run.sh).
splash_off() { killall -s TERM sdl_display 2>/dev/null; }
splash_on() {  # 1080p | 720p
  [ -f /tmp/lumen-splash.sh ] || return 0
  pidof sdl_display >/dev/null 2>&1 && return 0
  sh /tmp/lumen-splash.sh "$1" > /tmp/lumen-splash.out 2>&1 &
}

# Output resolution: Sony's weston.ini forces HDMI 1280x720. With display=1080p in
# settings.ini, Weston is restarted with a copy of that config set to 1920x1080
# (/tmp, nothing on the eMMC changes). The console's HDMI driver reads no EDID (the
# connector has no mode list), so a plain "1920x1080" is ignored and Weston keeps its
# built-in 720p: the mode is given as an explicit CEA-861 modeline (1080p60, 148.5 MHz).
# If Weston does not come up, or does not report 1920x1080 as the current mode,
# back to the stock 720p.
MODE_1080P="148.50 1920 2008 2052 2200 1080 1084 1089 1125 +hsync +vsync"
MODE_720P="74.25 1280 1390 1430 1650 720 725 730 750 +hsync +vsync"   # CEA-861 720p60
LUMEN_WESTON=""
# Compositor: the PSC compositor built from source by psc-platform, psc-weston-16
# (Wayland 1.26 + Weston 16.0.2). Not a user choice; Sony's Weston 1.11 is only the
# fallback (RetroArch then runs with its KMS context, see lumen.conf), or forced by hand
# with compositor=stock in settings.ini. The
# package is unpacked to RAM and overlaid on /usr (overlayfs: nothing on the eMMC
# changes, gone at power-off).
PW_VER=""; PW_ON=""; PW_SAMPLER=""
# Guard: <home>/compositor/.trying holds the version being tried and is removed once the
# frontend has presented frames. Still there at boot = that attempt never showed a
# picture (crash, black screen, power-off): it becomes .failed-<version> and that
# version is skipped from then on (the installer clears it, or delete it by hand).
PW_MARK="${LUMEN_HOME}/compositor/.trying"
pw_tar() {  # version -> package path
  [ -f "${LUMEN_HOME}/compositor/psc-weston-$1.tar.gz" ] && echo "${LUMEN_HOME}/compositor/psc-weston-$1.tar.gz"
}
# stop Weston politely first: Weston 16's libseat gives the VT back (text mode, keyboard)
# only when it exits normally; a SIGKILL leaves it in graphics mode
pw_stop_weston() {
  killall -s TERM weston 2>/dev/null
  i=0; while [ $i -lt 30 ] && pidof weston >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
  killall -s KILL weston 2>/dev/null; sleep 0.5
}
psc_weston_off() {
  [ -n "${PW_ON}" ] || return 0
  [ -n "${PW_SAMPLER}" ] && kill "${PW_SAMPLER}" 2>/dev/null
  pw_stop_weston
  if umount -l /usr; then rm -rf /tmp/pw_upper /tmp/pw_work; echo "psc compositor overlay removed"
  else echo "psc compositor: umount /usr failed"; fi
  PW_ON=""; LUMEN_WESTON=""
}
weston_stock() {
  psc_weston_off
  [ -n "${LUMEN_WESTON}" ] && { splash_off; killall -s KILL weston 2>/dev/null; sleep 1; LUMEN_WESTON=""; }
  if ! systemctl status weston.service | grep -qE 'active\s+\(running\)'; then
    echo "starting weston (stock)"
    splash_off
    systemctl restart weston.service
  fi
}
weston_up() {  # seconds: wait for the Wayland socket
  i=0
  while [ $i -lt $(( $1 * 10 )) ]; do [ -S "${XDG_RUNTIME_DIR}/wayland-0" ] && return 0; sleep 0.1; i=$((i + 1)); done
  return 1
}
weston_custom() {  # log file, extra weston args...: start Weston with /tmp/lumen-weston.ini
  wlog=$1; shift
  splash_off
  systemctl stop weston.service
  killall -s KILL weston 2>/dev/null
  rm -f "${XDG_RUNTIME_DIR}/wayland-0" "${XDG_RUNTIME_DIR}/wayland-0.lock"
  /usr/bin/weston-launch -u root -- --idle-time=4294967 --tty=1 --config=/tmp/lumen-weston.ini "$@" \
    > "${wlog}" 2>&1 &
  LUMEN_WESTON=$!
}
setting() { sed -n "s/^$1=//p" "${LUMEN_HOME}/settings.ini" 2>/dev/null | tr -d '\r' | tail -1; }
DISPLAY_MODE=$(setting display)
COMPOSITOR=$(setting compositor)
[ -f /etc/xdg/weston/weston.ini ] || { DISPLAY_MODE=720p; COMPOSITOR=""; }
if [ "${DISPLAY_MODE}" = 1080p ]; then
  sed "/^name=HDMI-A-1/{n;s/^mode=.*/mode=${MODE_1080P}/;}" /etc/xdg/weston/weston.ini > /tmp/lumen-weston.ini
else
  cp /etc/xdg/weston/weston.ini /tmp/lumen-weston.ini 2>/dev/null
fi
STARTED=""
# Weston 16 settings for the PSC: the shell key that hides the panel was renamed;
# the HDMI connector reports "disconnected" with no EDID at boot and no mode list, so the
# output is forced on and every mode is a full modeline (a plain "1280x720" finds nothing
# to match and Weston exits). pageflip-timeout: a lost page flip makes Weston exit
# instead of freezing. (Sony's 1.11 ignores these keys.)
[ "${DISPLAY_MODE}" = 1080p ] ||
  sed -i "/^name=HDMI-A-1/{n;s/^mode=.*/mode=${MODE_720P}/;}" /tmp/lumen-weston.ini
sed -i -e '/^panel-location=none/a panel-position=none' \
       -e '/^name=HDMI-A-1/a force-on=true' \
       -e '/^\[core\]/a pageflip-timeout=1500' /tmp/lumen-weston.ini

# start one PSC compositor version from its package; 0 = up with a mode set
try_psc_weston() {  # version package
  v=$1
  echo "compositor psc weston ${v} (${DISPLAY_MODE:-720p}): overlay from RAM"
  printf '%s\n' "${v}" > "${PW_MARK}"; sync
  # unpacked while Sony's Weston still shows the boot splash: the screen is black only
  # from here on, for the compositor swap itself
  echo "  $(date +%T) unpacking"
  mkdir -p /tmp/pw_upper/usr /tmp/pw_work/usr
  if ! tar xzpf "$2" -C /tmp/pw_upper/usr; then
    echo "psc compositor ${v}: unpack failed"; rm -rf /tmp/pw_upper /tmp/pw_work; return 1
  fi
  echo "  $(date +%T) stopping Sony's weston"
  splash_off
  systemctl stop weston.service; pw_stop_weston
  echo "  $(date +%T) mounting overlay"
  if ! mount -t overlay overlay -o lowerdir=/usr,upperdir=/tmp/pw_upper/usr,workdir=/tmp/pw_work/usr /usr; then
    echo "psc compositor ${v}: overlay failed"; rm -rf /tmp/pw_upper /tmp/pw_work; return 1
  fi
  echo "  $(date +%T) starting weston ${v}"
  PW_ON=1
  # stock Weston's PAM session (weston-launch) owned /run/user/0; launched directly,
  # nothing creates it, and Weston refuses to start without it
  mkdir -p "${XDG_RUNTIME_DIR}" && chmod 0700 "${XDG_RUNTIME_DIR}"
  # libseat's builtin backend takes the active VT (no --tty). Atomic KMS first
  # (psc-platform patch 0001), legacy KMS (no direct scanout) if it fails.
  chvt 1 2>/dev/null
  # scanout=0 in settings.ini (Settings > Display): everything through the GL renderer,
  # no client buffer straight on a KMS plane (default on: 1080p at ~59 fps)
  if [ "$(setting scanout)" = 0 ]; then export WESTON_FORCE_RENDERER=1; echo "weston: renderer only (scanout=0)"; else unset WESTON_FORCE_RENDERER; fi
  for kms in atomic legacy; do
    rm -f "${XDG_RUNTIME_DIR}/wayland-0" "${XDG_RUNTIME_DIR}/wayland-0.lock"
    if [ "${kms}" = legacy ]; then export WESTON_DISABLE_ATOMIC=1; else unset WESTON_DISABLE_ATOMIC; fi
    # --socket: Weston 16 picks wayland-1 by default; clients (SDL, RetroArch) look for wayland-0
    LIBSEAT_BACKEND=builtin /usr/bin/weston --backend=drm --shell=desktop --idle-time=0 \
      --socket=wayland-0 --config=/tmp/lumen-weston.ini --debug > "${LOG}/weston-psc.log" 2>&1 &
    LUMEN_WESTON=$!
    if psc_weston_ready; then echo "weston ${v}: ${kms} KMS"; break; fi
    echo "weston ${v} (${kms} KMS) did not come up: $(tail -4 "${LOG}/weston-psc.log" 2>/dev/null)"
    cp -f "${LOG}/weston-psc.log" "${LOG}/weston-psc-${kms}.log" 2>/dev/null
    pw_stop_weston; LUMEN_WESTON=""
  done
  [ -n "${LUMEN_WESTON}" ] || { psc_weston_off; return 1; }
  PW_VER=${v}
  echo "psc compositor up (${LUMEN_WESTON}): $(/usr/bin/weston --version 2>&1 | head -1)"
  grep -aiE 'video modes|, current|scanout|atomic|renderer:|enabled with head' "${LOG}/weston-psc.log" | head -8
  grep -aqE '^ +1920x1080@[0-9.]+.*, current' "${LOG}/weston-psc.log" || DISPLAY_MODE=720p
  # evidence: which plane each view lands on, sampled twice while the frontend runs
  ( sleep 14; [ -x /usr/bin/weston-debug ] || exit 0
    /usr/bin/weston-debug drm-backend > "${LOG}/weston-planes.log" 2>&1 & d=$!; sleep 2; kill $d 2>/dev/null
    sleep 20; [ -x /usr/bin/weston-debug ] || exit 0
    /usr/bin/weston-debug drm-backend >> "${LOG}/weston-planes.log" 2>&1 & d=$!; sleep 1; kill $d 2>/dev/null ) &
  PW_SAMPLER=$!
  return 0
}
psc_weston_ready() {  # socket up, process alive, an output mode set
  weston_up 8 && sleep 1 && kill -0 "${LUMEN_WESTON}" 2>/dev/null &&
    grep -aqE '^ +[0-9]+x[0-9]+@[0-9.]+.*, current' "${LOG}/weston-psc.log"
}

if [ "${COMPOSITOR}" != stock ]; then
  if [ -f "${PW_MARK}" ]; then   # the last attempt never showed a picture
    v=$(head -1 "${PW_MARK}" | tr -dc 0-9)
    [ -n "${v}" ] && mv -f "${PW_MARK}" "${LUMEN_HOME}/compositor/.failed-${v}" &&
      echo "psc compositor ${v}: the last attempt showed no picture, skipped from now on"
    rm -f "${PW_MARK}"; sync
  fi
  for v in 16; do
    [ -n "${STARTED}" ] && break
    tar=$(pw_tar "${v}") || continue
    [ -f "${LUMEN_HOME}/compositor/.failed-${v}" ] && { echo "psc compositor ${v}: skipped (.failed-${v})"; continue; }
    try_psc_weston "${v}" "${tar}" && STARTED=psc
  done
fi
if [ -z "${STARTED}" ] && [ "${DISPLAY_MODE}" = 1080p ]; then
  echo "display 1080p: restarting stock weston at 1920x1080"
  weston_custom "${LOG}/weston.log"
  if weston_up 6 && sleep 1 && grep -aq 'mode 1920x1080.*current' "${LOG}/weston.log"; then
    echo "weston 1080p up (${LUMEN_WESTON}): $(grep -a 'mode 1920x1080' "${LOG}/weston.log" | head -1)"
    STARTED=stock1080
  else
    echo "weston 1080p failed: $(grep -aE '^ +mode |failed to set mode|atomic commit failed' "${LOG}/weston.log" | head -3), back to 720p"
    DISPLAY_MODE=720p
  fi
fi
[ -n "${STARTED}" ] || { weston_stock; DISPLAY_MODE=720p; }
export LUMEN_DISPLAY="${DISPLAY_MODE:-720p}"
splash_on "${LUMEN_DISPLAY}"   # back on the session's compositor (still up if it never stopped)
echo "compositor ready: $(cut -d' ' -f1 /proc/uptime) s since power-on"
case "${STARTED}" in psc) export LUMEN_COMPOSITOR="weston${PW_VER}" ;; *) export LUMEN_COMPOSITOR=stock ;; esac
# how to start the compositor again after a suspend (power.sh: the PowerVR / display
# driver cannot suspend under a running GL compositor, so it is stopped first)
{ echo '#!/bin/sh'
  echo "export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR}"
  echo 'rm -f "${XDG_RUNTIME_DIR}/wayland-0" "${XDG_RUNTIME_DIR}/wayland-0.lock"'
  case "${STARTED}" in
    psc)
      echo 'chvt 1 2>/dev/null'
      [ -n "${WESTON_FORCE_RENDERER}" ] && echo 'export WESTON_FORCE_RENDERER=1'
      [ -n "${WESTON_DISABLE_ATOMIC}" ] && echo 'export WESTON_DISABLE_ATOMIC=1'
      echo "LIBSEAT_BACKEND=builtin /usr/bin/weston --backend=drm --shell=desktop --idle-time=0 --socket=wayland-0 --config=/tmp/lumen-weston.ini > ${LOG}/weston-psc.log 2>&1 &" ;;
    stock1080)
      echo "/usr/bin/weston-launch -u root -- --idle-time=4294967 --tty=1 --config=/tmp/lumen-weston.ini > ${LOG}/weston.log 2>&1 &" ;;
    *) echo 'systemctl restart weston.service' ;;
  esac
  echo 'i=0; while [ $i -lt 100 ] && [ ! -S "${XDG_RUNTIME_DIR}/wayland-0" ]; do sleep 0.1; i=$((i + 1)); done; sleep 0.5'
} > /tmp/lumen-weston-start.sh
# Sony's power manager runs /usr/bin/resume_run after every resume: it starts Sony's
# Weston and menu ("An error has occurred" over Lumen). For this session it runs Lumen's
# resume instead (bind mount from RAM, nothing on the eMMC changes), and it waits up to
# 15 s (default 5) for apps_okay: closing a game with its save takes a few seconds.
printf '#!/bin/sh\n/usr/bin/cpu_mode full 2>/dev/null\nsh /tmp/lumen-power.sh resume resume_run\n' > /tmp/lumen-resume_run
chmod 755 /tmp/lumen-resume_run
mount --bind /tmp/lumen-resume_run /usr/bin/resume_run && echo "resume_run: Lumen's for this session"
for d in /dev/shm/power /data/power; do [ -d "$d" ] && echo 15 > "$d/apps_timeout"; done
/usr/bin/wait_sdl

# console buttons: POWER suspends (handshake with the system power manager),
# RESET quits a running game. Only hand POWER to the system if the daemon runs.
rm -f /data/power/prepare_suspend /data/power/apps_okay
BUTTONS_PID=""
# POWER (Sony's suspend): power.sh, run by lumen-buttons, pauses the processes that use
# the USB stick; it runs from RAM and the stick's writers are recorded for it
cp "${SCRIPTS}/power.sh" /tmp/lumen-power.sh 2>/dev/null
if [ -x "${SCRIPTS}/lumen-buttons" ] || chmod +x "${SCRIPTS}/lumen-buttons" 2>/dev/null; then
  "${SCRIPTS}/lumen-buttons" --log "${BUTTONS_LOG}" > /tmp/lumen-buttons.out 2>&1 &
  BUTTONS_PID=$!
  # up to 5 s: the first start from a cold FAT32 stick took longer than 1 s (POWER then
  # stayed disabled for the whole session)
  i=0; while [ $i -lt 50 ] && ! grep -q ready /tmp/lumen-buttons.out 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
fi
if [ -n "${BUTTONS_PID}" ] && grep -q ready /tmp/lumen-buttons.out 2>/dev/null; then
  # POWER: Sony's suspend, only with <home>/debug/power-suspend (test switch) until the
  # resume is verified on the console, and only when the session runs from RAM (above);
  # otherwise POWER is ignored
  if [ -f "${LUMEN_HOME}/debug/power-suspend" ] && [ "${LUMEN_RAM}" = 1 ]; then
    echo 1 > /data/power/disable      # power manager suspends after our apps_okay
    echo "POWER: suspend enabled (session in RAM: ${RUN})"
  else
    echo 2 > /data/power/disable      # ignored: unplug to turn off
    [ -f "${LUMEN_HOME}/debug/power-suspend" ] && echo "POWER: test switch present but the session is not in RAM: ignored"
  fi
  echo "buttons daemon running (${BUTTONS_PID})"
else
  echo 2 > /data/power/disable        # no daemon: keep POWER ignored
  echo "buttons daemon not running"
fi

# kernel messages to the USB, one write per line (the stick is exFAT over FUSE: a file's
# size is only committed when it is closed, so a long-open log would be lost if the
# kernel dies; each >> opens and closes it). Driver oopses before a crash land here.
# busybox dmesg has no -w: /proc/kmsg blocks until the next message (the ring buffer so
# far goes first, from dmesg)
dmesg > "${LOG}/kernel.log" 2>/dev/null
( while IFS= read -r kl; do printf '%s\n' "$kl" >> "${LOG}/kernel.log"; sync; done < /proc/kmsg ) &
KLOG_PID=$!
echo "${KLOG_PID}" >> /tmp/lumen-usb-writers.pids
# Sony's power manager (power_manage: POWER -> prepare_suspend / apps_okay -> suspend to
# RAM) logs to /tmp/power.log: copied to the USB line by line, for the POWER button
( tail -f /tmp/power.log 2>/dev/null | while IFS= read -r pl; do printf '%s %s\n' "$(date +%T)" "$pl" >> "${LOG}/power.log"; sync; done ) &
echo $! >> /tmp/lumen-usb-writers.pids

# 2. Lumen session
# the one-shot marker only guards the window until a frame is on screen: Lumen writes
# /tmp/lumen-presented after its first swap; a normal power-off later is not a failure
rm -f /tmp/lumen-presented /tmp/lumen-splash-shown /tmp/lumen-boot-splash
# The boot splash goes when Lumen's own is on screen (/tmp/lumen-splash-shown, written
# after its second frame), or after 30 s whatever happens. While it is up Lumen skips its
# fade-in from black (/tmp/lumen-boot-splash): the same picture is already there.
if [ -f /tmp/lumen-splash.sh ] || pidof sdl_display >/dev/null 2>&1; then
  : > /tmp/lumen-boot-splash
  ( i=0; while [ $i -lt 300 ] && [ ! -f /tmp/lumen-splash-shown ] && [ ! -f /tmp/lumen-presented ]; do sleep 0.1; i=$((i + 1)); done
    sleep 0.2; killall -s TERM sdl_display 2>/dev/null ) &
fi
if [ -f "${PW_MARK}" ]; then
  ( i=0; while [ $i -lt 120 ]; do
      [ -f /tmp/lumen-presented ] && { rm -f "${PW_MARK}"; sync; echo "psc compositor: first frame presented"; exit 0; }
      sleep 1; i=$((i + 1)); done ) &
fi
echo "starting the frontend: $(cut -d' ' -f1 /proc/uptime) s since power-on"
LUMEN_HOME="${LUMEN_HOME}" sh "${SCRIPTS}/run.sh"
SESSION_RC=$?
echo "session ended: ${SESSION_RC}"
sync
if [ -f /tmp/lumen-resume-failed ]; then
  # the stick did not come back after a suspend: nothing of Lumen can start (it lives on
  # the stick). A restart finds it again, or ends in the stock menu when it is gone
  echo "resume failed (no stick): restarting the console"
  sync; reboot; sleep 30
fi
if [ "${SESSION_RC}" = 12 ]; then   # Display settings changed: start over with the new setup
  echo "restart requested by the frontend"
  [ -n "${BUTTONS_PID}" ] && kill "${BUTTONS_PID}" 2>/dev/null
  sync; reboot; sleep 30
fi

# 3. stock Sony menu (internal games); power button handled by Sony's power manager
umount /usr/bin/resume_run 2>/dev/null   # Sony's resume again
[ -n "${BUTTONS_PID}" ] && kill "${BUTTONS_PID}" 2>/dev/null
echo "starting stock menu"
killall -s TERM sdl_display 2>/dev/null
weston_stock   # the Sony menu is laid out for 720p
/usr/bin/wait_sdl
rm -f /data/power/prepare_suspend /data/power/apps_okay
echo 1 > /data/power/disable
sync
cd /data/AppData/sony/pcsx
export PCSX_ESC_KEY=2
/usr/sony/bin/ui_menu --power-off-enable >> "${LOG}/ui_menu.log" 2>&1
echo "stock menu exited: $?"
sync
