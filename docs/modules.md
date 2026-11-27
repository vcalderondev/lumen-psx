# Modules

A **module** is a feature of Lumen that the user can switch on and off from
**Settings > Modules**, each with a name and a short description: Wi-Fi, Bluetooth, the USB
Ethernet adapter, and later the homebrew store, network libraries (SMB, FTP), and modules
installed from a file. A module that is off costs nothing. It starts nothing, loads no
package into RAM, and nothing it contributes shows up in the interface.

Modules never draw. They **contribute** to fixed **extension points** (an icon in the status
bar, an entry in the game menu, a row in Settings...) and Lumen draws those contributions
with its own UI kit. A third-party module therefore looks like the rest of Lumen, follows the
theme, and works at 720p and 1080p.

This document is the contract. API version: **1** (`LUMEN_MODULE_API`).

| | Status |
|---|---|
| Module registry, state, dependencies, Settings > Modules | implemented |
| Extension points `status`, `menu.game`, `events` | implemented |
| Built-in modules `wifi`, `bluetooth`, `ethernet` | implemented |
| Extension points `hints`, `settings` rows on other pages | specified, next |
| Installable modules (`.lpm`, `service`, protocol, permissions) | specified, phase 2 |
| Extension points `library`, `card`; store and network library modules | phase 3 |

## 1. Identity and lifecycle

Every module has an `id`: `a-z0-9-`, 2 to 24 characters. The ids `lumen`, `core`, `system`
and `settings` are reserved.

A module is in one of these states (`MOD_*` in `src/module.h`):

| State | Meaning | Settings > Modules shows |
|---|---|---|
| `MOD_OFF` | switched off by the user (or by default) | Off |
| `MOD_ON` | running: `start()` was called, its contributions are live | On |
| `MOD_UNAVAILABLE` | switched on, but `available()` says it cannot run here (no kernel support, a package missing) | the reason (a translation key) |
| `MOD_BLOCKED` | switched on, but a module it `requires` cannot run (off, unavailable, or blocked itself) | "Needs *name*" |
| `MOD_RESTART` | the switch changed, and the module only applies that at the next start | "After restart" |

- **The switch** is stored in `<home>/modules/state.ini`, one `<id>=0|1` line per module.
  It is kept apart from `settings.ini`, whose number of keys is limited. A module with no
  line uses its default (`MOD_F_DEFAULT_ON`).
- **Dependencies**: `requires` lists other module ids. Switching a module on also switches
  on what it requires. Switching a module off also switches off the modules that require it,
  and the toast names them.
- **When modules start**: at boot, after settings and translations, in dependency order.
  When switched on, `start()`; when switched off, `stop()`, after which every contribution
  the module registered is removed by Lumen, even ones it forgot.
- **`MOD_F_RESTART`** marks a module whose switch only takes effect at the next start
  (kernel modules that cannot be unloaded, overlays on `/usr`). Toggling it marks it
  `MOD_RESTART`, and the Modules page offers the restart.
- **`MOD_F_REQUIRED`** marks a module that cannot be switched off. None of today's modules
  use it; it is reserved for features that other parts of Lumen depend on.

## 2. Translations

**Every language Lumen ships is mandatory, English first.** Today that is `de`, `en`, `es`,
`fr`, `it` and `pt` (`dist/lang/`).

- **Every text a module contributes is a translation key, never a literal**: its name, its
  description, every menu entry, row label, value and toast.
- **A module's keys start with `mod.<id>.`**: `mod.wifi.name`, `mod.wifi.description`,
  `mod.wifi.signal`. A module cannot read or override Lumen's keys, or another module's.
- **Required keys**: `mod.<id>.name` (up to 24 characters in English) and
  `mod.<id>.description` (one sentence, up to 90 characters; the help line of the Modules
  page shows it).
- **Built-in modules** keep their keys in `dist/lang/*.lang`, next to Lumen's own.
  `tools/check-lang.sh` already fails when a language lacks a key the others have.
