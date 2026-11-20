/* Connections worker, see net.h.
 *
 * One thread, one queue: every request becomes a job that runs scripts/net.sh with
 * fork/exec (no shell in between: a network name is an argument, never code; a password
 * goes in on stdin, never on a command line). Between jobs the worker refreshes the
 * status while the UI looks at it: every 3 s on the Connections page and the Wi-Fi card,
 * and on the Bluetooth card it keeps searching (bt scan) and listing the controllers.
 * <home>/net.sh is the script; LUMEN_NET_SH overrides it (psc-sim uses a fake one). */
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lumen.h"
#include "net.h"
#include "settings.h"

enum { J_CAPS, J_WIFI_ON, J_WIFI_OFF, J_WIFI_SCAN, J_WIFI_CONNECT, J_WIFI_SELECT, J_WIFI_FORGET, J_BT_ON, J_BT_OFF,
       J_BT_PAIR, J_BT_REMOVE, J_ETH_ON, J_TIME, J_COUNT };
typedef struct { int kind; char a[96]; char pass[72]; } job_t;

#define QMAX 16
static job_t        s_q[QMAX];
static int          s_qn;
static SDL_mutex   *s_mx;
static SDL_cond    *s_cv;
static net_state_t  w_st;              /* the worker's, under s_mx */
static net_state_t  s_ui;              /* the UI's copy */
static const char  *w_toast;           /* under s_mx */
static int          w_watch_page, w_watch_wifi, w_watch_bt;   /* under s_mx */
static int          w_watch_pad, w_autopair;                  /* under s_mx: net_pad_watch */
static char         s_script[300];
static int          s_started;

static void bump(void) { w_st.gen++; }

static void push(int kind, const char *a, const char *pass)
{
   int i;
   if (!s_started) return;
   SDL_LockMutex(s_mx);
   for (i = 0; i < s_qn; i++)   /* the same request twice in the queue: once is enough */
      if (s_q[i].kind == kind && !strcmp(s_q[i].a, a ? a : "") && kind != J_WIFI_CONNECT) { SDL_UnlockMutex(s_mx); return; }
   if (s_qn < QMAX)
   {
      job_t *j = &s_q[s_qn++];
      memset(j, 0, sizeof(*j));
      j->kind = kind;
      snprintf(j->a, sizeof(j->a), "%s", a ? a : "");
      snprintf(j->pass, sizeof(j->pass), "%s", pass ? pass : "");
   }
   SDL_CondSignal(s_cv);
   SDL_UnlockMutex(s_mx);
}

/* run "sh net.sh <args...>"; stdin_text goes in on stdin; the output (cut at n - 1) in out.
 * Returns the exit status, -1 when it could not run. */
