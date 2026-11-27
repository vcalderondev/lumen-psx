/* Unit test of src/module.c (docs/modules.md): registry, switches and state.ini,
 * dependencies, availability, contributions and their limits. Host build, no GL:
 *
 *   tests/run.sh          (gcc or clang, SDL2 headers)
 *
 * The three built-in slots (mod_wifi, mod_ethernet, mod_bluetooth) are fakes here: in this
 * test "bluetooth" requires "wifi", so the dependency rules have something to act on. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/module.h"

/* ---- what module.c needs from the rest of Lumen ---- */
void logf_(const char *fmt, ...) { (void)fmt; }
static double s_now;
double now_s(void) { return s_now; }
const char *tr(const char *key) { return key; }
int settings_fsync_dir(const char *dir) { (void)dir; return 0; }

/* ---- fake modules ---- */
static int starts[3], stops[3], polls[3], events[3], last_ev = -1;
static const char *wifi_why;   /* what wifi's available() answers */

static void s0(void) { starts[0]++; }  static void x0(void) { stops[0]++; }
static void s1(void) { starts[1]++; }  static void x1(void) { stops[1]++; }
static void s2(void) { starts[2]++; }  static void x2(void) { stops[2]++; }
static void p0(double t) { (void)t; polls[0]++; }
static void e0(int ev, const void *a) { (void)a; events[0]++; last_ev = ev; }
static const char *a0(void) { return wifi_why; }

const lumen_module_t mod_wifi = { .id = "wifi", .api = LUMEN_MODULE_API, .flags = MOD_F_DEFAULT_ON, .version = "1.0",
                                  .available = a0, .start = s0, .stop = x0, .poll = p0, .event = e0 };
const lumen_module_t mod_ethernet = { .id = "ethernet", .api = LUMEN_MODULE_API, .version = "1.0",   /* default off */
                                      .start = s1, .stop = x1 };
const lumen_module_t mod_bluetooth = { .id = "bluetooth", .api = LUMEN_MODULE_API, .flags = MOD_F_DEFAULT_ON,
                                       .requires = "wifi", .version = "1.0", .start = s2, .stop = x2 };

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char s_home[256];
static void read_state(char *buf, size_t n)
{
   char p[300]; FILE *f; size_t r;
   snprintf(p, sizeof(p), "%s/modules/state.ini", s_home);
   buf[0] = 0;
   if (!(f = fopen(p, "r"))) return;
   r = fread(buf, 1, n - 1, f); buf[r] = 0; fclose(f);
}

static int vis_calls;
static int only_even(int gi, void *u) { (void)u; vis_calls++; return gi % 2 == 0; }
static int invoked_gi = -1;
static void inv(int gi, void *u) { (void)u; invoked_gi = gi; }

