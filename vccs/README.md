# VCCS - Vibe Coded Controller Settings

Native PS4 homebrew (C, OpenOrbis toolchain) that sets the DualShock 4 light bar:
RGB sliders, presets, brightness (MAX / MED / MIN / ZERO + fine slider), OFF,
restore-system-color, and **KEEP AFTER EXIT** so the setting stays after you leave the app.
Written for a jailbroken PS4 (FW 13.52 + GoldHEN).

## Status (read this)

| Item | State |
|---|---|
| App logic, input, UI, settings save/load | Written; **host tests pass** against stubbed Pad/UserService APIs |
| Background plugin (hooks + polling, stays-after-exit) | Written on the real GoldHEN Plugin SDK; **44 host tests pass** (stubbed Pad APIs, fake Detour). Its PS4 branch was only syntax-checked against stand-in headers. |
| Build with the real OpenOrbis SDK, `.pkg` / `.prx` output | **Not done here** - no SDK in the authoring environment. The GitHub workflow does it for you. |
| Run on real hardware (app or plugin) | **Not done** |

The PS4-specific calls (libScePad incl. `scePadGetHandle`, libSceVideoOut, libkernel
memory mapping, UserService, `sceKernelMkdir`, `scePthreadCreate`, `sceKernelGetProcessTime`)
are written from knowledge of the OpenOrbis headers and have not been compiled against them. Expect to
fix a few names/signatures on first build (see `plat_*`, `add_user`, `read_pads`,
`push_light` in `src/main.c` and `plugin/vccs_plugin.c`). The plugin's build rules are copied from the
GoldHEN Plugin SDK's own Makefile (the SDK is vendored in `third_party/`, MIT licensed).

## Build the .pkg with GitHub (easiest)

1. Create a GitHub repo and push this folder. `.github` is a hidden folder and web uploads often skip it: if it is missing in your repo, create `.github/workflows/build-pkg.yml` on github.com and paste in `workflow-copy/build-pkg.yml` (see `workflow-copy/README.txt`).
2. Open the repo's **Actions** tab. The **Build VCCS PKG** workflow runs on every push.
   You can also start it by hand: Actions -> Build VCCS PKG -> Run workflow.
3. When it finishes, open the run and download the artifact **vccs-pkg-and-plugin**. It
   contains `IV0000-VCCS00001_00-VCCS000000000000.pkg` and `vccs_plugin.prx`.
4. Pushing a tag such as `v1.0.0` also publishes both files on the repo's Releases page.

The workflow first runs the host tests, then downloads the latest OpenOrbis toolchain
release, installs clang/lld and .NET, finds `right.sprx` inside the toolchain, and runs
`make` and `make plugin`. If it cannot pick the right toolchain download on its own, run it
manually and paste a direct link to the toolchain's Linux archive into **toolchain_url**.
The workflow has not been run yet; if a step fails, the log shows which one, and the fix
is usually a tool flag in the Makefile (compare with `samples/hello_world/Makefile` in the toolchain).

## Build locally

1. Install the OpenOrbis PS4 Toolchain release, `clang` and `lld` (Debian/Ubuntu:
   `sudo apt install clang lld`), plus the .NET runtime for `PkgTool.Core`.
2. `export OO_PS4_TOOLCHAIN=/path/to/toolchain`
3. `make` -> `pkg/IV0000-VCCS00001_00-VCCS000000000000.pkg`
   (`make RIGHT_SPRX=/path/to/right.sprx` if it cannot find `right.sprx`)
4. `make plugin` -> `build/vccs_plugin.prx`
5. `make host-test` runs the logic tests on a PC without the SDK.

## Install on the console

1. Install the .pkg with GoldHEN's package installer (USB drive or your usual method).
2. For KEEP AFTER EXIT, also install the plugin:
   - copy `vccs_plugin.prx` to `/data/GoldHEN/plugins/`
   - add it to `/data/GoldHEN/plugins.ini` (a ready-made snippet is in `plugin/plugins.ini.example`):
     ```
     [default]
     /data/GoldHEN/plugins/vccs_plugin.prx
     ```
   - make sure plugin loading is enabled in GoldHEN, then restart the console or relaunch games.

## Controls

| Input | Action |
|---|---|
| D-pad Up/Down | Select row |
| D-pad Left/Right | Adjust (hold to repeat; hold R1 for big steps) |
| Square | Light ON/OFF toggle |
| Triangle | Restore system default color |
| Cross | Activate row (OFF toggle, restore system color, KEEP toggle) |

## How "stays after exit" works

The plugin (`plugin/vccs_plugin.c`, built on the GoldHEN Plugin SDK) runs inside whichever
processes GoldHEN loads it into (games, and possibly the system UI) and does two things:

1. **Hooks** `scePadSetLightBar` and `scePadResetLightBar` with the SDK's Detour. While the
   override is active, any colour a game or the system asks for is replaced by your VCCS colour,
   and "reset to the system colour" becomes your VCCS colour. This is what makes it hold inside games.
