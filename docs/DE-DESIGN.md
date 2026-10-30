# Other desktop environments: research and backend design

**English** · [Türkçe](DE-TASARIM.md)

Sources were read on 30 October 2026; version lines come from the official repositories on that day. The development machine runs only Hyprland 0.56; none of the environments below were tried there.

## Summary

| Environment | Capture | Input | Clipboard | Path |
|---|---|---|---|---|
| Hyprland | `ext-image-copy-capture-v1` | `zwlr_virtual_pointer_v1` + `zwp_virtual_keyboard_v1` | `ext-data-control-v1` / `wlr-data-control` | Current code. **Works.** |
| Sway and wlroots ≥ 0.19 (river, labwc, Wayfire…) | `ext-image-copy-capture-v1` | same | same | Current code, no change expected. **Untested.** |
| wlroots 0.18 and older | only `wlr-screencopy-unstable-v1` | same | `wlr-data-control` | Needs a screencopy capture fallback. **Not written.** |
| niri | Smithay-based, not wlroots | unverified | unverified | Read the global list in a session. **Unknown.** |
| KDE Plasma 6 (KWin) | `zkde_screencast_unstable_v1` (KWin produces the PipeWire stream itself) | `org_kde_kwin_fake_input` | `ext-data-control-v1` | Separate branch. **Written, untested.** No greeter and no seamless handover. |
| GNOME (Mutter) | `org.gnome.Mutter.ScreenCast` (D-Bus) | `org.gnome.Mutter.RemoteDesktop` + EIS | Mutter RemoteDesktop clipboard | The CRD host's **own GNOME path**; our portal is not used. **Untested.** |

## Evidence

**wlroots.** At tag 0.18.0 the `types/` directory contains `wlr_screencopy_v1.c`, `wlr_data_control_v1.c`, `wlr_virtual_pointer_v1.c` and `wlr_virtual_keyboard_v1.c`. 0.19.0 adds `wlr_ext_image_copy_capture_v1.c` and `wlr_ext_data_control_v1.c`. Source: `gitlab.freedesktop.org/wlroots/wlroots`, `types` tree at both tags. Whether a compositor actually advertises these globals is a separate question: protocol code in wlroots does not mean the compositor enables it. That is why the code reads `wl_registry` at runtime instead of guessing from versions.

**KWin.** `KDE/kwin` master (latest tag v6.8.2) contains `src/wayland/screencast_v1.cpp`, `src/backends/fakeinput/`, `src/plugins/eis/` (including `eisprompter.cpp`) and `src/wayland/datacontroldevicemanager_v1.cpp`. The clipboard manager is `ext_data_control_manager_v1`. The tree has no `ext-image-copy-capture`, `wlr-screencopy` or wlr virtual pointer/keyboard.

In `src/wayland_server.cpp`, `KWinDisplay::allowInterface` lists `org_kde_kwin_fake_input`, `zkde_screencast_unstable_v1` and `ext_data_control_manager_v1` as restricted. The restriction only applies when `client->isSandboxed()` is true, so a native, unsandboxed process can bind them; the older `X-KDE-Wayland-Interfaces` desktop-file requirement is not in this version of the code. Earlier Plasma 6.x releases may behave differently; the version range is unverified.

**Mutter.** `GNOME/mutter` main has `src/backends/meta-remote-desktop*.c`, `meta-screen-cast*.c` and `meta-eis*.c`. On the Wayland side there is no `image-copy`, `data-control` or virtual pointer/keyboard protocol.

**CRD 157.** `third_party/chromium-157/remoting_host_create_desktop_interaction_strategy_factory.cc`, lines 31-35: under Wayland, if `GnomeRemoteDesktopSession::IsRunningUnderGnome()` is true, `GnomeInteractionStrategyFactory` is used, otherwise the portal path (`PortalInteractionStrategyFactory`). The GNOME path calls `org.gnome.Mutter.RemoteDesktop` and `org.gnome.Mutter.ScreenCast` directly (`remoting_host_linux_gnome_remote_desktop_session.cc`, lines 44-48). This project's portal is only used outside GNOME.

## How the backends are split

Shared:

- `src/portal`: D-Bus portal interfaces, session and request objects, EIS server.
- `src/proxy`: private bus for the host.
- `src/oturum`: seat watcher, greeter ↔ desktop handover.
- `src/ses`: PipeWire speaker monitor.
- `src/cikti`: output selection rule.
- `src/clipboard`: `ext-data-control-v1` (and `wlr-data-control`) on every backend.

Per compositor, chosen from the globals it advertises:

- `src/capture/capture.c`: if `ext_image_copy_capture_manager_v1` and the capture source manager are present, the process creates its own PipeWire node and copies frames. Otherwise, if `zkde_screencast_unstable_v1` is present (bound at version 5 so the `created` event carries the node id), it asks KWin to stream the chosen output with cursor metadata and hands KWin's node id to the host; no frames are copied.
- `src/giris/giris.c`: if `zwlr_virtual_pointer_manager_v1` and `zwp_virtual_keyboard_manager_v1` are present, they are used. Otherwise, if `org_kde_kwin_fake_input` version ≥ 4 is present, it is authenticated once and receives absolute motion (global logical coordinates), buttons, axes and keys. A half-present wlr set does not fall back to KWin.
- If neither set is complete, initialization fails and the missing globals are logged by name.

On KWin the stream closes when the seat changes, so the host's link breaks. Seamless handover would need an intermediate PipeWire node (today's node, fed from KWin's stream); not implemented.

## Greeter

| Greeter | Compositor | Status |
|---|---|---|
| greetd + dms-greeter / tuigreet (Hyprland, Sway) | wlroots-based compositor as the greeter user | Current ACL approach applies. Proven on Hyprland. |
| SDDM (KDE) | KWin as the `sddm` user on Plasma 6 (with the Wayland greeter) | Socket and ACL approach looks applicable. Unverified that the greeter KWin exposes the restricted globals to an unsandboxed client. |
| GDM (GNOME) | gnome-shell as `gdm` | Mutter's D-Bus interfaces live on the `gdm` session bus; the host cannot reach them as another user. Not supported; GNOME works only in an open session. |

## To do

1. wlroots: try on Sway (installer `--experimental`). Missing globals show up in the log.
2. wlroots 0.18: capture fallback with `wlr-screencopy-unstable-v1`.
3. KWin: try on Plasma; seamless handover needs an intermediate PipeWire node.
4. GNOME: try the user unit; greeter is not supported.
5. niri: read its globals in a session and pick the matching backend.

## Unverified

- Which global each wlroots compositor advertises in which version.
- That the `allowInterface` rule is the same across Plasma 6.0–6.7.
- KWin's restricted globals and socket permissions on the SDDM Wayland greeter.
- That KWin's stream can survive a seat change.
- niri's protocol support.
- Lock screen and greeter behaviour of CRD's GNOME path.
