/* Built-in modules over net.c (docs/modules.md 4): wifi, ethernet, bluetooth.
 *
 * Each one allows its radio in net.c while it runs, so a module switched off leaves the
 * radio off without touching the user's own switch on the Network / Bluetooth page, and
 * puts its item in the status bar:
 *   wifi       wifi-0 .. wifi-3 by signal, while connected
 *   ethernet   ethernet, while the cable has an address
 *   bluetooth  always: bluetooth-off (faint: no adapter; else the radio is off), bluetooth
 *              faint with nothing connected, bright with a device (BlueZ's list, or a
 *              controller SDL has on the Bluetooth bus), "..." while one pairs */
#include <stdio.h>
#include <string.h>
#include "lumen.h"
#include "module.h"
#include "net.h"

/* the signal as the lit arcs of the glyph: RSSI in dBm (wpa_cli signal_poll) */
static const char *wifi_glyph(int rssi)
{
   if (rssi == 0) return "wifi-2";      /* connected, the driver does not report a level */
   if (rssi >= -60) return "wifi-3";
   if (rssi >= -70) return "wifi-2";
   if (rssi >= -80) return "wifi-1";
   return "wifi-0";
}

static const char *net_unavailable(void) { return net_supported() ? NULL : "mods.net_missing"; }

/* while any of the three runs, net.c keeps the status fresh for the status bar */
static void watch_update(void)
{
   net_status_watch(module_running("wifi") || module_running("ethernet") || module_running("bluetooth"));
}

/* ---- wifi ---- */
static const char *wifi_available(void)
{
   const char *k = net_unavailable();
   if (k) return k;
   return net_state()->wifi_cap == NET_CAP_NO ? "net.wifi_nokernel" : NULL;
}
static void wifi_start(void) { net_allow(NET_ALLOW_WIFI, 1); watch_update(); }
static void wifi_stop(void) { net_allow(NET_ALLOW_WIFI, 0); watch_update(); }
static void wifi_poll(double t)
{
   const net_state_t *ns = net_state();
   (void)t;
   if (ns->wifi_on && ns->wifi == NET_CONNECTED)
      mod_status_set("wifi", "signal", &(mod_status_t){ .glyph = wifi_glyph(ns->rssi), .order = 10 });
   else mod_status_clear("wifi", "signal");
}
const lumen_module_t mod_wifi = {
   .id = "wifi", .api = LUMEN_MODULE_API, .flags = MOD_F_DEFAULT_ON, .version = "1.0",
   .available = wifi_available, .start = wifi_start, .stop = wifi_stop, .poll = wifi_poll,
};

/* ---- ethernet ---- */
static void eth_start(void) { net_allow(NET_ALLOW_ETH, 1); watch_update(); }
static void eth_stop(void) { net_allow(NET_ALLOW_ETH, 0); watch_update(); }
static void eth_poll(double t)
{
   (void)t;
   if (net_state()->eth == NET_CONNECTED)
      mod_status_set("ethernet", "link", &(mod_status_t){ .glyph = "ethernet", .order = 11 });
   else mod_status_clear("ethernet", "link");
}
const lumen_module_t mod_ethernet = {
   .id = "ethernet", .api = LUMEN_MODULE_API, .flags = MOD_F_DEFAULT_ON, .version = "1.0",
   .available = net_unavailable, .start = eth_start, .stop = eth_stop, .poll = eth_poll,
};

/* ---- bluetooth ---- */
static const char *bt_available(void)
{
   const char *k = net_unavailable();
   if (k) return k;
   return net_state()->bt_cap == NET_CAP_NO ? "net.bt_nokernel" : NULL;
}
static void bt_start(void) { net_allow(NET_ALLOW_BT, 1); watch_update(); }
static void bt_stop(void) { net_allow(NET_ALLOW_BT, 0); watch_update(); }
/* controllers SDL has on the Bluetooth bus: the GUID starts with the bus type, little
 * endian (0x0005 Bluetooth, 0x0003 USB). They count as connected even before BlueZ's list
 * (net.c, refreshed every few seconds) has them */
static int bt_pads(void)
{
   int i, n = 0;
   for (i = 0; i < SDL_NumJoysticks(); i++)
   {
      SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
      if (g.data[0] == 0x05 && g.data[1] == 0x00) n++;
   }
   return n;
}
static void bt_poll(double t)
{
   const net_state_t *ns = net_state();
   static double next;
   static int pads;
   if (t >= next) { pads = bt_pads(); next = t + 1.0; }   /* SDL's list, once a second */
   /* the state of Bluetooth is this glyph (not a screen of its own): crossed out and faint
    * with no adapter, crossed out with the radio off, faint while on with nothing connected,
    * "..." while a controller pairs, bright with one connected */
   if (!ns->bt_present || ns->bt == NET_NOADAPTER)
      mod_status_set("bluetooth", "radio", &(mod_status_t){ .glyph = "bluetooth-off", .dim = 1, .order = 20 });
   else if (!ns->bt_on || ns->bt != NET_CONNECTED)
      mod_status_set("bluetooth", "radio", &(mod_status_t){ .glyph = "bluetooth-off", .order = 20 });
   else
      mod_status_set("bluetooth", "radio", &(mod_status_t){ .glyph = "bluetooth", .dim = ns->connected == 0 && pads == 0,
                                                             .badge = ns->ap_state == NET_AP_PAIRING ? "\xE2\x80\xA6" : NULL, .order = 20 });
}
const lumen_module_t mod_bluetooth = {
   .id = "bluetooth", .api = LUMEN_MODULE_API, .flags = MOD_F_DEFAULT_ON, .version = "1.0",
   .available = bt_available, .start = bt_start, .stop = bt_stop, .poll = bt_poll,
};