static int run(const char *const *args, const char *stdin_text, char *out, size_t n)
{
   int po[2], pi[2], st = 0;
   pid_t pid;
   size_t got = 0;
   const char *argv[12];
   int k = 0, i;
   if (n) out[0] = 0;
   argv[k++] = "sh"; argv[k++] = s_script;
   for (i = 0; args[i] && k < 11; i++) argv[k++] = args[i];
   argv[k] = NULL;
   if (pipe(po) != 0) return -1;
   if (pipe(pi) != 0) { close(po[0]); close(po[1]); return -1; }
   pid = fork();
   if (pid < 0) { close(po[0]); close(po[1]); close(pi[0]); close(pi[1]); return -1; }
   if (pid == 0)
   {
      int nul = open("/dev/null", O_WRONLY);
      dup2(pi[0], 0); dup2(po[1], 1);
      if (nul >= 0) dup2(nul, 2);
      close(po[0]); close(po[1]); close(pi[0]); close(pi[1]);
      execv("/bin/sh", (char *const *)argv);
      _exit(127);
   }
   close(po[1]); close(pi[0]);
   if (stdin_text && *stdin_text) { ssize_t w = write(pi[1], stdin_text, strlen(stdin_text)); (void)w; }
   if (write(pi[1], "\n", 1) < 0) {}
   close(pi[1]);
   for (;;)
   {
      char buf[512];
      ssize_t r = read(po[0], buf, sizeof(buf));
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      if (n && got < n - 1)
      {
         size_t c = (size_t)r < n - 1 - got ? (size_t)r : n - 1 - got;
         memcpy(out + got, buf, c); got += c; out[got] = 0;
      }
   }
   close(po[0]);
   while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
   return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *kv(const char *text, const char *key, char *val, size_t n)
{
   size_t kl = strlen(key);
   const char *p = text;
   val[0] = 0;
   while (p && *p)
   {
      if (!strncmp(p, key, kl) && p[kl] == '=')
      {
         const char *e = strchr(p + kl + 1, '\n');
         size_t l = e ? (size_t)(e - (p + kl + 1)) : strlen(p + kl + 1);
         if (l >= n) l = n - 1;
         memcpy(val, p + kl + 1, l); val[l] = 0;
         return val;
      }
      p = strchr(p, '\n'); if (p) p++;
   }
   return val;
}

/* ---- jobs (worker thread; w_st is written under s_mx) ---- */
static int cap_of(const char *caps, const char *key)
{
   char v[16];
   kv(caps, key, v, sizeof(v));
   return !strcmp(v, "yes") || !strcmp(v, "load") ? NET_CAP_YES : NET_CAP_NO;
}

static void do_caps(void)
{
   char out[200], v[16];
   const char *a[] = { "caps", NULL };
   char *p;
   run(a, NULL, out, sizeof(out));
   for (p = out; *p; p++) if (*p == ' ') *p = '\n';   /* "wifi=yes bt=load ..." */
   SDL_LockMutex(s_mx);
   w_st.wifi_cap = cap_of(out, "wifi"); w_st.bt_cap = cap_of(out, "bt"); w_st.eth_cap = cap_of(out, "eth");
   w_st.stock_kernel = !strcmp(kv(out, "kernel", v, sizeof(v)), "stock");
   bump();
   SDL_UnlockMutex(s_mx);
   logf_("net: caps wifi=%d bt=%d eth=%d stock=%d", w_st.wifi_cap, w_st.bt_cap, w_st.eth_cap, w_st.stock_kernel);
}

static void time_job(void)
{
   char out[64];
   const char *a[] = { "time", NULL };
   if (run(a, NULL, out, sizeof(out)) == 0 && out[0])
   {
      char *nl = strchr(out, '\n'); if (nl) *nl = 0;
      SDL_LockMutex(s_mx);
      w_st.time_ok = 1; snprintf(w_st.time_text, sizeof(w_st.time_text), "%.16s", out); bump();
      SDL_UnlockMutex(s_mx);
      logf_("net: clock set over NTP: %s UTC", out);
   }
}

static void status_wifi(void)
{
   char out[400], v[64];
   const char *a[] = { "wifi", "status", NULL };
   int st;
   run(a, NULL, out, sizeof(out));
   kv(out, "state", v, sizeof(v));
   st = !strcmp(v, "noadapter") ? NET_NOADAPTER : !strcmp(v, "off") || !v[0] ? NET_OFF :
        !strcmp(v, "COMPLETED") ? NET_CONNECTED :
        (!strcmp(v, "DISCONNECTED") || !strcmp(v, "INACTIVE") || !strcmp(v, "INTERFACE_DISABLED")) ? NET_DOWN : NET_CONNECTING;
   SDL_LockMutex(s_mx);
   w_st.wifi = st;
   kv(out, "ssid", w_st.ssid, sizeof(w_st.ssid));
   kv(out, "ip", w_st.ip, sizeof(w_st.ip));
   if (st != NET_CONNECTED) w_st.ip[0] = 0;
   bump();
   SDL_UnlockMutex(s_mx);
}

static void status_eth(void)
{
   char out[200], v[32];
   const char *a[] = { "eth", "status", NULL };
   int st;
   run(a, NULL, out, sizeof(out));
   kv(out, "state", v, sizeof(v));
   st = !strcmp(v, "connected") ? NET_CONNECTED : !strcmp(v, "connecting") ? NET_CONNECTING :
        !strcmp(v, "nocable") ? NET_NOCABLE : !strcmp(v, "off") ? NET_OFF : NET_NOADAPTER;
   SDL_LockMutex(s_mx);
   w_st.eth = st; kv(out, "ip", w_st.eth_ip, sizeof(w_st.eth_ip)); bump();
   SDL_UnlockMutex(s_mx);
}

static int cmp_ap(const void *a, const void *b)
{
   const net_ap_t *x = a, *y = b;
   if (x->connected != y->connected) return y->connected - x->connected;
   if (x->saved != y->saved) return y->saved - x->saved;
   return y->signal - x->signal;
}

static void scan_wifi(void)
{
   static char out[4096], saved[1024];
   const char *a[] = { "wifi", "scan", NULL }, *b[] = { "wifi", "saved", NULL };
   net_ap_t aps[NET_MAX_APS];
   int n = 0;
   char *line, *save = NULL;
   SDL_LockMutex(s_mx); w_st.scanning = 1; bump(); SDL_UnlockMutex(s_mx);
   run(a, NULL, out, sizeof(out));
   run(b, NULL, saved, sizeof(saved));
   for (line = strtok_r(out, "\n", &save); line && n < NET_MAX_APS; line = strtok_r(NULL, "\n", &save))
   {  /* signal \t security \t ssid */
      char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
      if (!t2) continue;
      *t1 = 0; *t2 = 0;
      memset(&aps[n], 0, sizeof(aps[n]));
      aps[n].signal = atoi(line);
      snprintf(aps[n].sec, sizeof(aps[n].sec), "%s", t1 + 1);
      snprintf(aps[n].ssid, sizeof(aps[n].ssid), "%s", t2 + 1);
      {  /* saved: one name per line in `saved` */
         const char *p = saved;
         size_t l = strlen(aps[n].ssid);
         while (p && *p)
         {
            if (!strncmp(p, aps[n].ssid, l) && (p[l] == '\n' || !p[l])) { aps[n].saved = 1; break; }
            p = strchr(p, '\n'); if (p) p++;
         }
      }
      n++;
   }
   SDL_LockMutex(s_mx);
   {
      int i;
      for (i = 0; i < n; i++) aps[i].connected = w_st.wifi == NET_CONNECTED && !strcmp(aps[i].ssid, w_st.ssid);
   }
   qsort(aps, (size_t)n, sizeof(aps[0]), cmp_ap);
   memcpy(w_st.aps, aps, sizeof(aps[0]) * (size_t)n);
   w_st.naps = n; w_st.scanning = 0; w_st.scan_age = now_s(); bump();
   SDL_UnlockMutex(s_mx);
}

static int cmp_dev(const void *a, const void *b)
{
   const net_dev_t *x = a, *y = b;
   if (x->connected != y->connected) return y->connected - x->connected;
   if (x->paired != y->paired) return y->paired - x->paired;
   return strcmp(x->name, y->name);
}

static void list_bt(int scan_seconds)
{
   static char out[4096];
   net_dev_t dev[NET_MAX_DEVS];
   int n = 0, paired = 0, conn = 0;
   char *line, *save = NULL;
   if (scan_seconds > 0)
   {
      char secs[8];
      const char *a[] = { "bt", "scan", secs, NULL };
      snprintf(secs, sizeof(secs), "%d", scan_seconds);
      SDL_LockMutex(s_mx); w_st.bt_scanning = 1; bump(); SDL_UnlockMutex(s_mx);
      run(a, NULL, out, sizeof(out));
   }
   {
      const char *a[] = { "bt", "devices", NULL };
      run(a, NULL, out, sizeof(out));
   }
   for (line = strtok_r(out, "\n", &save); line && n < NET_MAX_DEVS; line = strtok_r(NULL, "\n", &save))
   {  /* mac \t paired \t connected \t name [\t icon] */
      char *f[5]; int k = 0; char *p = line;
      f[k++] = p;
      while (k < 5 && (p = strchr(p, '\t'))) { *p++ = 0; f[k++] = p; }
      if (k < 4 || strlen(f[0]) != 17) continue;
      memset(&dev[n], 0, sizeof(dev[n]));
      snprintf(dev[n].mac, sizeof(dev[n].mac), "%s", f[0]);
      dev[n].paired = atoi(f[1]); dev[n].connected = atoi(f[2]);
      snprintf(dev[n].name, sizeof(dev[n].name), "%s", f[3]);
      if (k > 4) snprintf(dev[n].icon, sizeof(dev[n].icon), "%s", f[4]);
      paired += dev[n].paired; conn += dev[n].connected;
      n++;
   }
   qsort(dev, (size_t)n, sizeof(dev[0]), cmp_dev);
   SDL_LockMutex(s_mx);
   memcpy(w_st.dev, dev, sizeof(dev[0]) * (size_t)n);
   w_st.ndev = n; w_st.paired = paired; w_st.connected = conn; w_st.bt_scanning = 0; bump();
   SDL_UnlockMutex(s_mx);
}

static void toast(const char *key) { SDL_LockMutex(s_mx); w_toast = key; SDL_UnlockMutex(s_mx); }

static void busy(int on);

/* a USB Bluetooth adapter, plugged in: a USB interface of class e0/01/01 (Bluetooth HCI,
 * whether or not its driver is loaded), or an hci device the kernel already has */
static int usb_bt_present(void)
{
   DIR *d;
   struct dirent *e;
   const char *fake = getenv("LUMEN_FAKE_BT_ADAPTER");
   int found = 0;
   if (fake && *fake == '1') return 1;
   if ((d = opendir("/sys/class/bluetooth")))
   {
      while ((e = readdir(d))) if (!strncmp(e->d_name, "hci", 3)) found = 1;
      closedir(d);
      if (found) return 1;
   }
   if (!(d = opendir("/sys/bus/usb/devices"))) return 0;
   while (!found && (e = readdir(d)))
   {
      char p[300], v[3][8];
      static const char *const f[3] = { "bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol" };
      int k;
      if (!strchr(e->d_name, ':')) continue;   /* interfaces are "1-1:1.0" */
      for (k = 0; k < 3; k++)
      {
         FILE *fp;
         snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/%s", e->d_name, f[k]);
         v[k][0] = 0;
         if ((fp = fopen(p, "r"))) { if (!fgets(v[k], sizeof(v[k]), fp)) v[k][0] = 0; fclose(fp); }
      }
      found = !strncmp(v[0], "e0", 2) && !strncmp(v[1], "01", 2) && !strncmp(v[2], "01", 2);
   }
   closedir(d);
   return found;
}

/* a Wi-Fi interface the kernel has (a dongle whose driver is bound) */
static int wlan_present(void)
{
   DIR *d = opendir("/sys/class/net");
   struct dirent *e;
   int found = 0;
   if (!d) return 0;
   while (!found && (e = readdir(d)))
   {
      char p[300];
      if (e->d_name[0] == '.') continue;
      snprintf(p, sizeof(p), "/sys/class/net/%s/wireless", e->d_name);
      if (access(p, F_OK) == 0) found = 1;
      snprintf(p, sizeof(p), "/sys/class/net/%s/phy80211", e->d_name);
      if (access(p, F_OK) == 0) found = 1;
   }
   closedir(d);
   return found;
}

/* the USB devices plugged in, "vvvv:pppp product" each, for the log: which dongle a user has
 * is the first question when one does not work */
static void usb_list(char *out, size_t n)
{
   DIR *d = opendir("/sys/bus/usb/devices");
   struct dirent *e;
   size_t o = 0;
   out[0] = 0;
   if (!d) return;
   while ((e = readdir(d)))
   {
      static const char *const f[3] = { "idVendor", "idProduct", "product" };
      char v[3][64], p[300];
      int k;
      if (e->d_name[0] == '.' || strchr(e->d_name, ':') || !strncmp(e->d_name, "usb", 3)) continue;   /* devices, not interfaces or root hubs */
      for (k = 0; k < 3; k++)
      {
         FILE *fp;
         snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/%s", e->d_name, f[k]);
         v[k][0] = 0;
         if ((fp = fopen(p, "r"))) { if (fgets(v[k], sizeof(v[k]), fp)) v[k][strcspn(v[k], "\n")] = 0; else v[k][0] = 0; fclose(fp); }
      }
      if (o < n) o += (size_t)snprintf(out + o, n - o, "%s%s %s:%s %s", o ? "; " : "", e->d_name, v[0], v[1], v[2]);
   }
   closedir(d);
}

/* a game controller among the devices nearby, not paired yet */
static int is_pad(const net_dev_t *d)
{
   static const char *const names[] = { "controller", "dualsense", "dualshock", "8bitdo", "gamepad", "joy-con" };
   size_t i;
   if (!strcmp(d->icon, "input-gaming")) return 1;
   for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (strcasestr(d->name, names[i])) return 1;
   return 0;
}
static void ap_set(int st, const char *name)
{
   SDL_LockMutex(s_mx);
   w_st.ap_state = st;
   if (name) snprintf(w_st.ap_name, sizeof(w_st.ap_name), "%s", name);
   bump();
   SDL_UnlockMutex(s_mx);
}
/* one round: search 5 s, pair the first controller in pairing mode */
static void autopair_round(void)
{
   /* a controller that did not pair is left alone for a minute (it would be tried every
    * round and keep the others waiting), and its "did not pair" stays on screen for the next
    * search instead of being replaced at once */
   static char fail_mac[18];
   static double fail_until;
   char mac[18] = "", name[64] = "", out[200];
   int i, rc;
   if (w_st.ap_state != NET_AP_DONE && w_st.ap_state != NET_AP_FAILED) ap_set(NET_AP_SEARCHING, NULL);
   list_bt(5);
   if (w_st.ap_state == NET_AP_FAILED) ap_set(NET_AP_SEARCHING, NULL);
   SDL_LockMutex(s_mx);
   for (i = 0; i < w_st.ndev; i++)
      if (!w_st.dev[i].paired && is_pad(&w_st.dev[i]) && !(now_s() < fail_until && !strcmp(w_st.dev[i].mac, fail_mac)))
      { snprintf(mac, sizeof(mac), "%s", w_st.dev[i].mac); snprintf(name, sizeof(name), "%s", w_st.dev[i].name); break; }
   SDL_UnlockMutex(s_mx);
   if (!mac[0]) return;
   ap_set(NET_AP_PAIRING, name);
   logf_("net: pairing %s (%s) unattended", name, mac);
   {
      const char *a[] = { "bt", "pair", mac, NULL };
      busy(1); rc = run(a, NULL, out, sizeof(out)); busy(0);
   }
   ap_set(rc == 0 ? NET_AP_DONE : NET_AP_FAILED, name);
   if (rc != 0) { snprintf(fail_mac, sizeof(fail_mac), "%s", mac); fail_until = now_s() + 60.0; }
   logf_("net: %s %s", name, rc == 0 ? "paired" : "did not pair");
   list_bt(0);
}
static void busy(int on) { SDL_LockMutex(s_mx); w_st.busy = on; bump(); SDL_UnlockMutex(s_mx); }

static void do_job(const job_t *j)
{
   char out[300];
   switch (j->kind)
   {
   case J_CAPS: do_caps(); break;
   case J_WIFI_ON:
   {
      const char *a[] = { "wifi", "on", NULL };
      int rc = run(a, NULL, out, sizeof(out));
      if (rc != 0) logf_("net: wifi on: %s", out);
      status_wifi();
      if (rc != 0 && strstr(out, "unsupported")) toast("net.wifi_unsupported");   /* an experimental driver, off */
      else if (rc != 0 && strstr(out, "blocked")) toast("net.wifi_blocked");   /* net.sh's crash guard */
      else if (rc != 0 && w_st.wifi == NET_NOADAPTER) toast("net.wifi_noadapter");
      break;
   }
   case J_WIFI_OFF: { const char *a[] = { "wifi", "off", NULL }; run(a, NULL, out, sizeof(out)); status_wifi(); break; }
   case J_WIFI_SCAN: scan_wifi(); break;
   case J_WIFI_CONNECT:
   {
      const char *a[] = { "wifi", "connect", j->a, NULL };
      int rc;
      busy(1);
      rc = run(a, j->pass, out, sizeof(out));
      busy(0);
      logf_("net: wifi connect \"%s\": %s", j->a, rc == 0 ? "ok" : "failed");
      toast(rc == 0 ? "net.wifi_connected" : strstr(out, "invalid password") ? "net.wifi_badpass" : "net.wifi_failed");
      status_wifi(); scan_wifi();
      break;
   }
   case J_WIFI_SELECT:
   {
      const char *a[] = { "wifi", "select", j->a, NULL };
      int rc;
      busy(1);
      rc = run(a, NULL, out, sizeof(out));
      busy(0);
      toast(rc == 0 ? "net.wifi_connected" : "net.wifi_failed");
      status_wifi(); scan_wifi();
      break;
   }
   case J_WIFI_FORGET:
   { const char *a[] = { "wifi", "forget", j->a, NULL }; run(a, NULL, out, sizeof(out)); status_wifi(); scan_wifi(); toast("net.wifi_forgotten"); break; }
   case J_BT_ON:
   {
      const char *a[] = { "bt", "on", NULL };
      int rc = run(a, NULL, out, sizeof(out));
      SDL_LockMutex(s_mx); w_st.bt = rc == 0 ? NET_CONNECTED : NET_NOADAPTER; bump(); SDL_UnlockMutex(s_mx);
      if (rc != 0) { logf_("net: bt on: %s", out); toast("net.bt_noadapter"); }
      else list_bt(0);
      break;
   }
   case J_BT_OFF:
   { const char *a[] = { "bt", "off", NULL }; run(a, NULL, out, sizeof(out)); SDL_LockMutex(s_mx); w_st.bt = NET_OFF; w_st.ndev = 0; bump(); SDL_UnlockMutex(s_mx); break; }
   case J_BT_PAIR:
   {
      const char *a[] = { "bt", "pair", j->a, NULL };
      int rc;
      busy(1);
      rc = run(a, NULL, out, sizeof(out));
      busy(0);
      toast(rc == 0 ? "net.bt_paired" : "net.bt_failed");
      list_bt(0);
      break;
   }
   case J_BT_REMOVE:
   { const char *a[] = { "bt", "remove", j->a, NULL }; run(a, NULL, out, sizeof(out)); toast("net.bt_removed"); list_bt(0); break; }
   case J_ETH_ON: { const char *a[] = { "eth", "on", NULL }; run(a, NULL, out, sizeof(out)); status_eth(); break; }
   case J_TIME: time_job(); break;
   }
}

static int worker(void *unused)
{
   double next = 0, eth_retry = 0, next_present = 0, bt_retry = 0, wifi_retry = 0;
   (void)unused;
   for (;;)
   {
      job_t j;
      int have = 0, page, wifi_card, bt_card, pad, autopair;
      SDL_LockMutex(s_mx);
      if (!s_qn) SDL_CondWaitTimeout(s_cv, s_mx, 500);
      if (s_qn) { j = s_q[0]; memmove(s_q, s_q + 1, sizeof(s_q[0]) * (size_t)--s_qn); have = 1; }
      page = w_watch_page; wifi_card = w_watch_wifi; bt_card = w_watch_bt;
      pad = w_watch_pad; autopair = w_autopair;
      SDL_UnlockMutex(s_mx);
      if (have) { do_job(&j); continue; }
      if (now_s() >= next_present)
      {  /* hot-plug, every few seconds: the USB devices (logged when they change), a Bluetooth
          * adapter, and a Wi-Fi or Bluetooth that was switched on before its dongle was
          * plugged in (it failed then, "no adapter"): started again once one is there */
         static char last_usb[1024];
         char usb[1024];
         int p = usb_bt_present();
         usb_list(usb, sizeof(usb));
         int usb_changed = strcmp(usb, last_usb) != 0;
         if (usb_changed) { logf_("net: USB devices: %s", usb[0] ? usb : "none"); snprintf(last_usb, sizeof(last_usb), "%s", usb); wifi_retry = 0; }
         if (p != w_st.bt_present) { SDL_LockMutex(s_mx); w_st.bt_present = p; bump(); SDL_UnlockMutex(s_mx); logf_("net: Bluetooth adapter %s", p ? "plugged in" : "gone"); }
         if (w_st.bt_on && w_st.bt == NET_NOADAPTER && p && now_s() >= bt_retry)
         {
            job_t r; memset(&r, 0, sizeof(r)); r.kind = J_BT_ON;
            logf_("net: a Bluetooth adapter is there now: starting Bluetooth again");
            do_job(&r); bt_retry = now_s() + 15.0;
         }
         /* Wi-Fi: its driver is only loaded for the adapter plugged in, so try again when the
          * USB devices changed (once per change), or when an interface is already there */
         if (w_st.wifi_on && w_st.wifi == NET_NOADAPTER && (usb_changed || wlan_present()) && now_s() >= wifi_retry)
         {
            job_t r; memset(&r, 0, sizeof(r)); r.kind = J_WIFI_ON;
            logf_("net: the USB devices changed: starting Wi-Fi again");
            do_job(&r); wifi_retry = now_s() + 15.0;
         }
         next_present = now_s() + (pad ? 3.0 : 5.0);
      }
      if (autopair)
      {
         /* one controller per card: after "Done" nothing else is paired until it opens again */
         if (w_st.bt == NET_CONNECTED && w_st.ap_state != NET_AP_DONE) { autopair_round(); continue; }
         if (w_st.bt == NET_NOADAPTER && w_st.ap_state != NET_AP_NOADAPTER) ap_set(NET_AP_NOADAPTER, NULL);
      }
      else if (w_st.ap_state != NET_AP_IDLE) ap_set(NET_AP_IDLE, "");
      if (now_s() >= next && (page || wifi_card || bt_card))
      {
         int was_online = w_st.wifi == NET_CONNECTED || w_st.eth == NET_CONNECTED;
         if (w_st.wifi_on) status_wifi();
         status_eth();
         /* an adapter plugged in after the start: bring it up (loads nothing twice) */
         if (w_st.eth == NET_NOADAPTER && now_s() >= eth_retry) { const char *a[] = { "eth", "on", NULL }; char o[64]; run(a, NULL, o, sizeof(o)); eth_retry = now_s() + 10; }
         if (!was_online && (w_st.wifi == NET_CONNECTED || w_st.eth == NET_CONNECTED) && w_st.time_on && !w_st.time_ok) time_job();
         if (wifi_card && w_st.wifi_on && now_s() - w_st.scan_age > 20) scan_wifi();
         if (bt_card && w_st.bt == NET_CONNECTED) list_bt(6);   /* searching while the card is open */
         else if (page && w_st.bt == NET_CONNECTED) list_bt(0);
         next = now_s() + 3.0;
      }
   }
   return 0;
}

void net_init(void)
{
   const char *o = getenv("LUMEN_NET_SH");
   SDL_Thread *th;
   snprintf(s_script, sizeof(s_script), "%s", o && *o ? o : "");
   if (!s_script[0]) snprintf(s_script, sizeof(s_script), "%s/net.sh", g_home);
   if (access(s_script, R_OK) != 0) { logf_("net: no %s, connections off", s_script); return; }
   signal(SIGPIPE, SIG_IGN);
   s_mx = SDL_CreateMutex(); s_cv = SDL_CreateCond();
   memset(&w_st, 0, sizeof(w_st));
   w_st.wifi_on = settings_get_int("wifi", 0, 0, 1);
   w_st.bt_on = settings_get_int("bluetooth", 0, 0, 1);
   w_st.time_on = settings_get_int("net_time", 1, 0, 1);
   w_st.wifi = w_st.eth = NET_NOADAPTER;
   w_st.bt = NET_OFF;
   if (!(th = SDL_CreateThread(worker, "net", NULL))) { logf_("net: no thread"); return; }
   SDL_DetachThread(th);
   s_started = 1;
   push(J_CAPS, NULL, NULL);
   if (w_st.wifi_on) push(J_WIFI_ON, NULL, NULL);
   if (w_st.bt_on) push(J_BT_ON, NULL, NULL);
   push(J_ETH_ON, NULL, NULL);   /* a USB Ethernet adapter needs nothing: DHCP when there is one */
   s_ui = w_st;
}

void net_poll(void)
{
   if (!s_started) return;
   SDL_LockMutex(s_mx);
   if (w_st.gen != s_ui.gen) s_ui = w_st;
   SDL_UnlockMutex(s_mx);
}

const net_state_t *net_state(void) { return &s_ui; }

const char *net_take_toast(void)
{
   const char *k;
   if (!s_started) return NULL;
   SDL_LockMutex(s_mx); k = w_toast; w_toast = NULL; SDL_UnlockMutex(s_mx);
   return k;
}

void net_watch(int page, int wifi_card, int bt_card)
{
   if (!s_started) return;
   SDL_LockMutex(s_mx);
   if ((wifi_card && !w_watch_wifi) || (bt_card && !w_watch_bt) || (page && !w_watch_page)) SDL_CondSignal(s_cv);
   w_watch_page = page; w_watch_wifi = wifi_card; w_watch_bt = bt_card;
   SDL_UnlockMutex(s_mx);
}

void net_pad_watch(int no_pads, int autopair)
{
   if (!s_started) return;
   SDL_LockMutex(s_mx);
   if ((no_pads && !w_watch_pad) || (autopair && !w_autopair)) SDL_CondSignal(s_cv);
   w_watch_pad = no_pads; w_autopair = autopair;
   SDL_UnlockMutex(s_mx);
}

static void set_flag(int *flag, const char *key, int on)
{
   SDL_LockMutex(s_mx); *flag = on; bump(); SDL_UnlockMutex(s_mx);
   settings_set_int(key, on);
}

void net_wifi_set(int on)
{
   if (!s_started) return;
   set_flag(&w_st.wifi_on, "wifi", on);
   push(on ? J_WIFI_ON : J_WIFI_OFF, NULL, NULL);
   if (on) push(J_WIFI_SCAN, NULL, NULL);
}
void net_bt_set(int on)
{
   if (!s_started) return;
   set_flag(&w_st.bt_on, "bluetooth", on);
   push(on ? J_BT_ON : J_BT_OFF, NULL, NULL);
}
void net_time_set(int on)
{
   if (!s_started) return;
   set_flag(&w_st.time_on, "net_time", on);
   if (on && !w_st.time_ok && (w_st.wifi == NET_CONNECTED || w_st.eth == NET_CONNECTED)) push(J_TIME, NULL, NULL);
}
void net_wifi_scan(void) { push(J_WIFI_SCAN, NULL, NULL); }
void net_wifi_connect(const char *ssid, const char *password) { push(J_WIFI_CONNECT, ssid, password); }
void net_wifi_select(const char *ssid) { push(J_WIFI_SELECT, ssid, NULL); }
void net_wifi_forget(const char *ssid) { push(J_WIFI_FORGET, ssid, NULL); }
void net_bt_pair(const char *mac) { push(J_BT_PAIR, mac, NULL); }
void net_bt_remove(const char *mac) { push(J_BT_REMOVE, mac, NULL); }
