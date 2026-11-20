/* Connections: Wi-Fi, Bluetooth controllers and a USB Ethernet adapter, run by
 * scripts/net.sh (everything from RAM, see that script). A worker thread runs the script's
 * commands one at a time so the UI never waits for them; the UI reads a copy of the state
 * that net_poll() refreshes once per frame. Settings: wifi, bluetooth, net_time (0/1). */
#ifndef LUMEN_NET_H
#define LUMEN_NET_H

#define NET_MAX_APS  32
#define NET_MAX_DEVS 16

enum { NET_CAP_UNKNOWN, NET_CAP_NO, NET_CAP_YES };
/* Wi-Fi and Ethernet states */
enum { NET_OFF, NET_NOADAPTER, NET_DOWN, NET_CONNECTING, NET_CONNECTED, NET_NOCABLE };

typedef struct { char ssid[64]; int signal, saved, connected; char sec[8]; } net_ap_t;
typedef struct { char mac[18], name[64], icon[24]; int paired, connected; } net_dev_t;   /* icon: BlueZ's, "input-gaming" ... */

typedef struct {
   int  wifi_cap, bt_cap, eth_cap, stock_kernel;
   int  wifi_on, wifi;                    /* setting; NET_* */
   char ssid[64], ip[20];
   int  eth;                              /* NET_* */
   char eth_ip[20];
   int  bt_on, bt;                        /* setting; NET_OFF / NET_NOADAPTER / NET_CONNECTED (on) */
   int  time_on, time_ok;
   char time_text[24];                    /* "2026-11-16 21:14" UTC, when set over NTP */
   int  naps, scanning;                   /* Wi-Fi networks of the last scan */
   net_ap_t aps[NET_MAX_APS];
   double scan_age;                       /* now_s() of that scan */
   int  ndev, bt_scanning, paired, connected;
   net_dev_t dev[NET_MAX_DEVS];
   int  busy;                             /* a connect / pair is running */
   int  gen;                              /* changes whenever something above changed */
} net_state_t;

void net_init(void);                      /* after settings: capabilities, then what is switched on */
void net_poll(void);                      /* each frame: takes the worker's news */
const net_state_t *net_state(void);
const char *net_take_toast(void);         /* a tr key the worker wants shown, once */

/* what the UI is looking at: the status is refreshed every few seconds only then */
void net_watch(int connections_page, int wifi_card, int bt_card);

void net_wifi_set(int on);
void net_bt_set(int on);
void net_time_set(int on);
void net_wifi_scan(void);
void net_wifi_connect(const char *ssid, const char *password);   /* "" = open network */
void net_wifi_select(const char *ssid);   /* a saved network */
void net_wifi_forget(const char *ssid);
void net_bt_pair(const char *mac);        /* pair + trust + connect, or connect when paired */

void net_bt_remove(const char *mac);

#endif