- **Installable modules** (phase 2) carry `lang/<code>.lang` files, in the format of
  Lumen's. Installing one fails unless there is one file for every language Lumen ships,
  each with exactly the keys of `en.lang`, all with the module's prefix.
- **If a later Lumen adds a language**, the modules already installed keep working: that
  language shows their English text, and the module's detail page says the translation is
  missing. The registry requires the new language for later versions.

## 3. Extension points

Contributions are owned by a module. They are created and updated from the module's
`start()`, `poll()` or `event()`, all on the render thread. Lumen removes them all when the
module stops. Each point has a per-module limit, so no module can crowd the screen, and
Lumen's own elements always come first.

### `status`: the status bar (implemented)

The status bar is the top right of the dashboard. It holds the clock, then the modules'
items, then the controllers.

```c
mod_status_set(owner, "signal", &(mod_status_t){ .glyph = "wifi-2", .order = 10 });
mod_status_clear(owner, "signal");
```

- `glyph` names a glyph in `assets/glyphs/`. The status glyphs are `wifi-0` to `wifi-3`
  (the lit arcs show the signal), `bluetooth`, `ethernet` and `network`; they come from
  `tools/make-glyphs.py`.
- `dim` draws the item faint (a device that is paired but not connected).
- `badge` is up to 3 characters drawn after the glyph (a count, a battery percentage).
- `order` sorts the items: a lower order is closer to the clock.
- Limit: 2 items per module, 6 in total.

### `menu.game`: the game menu (implemented)

These are entries in the card that Start opens on a game. They sit after Lumen's own entries
(Play, Favourites) and before Folders.

```c
mod_menu_add(&(mod_menu_t){ .owner = "netlib", .id = "download", .label_key = "mod.netlib.download",
                            .order = 10, .visible = is_remote, .invoke = download, .user = NULL });
```

- `visible(game_index, user)` is asked when the menu opens. If it returns 0, the entry is not
  shown for that game.
- `invoke(game_index, user)` runs when the entry is chosen, and the menu closes. It must not
  block: start the work on a thread, and report back through `panel_toast()` with a
  `mod.<id>.*` key.
- Limit: 2 entries per module, 4 in total.

### `events`: what happens around the modules (implemented)

```c
void (*event)(int ev, const void *arg);
```

| Event | When | `arg` |
|---|---|---|
| `MEV_STARTED` | every module has started (boot) | |
| `MEV_ONLINE` / `MEV_OFFLINE` | the first connection (Wi-Fi or cable) came up / the last one went down; Lumen emits them | |
| `MEV_LANGUAGE` | the language changed (rebuild what holds translated text) | the language code |
| `MEV_LAUNCH` | a game or app is about to start (Lumen exits next; `run.sh` starts it again afterwards) | the game index (`const int *`) |
| `MEV_EXIT` | Lumen is about to exit, for any reason (launch, restart, update): save what must survive | |

### `hints`: the help bar (next)

A module will be able to add one action to a help bar set (`HINTS_CAROUSEL`, or
`HINTS_CAROUSEL` with the focus on one system), bound to a button that the set leaves free.
For example, Square opens the store with the focus on Homebrew. Lumen routes that button to
the module only while the hint is shown.

### `settings`: rows in Settings (next)

A module will be able to add rows (switch, choice, action, info) to its own detail page in
Settings > Modules, or to an existing page (Network, Sound). The built-in network modules
keep their own pages (Network, Bluetooth). Those pages appear in the rail only while one of
their modules is on (implemented).

### `library`, `card` (phase 3)

- `library` will be for providers of library entries: a fixed "Store" tile in Homebrew, or
  games listed from an SMB or FTP share.
- `card` will be for full-screen cards, built-in modules only: the store, the list of
  networks.

## 4. Built-in modules: the C API

A built-in module is compiled into Lumen. It is a `lumen_module_t` listed in
`src/module.c` (`k_builtin`):

