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
mkdir -p "${LOG}"
exec >> "${LOG}/boot.log" 2>&1
echo "=== lumen boot $(date)"

export XDG_RUNTIME_DIR=/run/user/0
export HOME=/home/root
cd /home/root

# 1. take over the display
killall -s TERM sdl_display 2>/dev/null
killall -s KILL showLogo sonyapp ui_menu auto_dimmer pcsx 2>/dev/null
if ! systemctl status weston.service | grep -qE 'active\s+\(running\)'; then
  echo "starting weston"
  systemctl restart weston.service
fi
/usr/bin/wait_sdl

# console buttons: POWER suspends (handshake with the system power manager),
# RESET quits a running game. Only hand POWER to the system if the daemon runs.
rm -f /data/power/prepare_suspend /data/power/apps_okay
BUTTONS_PID=""
if [ -x "${LUMEN_HOME}/lumen-buttons" ] || chmod +x "${LUMEN_HOME}/lumen-buttons" 2>/dev/null; then
  "${LUMEN_HOME}/lumen-buttons" --log "${LOG}/buttons.log" > /tmp/lumen-buttons.out 2>&1 &
  BUTTONS_PID=$!
  for i in 1 2 3 4 5 6 7 8 9 10; do grep -q ready /tmp/lumen-buttons.out 2>/dev/null && break; sleep 0.1; done
fi
if [ -n "${BUTTONS_PID}" ] && grep -q ready /tmp/lumen-buttons.out 2>/dev/null; then
  echo 1 > /data/power/disable        # power manager suspends after our apps_okay
  echo "buttons daemon running (${BUTTONS_PID})"
else
  echo 2 > /data/power/disable        # no daemon: keep POWER ignored
  echo "buttons daemon not running"
fi

# 2. Lumen session
sh "${LUMEN_HOME}/run.sh"
echo "session ended: $?"

# 3. stock Sony menu (internal games); power button handled by Sony's power manager
[ -n "${BUTTONS_PID}" ] && kill "${BUTTONS_PID}" 2>/dev/null
echo "starting stock menu"
killall -s TERM sdl_display 2>/dev/null
rm -f /data/power/prepare_suspend /data/power/apps_okay
echo 1 > /data/power/disable
sync
cd /data/AppData/sony/pcsx
export PCSX_ESC_KEY=2
/usr/sony/bin/ui_menu --power-off-enable >> "${LOG}/ui_menu.log" 2>&1
echo "stock menu exited: $?"
sync