2. **Polls** `/data/vccs/config.ini` 4 times a second and pushes the colour itself, which covers
   a process that set the light bar before the plugin loaded or never sets it again.

The override is active only when the config is readable, `keep=1` (KEEP AFTER EXIT is ON),
`system_default=0`, and the VCCS app is **not** in front. The app writes an increasing counter to
`/data/vccs/alive` twice a second; if it stops changing for 3 s the app is considered gone.
So the app and plugin never fight while you are adjusting colours.

Safety features:
- **Prologue check:** the SDK's Detour overwrites the first 14 bytes of the hooked function. The plugin
  first disassembles them (SDK's HDE64) and refuses to hook if they contain a `ret`, a relative branch
  or a RIP-relative operand; it then falls back to polling only.
- **`no_hooks` switch:** create an empty file `/data/vccs/no_hooks` and the plugin never installs hooks
  (polling only). Use this if a game crashes after installing the plugin.
- **Never calls `scePadOpen`.** It only uses controllers the host process already opened
  (`scePadGetHandle`), so it cannot take a controller away from a game.
- **No-ops** when the config is missing, KEEP is OFF, or SYSTEM COLOR is active.

### Checking where it works: /data/vccs/plugin.log

One line per change of situation, tagged with the title id and process name, e.g.
`[… CUSA12345 eboot.bin pid=…] hook scePadSetLightBar installed` or
`active users=1 handles=1 applied=1 err=0x0`. Idle states say why (`standby:app-foreground`,
`standby:keep-off`, `standby:system-default`, `standby:no-config`). Hook problems say
`hook … failed` or `hook … skipped: unsafe prologue`. If the colour does not hold somewhere
(for example the home screen), this file shows whether the plugin loaded there at all and what
error code the light bar call returned. Send me the file and I can adjust the plugin.

### If there is no plugin.log at all

No log means the plugin never started in any process, so GoldHEN did not load it. Check, in order:

1. After the next build, the plugin also writes `/data/vccs_plugin_loaded.txt` the moment it loads. If that file
   does not exist either, it did not load.
2. The file is at exactly `/data/GoldHEN/plugins/vccs_plugin.prx` and `/data/GoldHEN/plugins.ini` contains
   `[default]` followed by that path on its own line (no `;` in front, no extra spaces).
3. Start the app **after** GoldHEN is loaded (re-run the jailbreak and load GoldHEN first), then open VCCS from
   the home screen. Plugins are loaded when a game or app starts, not into programs that were already running.
4. Make sure the `.prx` you copied is from the latest build artifact (`vccs-pkg-and-plugin`).
5. Test GoldHEN plugin loading itself with a plugin you know works; if that does not load either, the cause is
   the GoldHEN setup and not VCCS.

### Limits you should expect

- **Not tested on a PS4.** Nothing here has been compiled against the real OpenOrbis headers or run
  on a console. The hook target address is taken from the imported function symbol (as the SDK's
  `HOOK` macro does); if that does not land on real code, the prologue check should refuse it and
  you would see `skipped: unsafe prologue` in the log, leaving polling only.
- **Where the plugin loads is up to GoldHEN.** `[default]` applies to games; whether it also loads
  into the home-screen process is unverified. `plugin/plugins.ini.example` has an untested optional
  section for the system UI. If it holds in games but not on the home screen, that is the reason.
- **Detour limits:** hooking while another thread is inside the function is not made atomic by the SDK,
  and hooking a function twice is not supported.
- **If the console is rebooted**, the setting returns once the plugin loads again.
- Wait for "SETTINGS SAVED" in the footer before you leave the app so the plugin sees the latest change.
- The PS4's own Settings -> Devices -> Controllers -> "Brightness of DUALSHOCK 4 Light Bar"
  (Bright/Medium/Dim), as far as I know, still applies on top.

## Other technical notes

- **Protocol**: libScePad `scePadSetLightBar` / `scePadResetLightBar` with an RGB triple; libScePad
  handles USB and Bluetooth. Raw DS4 HID reports are not reachable from a userland app.
- **Brightness** is RGB scaled by a percentage; ZERO = black. **OFF** is RGB (0,0,0).
- **Multiple controllers**: found per logged-in user (up to 4); two controllers on the same user
  cannot be told apart. USB vs Bluetooth is not distinguished.
- **Errors** are shown on screen; disconnected controllers are skipped and re-applied on reconnect.

## Layout

```
src/main.c                        the app (UI, input, pad control, settings)
plugin/vccs_plugin.c              background plugin: hooks + polling (keeps colour after exit)
third_party/GoldHEN_Plugins_SDK/  GoldHEN Plugin SDK source (MIT, see its LICENSE)
plugin/plugins.ini.example        GoldHEN plugins.ini snippet
tests/                            host stubs, fake Detour, app and plugin logic tests
sce_sys/icon0.png, assets/        your VCCS logo
Makefile                          app, plugin, host tests
.github/workflows/build-pkg.yml   GitHub Actions build
```