```c
typedef struct {
   const char *id;                       /* "wifi" */
   int         api;                      /* LUMEN_MODULE_API */
   unsigned    flags;                    /* MOD_F_DEFAULT_ON, MOD_F_RESTART, MOD_F_REQUIRED */
   const char *requires;                 /* "ethernet wifi": space-separated ids, or NULL */
   const char *version;                  /* "1.0" */
   const char *(*available)(void);       /* NULL = can run; else a tr key saying why not */
   void (*start)(void);
   void (*stop)(void);
   void (*poll)(double t);               /* every frame while running */
   void (*event)(int ev, const void *arg);
} lumen_module_t;
```

Every callback is optional. `available()` must be cheap: it is asked when the Modules page is
drawn.

**Today's built-in modules** (`src/mod_net.c`):

| Module | Default | Contributes |
|---|---|---|
| `wifi` | on | the Network page; a status item with the signal (`wifi-0`..`wifi-3`) while connected |
| `ethernet` | on | the Network page; a status item (`ethernet`) while a cable has an address |
| `bluetooth` | on | the Bluetooth page; a status item (`bluetooth`), dim with no device connected |

Switching `wifi` or `bluetooth` off switches that radio off without changing the user's own
switch on the Network or Bluetooth page. Switching the module on again restores the radio
as the user had left it. `ethernet` off leaves an adapter that is plugged in unconfigured.

## 5. Installable modules (phase 2)

### The package

An installable module is a `.lpm` file: a gzip tar with the module's folder at its root.
Settings > Modules > **Install from file** picks it with the file picker. The store installs
the same packages.

```
module.ini               the manifest (below)
lang/<code>.lang         one file per language Lumen ships (section 2)
icon.png                 optional, 64 x 64, white on transparent (tinted like the glyphs)
bin/<service>            optional: the module's process (ARM hard-float, glibc 2.24)
glyphs/<name>.png        optional: status glyphs, used as "<id>/<name>"
```

```ini
id=netlib
api=1
version=1.2.0
author=Someone
lumen=0.24                    ; the oldest Lumen that runs it
requires=wifi                 ; or several, space-separated
restart=0
permissions=network storage   ; network, bluetooth, storage, launch
service=bin/netlib            ; optional

[status.share]                ; declared contributions exist before the service starts
glyph=netlib/share
order=40

[menu.download]
label=mod.netlib.download
order=10
when=system:psx               ; optional filters: system:<id>, kind:game|app, ext:<list>
```

Lumen unpacks the package to `<home>/modules/<id>/`. It checks the files (no links, nothing
outside the folder), the id, `api`, `lumen`, the translations (section 2) and, for a package
from the registry, its signature. A package with no signature can be installed, but its
detail page marks it "Third party, not verified" and the user confirms. Installing never
switches a module on.

### The service and its protocol

A module with a `service` runs it as its own process while the module is on. The service
talks to Lumen over a Unix socket (`$LUMEN_MODULE_SOCKET`), one command per line,
`verb key=value ...`, with values percent-encoded:

```
-> status.set slot=share glyph=netlib/share dim=0 badge=3
-> status.clear slot=share
-> menu.visible id=download game=<content path> show=1
-> toast key=mod.netlib.done
<- event name=online
<- menu.invoke id=download game=<content path>
<- stop
```

- **Lumen never waits for a service.** Commands are read on a thread and applied at the
  next frame.
- **If the service crashes**, Lumen keeps running. It removes the module's live
  contributions, marks the module "Error" with its log (`logs/mod-<id>.log`), and starts it
  again after 2, 4, 8... seconds. After five failures in a row it stops trying until the
  next boot.
- **Every contribution goes through the same limits** as a built-in module's, and every
  text must be a `mod.<id>.*` key.

## 6. Settings > Modules

The Modules page is a rail category, between Library and System.

- **The list**: one row per module, with its name and its state as the value. The help line
  shows the module's description while its row is focused.
- **The detail page** (Cross on a row):
  - The switch.
  - Version, author and, for installable modules, permissions and size.
  - Why the module is unavailable or blocked, when it is.
  - For installable modules (phase 2): Uninstall.
- **Install from file** (phase 2), at the end of the list.