int main(void)
{
   char st[512];
   int wi, ei, bi, h[MOD_MENU_MAX], n;

   snprintf(s_home, sizeof(s_home), "/tmp/lumen-module-test-%d", (int)getpid());
   mkdir(s_home, 0755);

   /* defaults: wifi and bluetooth on, ethernet off; bluetooth starts after wifi */
   modules_init(s_home);
   wi = module_index("wifi"); ei = module_index("ethernet"); bi = module_index("bluetooth");
   CHECK(modules_count() == 3 && wi == 0 && ei == 1 && bi == 2);
   CHECK(module_running("wifi") && module_running("bluetooth") && !module_running("ethernet"));
   CHECK(starts[0] == 1 && starts[2] == 1 && starts[1] == 0);
   CHECK(module_state(wi) == MOD_ON && module_state(ei) == MOD_OFF);
   CHECK(last_ev == MEV_STARTED);
   CHECK(!strcmp(module_name_key(bi), "mod.bluetooth.name") && !strcmp(module_desc_key(bi), "mod.bluetooth.description"));

   /* poll and events reach running modules only */
   modules_poll(0.5);
   CHECK(polls[0] == 1);
   modules_emit(MEV_ONLINE, NULL);
   CHECK(last_ev == MEV_ONLINE);

   /* status: owner must run, per-module limit, order, clear, unchanged item */
   CHECK(mod_status_set("ethernet", "link", &(mod_status_t){ .glyph = "ethernet" }) == -1);   /* not running */
   CHECK(mod_status_set("wifi", "signal", &(mod_status_t){ .glyph = "wifi-3", .order = 10 }) == 0);
   CHECK(mod_status_set("wifi", "extra", &(mod_status_t){ .glyph = "network", .order = 5 }) == 0);
   CHECK(mod_status_set("wifi", "third", &(mod_status_t){ .glyph = "network" }) == -1);       /* 2 per module */
   CHECK(mod_status_set("bluetooth", "radio", &(mod_status_t){ .glyph = "bluetooth", .dim = 1, .order = 20, .badge = "2" }) == 0);
   CHECK(mod_status_count() == 3);
   CHECK(!strcmp(mod_status_item(0)->slot, "extra") && !strcmp(mod_status_item(2)->owner, "bluetooth"));
   CHECK(mod_status_item(2)->dim == 1 && !strcmp(mod_status_item(2)->badge, "2"));
   CHECK(mod_status_set("wifi", "signal", &(mod_status_t){ .glyph = "wifi-1", .order = 10 }) == 0);   /* update in place */
   CHECK(mod_status_count() == 3 && !strcmp(mod_status_item(1)->glyph, "wifi-1"));
   mod_status_clear("wifi", "extra");
   CHECK(mod_status_count() == 2);

   /* menu: key prefix rule, visibility per game, order, limit */
   CHECK(!module_key_ok("wifi", "gm.play") && !module_key_ok("wifi", "mod.bluetooth.x") && !module_key_ok("wifi", "mod.wifi.") &&
         module_key_ok("wifi", "mod.wifi.download") && !module_key_ok("wifi", "mod.wifix.a"));
   CHECK(mod_menu_add(&(mod_menu_t){ .owner = "wifi", .id = "bad", .label_key = "gm.play", .invoke = inv }) == -1);
   CHECK(mod_menu_add(&(mod_menu_t){ .owner = "wifi", .id = "b", .label_key = "mod.wifi.b", .order = 20, .invoke = inv }) == 0);
   CHECK(mod_menu_add(&(mod_menu_t){ .owner = "wifi", .id = "a", .label_key = "mod.wifi.a", .order = 10, .visible = only_even, .invoke = inv }) == 0);
   CHECK(mod_menu_add(&(mod_menu_t){ .owner = "wifi", .id = "c", .label_key = "mod.wifi.c", .invoke = inv }) == -1);   /* 2 per module */
   CHECK(mod_menu_add(&(mod_menu_t){ .owner = "bluetooth", .id = "z", .label_key = "mod.bluetooth.z", .order = 15, .invoke = inv }) == 0);
   n = mod_menu_list(4, h, MOD_MENU_MAX);
   CHECK(n == 3 && !strcmp(mod_menu_label(h[0]), "mod.wifi.a") && !strcmp(mod_menu_label(h[1]), "mod.bluetooth.z") &&
         !strcmp(mod_menu_label(h[2]), "mod.wifi.b"));
   n = mod_menu_list(3, h, MOD_MENU_MAX);   /* odd game: "a" hidden */
   CHECK(n == 2 && !strcmp(mod_menu_label(h[0]), "mod.bluetooth.z"));
   mod_menu_invoke(h[0], 3);
   CHECK(invoked_gi == 3);

   /* switching wifi off also switches off bluetooth (it requires wifi); both stop, their
    * contributions go; state.ini keeps the switches */
   CHECK(module_set(wi, 0) == 1);
   CHECK(!module_running("wifi") && !module_running("bluetooth") && stops[0] == 1 && stops[2] == 1);
   CHECK(module_state(wi) == MOD_OFF && module_state(bi) == MOD_OFF);
   CHECK(mod_status_count() == 0);
   CHECK(mod_menu_list(4, h, MOD_MENU_MAX) == 0);
   read_state(st, sizeof(st));
   CHECK(strstr(st, "wifi=0") && strstr(st, "bluetooth=0") && strstr(st, "ethernet=0"));

   /* switching bluetooth on also switches on wifi; wifi starts first */
   CHECK(module_set(bi, 1) == 1);
   CHECK(module_running("wifi") && module_running("bluetooth") && starts[0] == 2 && starts[2] == 2);

   /* availability: wifi cannot run -> it stops, bluetooth is blocked behind it */
   wifi_why = "net.wifi_nokernel";
   modules_poll(2.0);
   CHECK(!module_running("wifi") && module_state(wi) == MOD_UNAVAILABLE && !strcmp(module_why(wi), "net.wifi_nokernel"));
   CHECK(module_switch(wi) == 1);   /* the switch stays on */
   CHECK(!module_running("bluetooth"));   /* nothing runs without what it requires */
   CHECK(module_state(bi) == MOD_BLOCKED && !strcmp(module_blocker(bi), "wifi"));
   wifi_why = NULL;
   modules_poll(4.0);
   CHECK(module_running("wifi") && module_running("bluetooth"));

   /* blocked: bluetooth on, wifi off (state.ini edited by hand) */
   {
      char p[300]; FILE *f;
      snprintf(p, sizeof(p), "%s/modules/state.ini", s_home);
      f = fopen(p, "w"); fprintf(f, "wifi=0\nbluetooth=1\nethernet=1\nunknown=1\n"); fclose(f);
   }
   memset(starts, 0, sizeof(starts));
   modules_init(s_home);
   CHECK(!module_running("wifi") && !module_running("bluetooth") && module_running("ethernet"));
   CHECK(module_state(bi) == MOD_BLOCKED && !strcmp(module_blocker(bi), "wifi") && !strcmp(module_why(bi), "mods.blocked"));
   CHECK(starts[1] == 1);

   if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
   printf("module test: all checks passed\n");
   return 0;
}
