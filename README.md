# CRD for Linux

**English** · [Türkçe](README.tr.md)

**Chrome Remote Desktop with your existing Wayland session — no virtual desktop, no Xvfb.**

Google's Chrome Remote Desktop host on Linux normally starts a separate virtual X session. This project runs the unmodified official host against the session you are already using: from the web client or the mobile app, without a permission dialog, you see your real desktop, type with your own keyboard layout, share the clipboard and hear your speakers. It also works on the login screen: the host starts on the greeter, you type your password remotely and the same connection continues into your desktop.

## Status

| Environment | Status |
|---|---|
| Hyprland 0.56 (CachyOS, greetd + dms-greeter) | **Works.** Video, mouse, keyboard (incl. Turkish layout), two-way clipboard, audio, login from the lock screen and seamless handover verified. |
| Sway, river, labwc, niri, Wayfire (wlroots) | **Untested.** Uses the same Wayland protocols, so it is expected to work. |
| KDE Plasma 6 (KWin) | **Written, untested.** Capture via KWin's own screencast (`zkde_screencast`), input via `fake_input`, same clipboard. No greeter (SDDM) and no seamless handover at login. |
| GNOME (Mutter) | **Untested.** On GNOME the CRD host uses Mutter's own remote desktop interface; `kur.sh` installs a user unit that starts it in your session. Greeter (GDM) is not supported. |
| Multiple monitors | Not yet: one output is shared (chosen with `CRD_OUTPUT`). |
| File transfer | Not yet. |

Per-environment details and sources: [docs/DE-DESIGN.md](docs/DE-DESIGN.md).

If you try it on an untested environment, please open an issue with the result (works or not, with logs). That is the most useful contribution right now.

## How it works

The Linux CRD host already knows how to get the screen and input on Wayland through xdg-desktop-portal (PipeWire + libei/EIS). What is missing is a portal that needs no permission dialog and also works on the greeter. This project implements that portal and gives it to the host only, over a private D-Bus; your desktop's own portal is untouched.

```
chrome-remote-desktop-host (Google's unmodified binary)
        │  private D-Bus (xdg-dbus-proxy)
        ▼
crd-portal ── RemoteDesktop / ScreenCast / Clipboard portal interfaces
   ├─ capture: ext-image-copy-capture → PipeWire node
   │           (KWin: zkde_screencast stream)
   ├─ input: EIS server → zwlr_virtual_pointer / zwp_virtual_keyboard
   │         (KWin: org_kde_kwin_fake_input)
   ├─ clipboard: ext-data-control / wlr-data-control
   └─ audio: default speaker monitor → host audio pipe
        ▲
crd-koltuk (systemd system unit)
   follows greeter ↔ desktop handover; does not kill the host,
   capture/input/clipboard reconnect to the new compositor
```

| Directory | Purpose |
|---|---|
| `src/oturum` | Startup: `crd-koltuk.sh`, systemd unit, empty Wayland socket for GTK |
| `src/portal` | Portal server (`crd-portal`) |
| `src/capture` | Screen capture and PipeWire node |
| `src/cikti` | Finding and choosing the Wayland output |
| `src/eis`, `src/giris`, `src/olay` | Remote mouse and keyboard |
| `src/clipboard` | Clipboard |
| `src/ses` | Audio |
| `src/proxy` | Private D-Bus for the host |
| `src/netmon` | Portal network monitor interface |
| `src/gnome` | GNOME: starts the host in your session |
| `third_party/chromium-157` | Chromium sources the interfaces were checked against (BSD license) |

Source comments and log messages are in Turkish.

## Installation

One command, as the user whose screen will be shared (not root):

```bash
curl -fsSL https://raw.githubusercontent.com/KadirJeager/crd-for-linux/main/install.sh | bash
```

It downloads this project to `~/.local/share/crd-for-linux`, offers to install missing packages (Arch, Debian/Ubuntu, Fedora), downloads Google's official CRD package to your machine and unpacks it there without installing it system-wide, builds, installs the service, and finally registers the host with your Google account: open [remotedesktop.google.com/headless](https://remotedesktop.google.com/headless), click Begin › Next › Authorize, copy the "Debian Linux" command and paste it when asked. Only the code from that command is used; Google's own daemon (which would start a virtual desktop) is not installed. The PIN is asked by Google's tool and is never logged.

Run the same command again to update. Options go after `bash -s --`:

```bash
curl -fsSL https://raw.githubusercontent.com/KadirJeager/crd-for-linux/main/install.sh | bash -s -- --experimental
```

| Option | Meaning |
|---|---|
| `--experimental` | Required on anything other than Hyprland (untested environments). |
| `--dry-run` | Only print what would be done. |
| `--yes` | Do not ask before installing packages. |

From a clone you can also run `./kur.sh check`, `./kur.sh install`, `./kur.sh uninstall` (Turkish aliases: `denetle`, `kur`, `kaldir`, `--deneme`, `--deneysel`, `--evet`).

To remove:

```bash
~/.local/share/crd-for-linux/kur.sh uninstall
```

## Settings

| Variable | Meaning |
|---|---|
| `CRD_OUTPUT` | Name of the output to share (e.g. `HDMI-A-1`). If unset: the only output, or with several, the one at logical 0,0. |

The keyboard layout is read from the compositor's active keymap; no setting is needed.

## Known limits

- One monitor is shared.
- The host runs as the user whose screen is shared; to capture the greeter, that user gets ACL access to the greeter compositor's socket.
- If the screen size changes during a connection the stream is rebuilt; this path is untested.
- The client's on-screen keyboard (the panel with Esc, F keys, Ctrl, Win, Alt) opens empty. The client draws its keys from the keyboard layout the host sends, and the CRD host's portal path never connects its layout monitor to the input session (`PortalInteractionStrategy::CreateKeyboardLayoutMonitor` in Chromium; the GNOME path does call `SetKeyboardLayoutMonitor`), so it always sends an empty layout. This is in Google's host, not in this project. Typing with the phone's own keyboard works.

## License

GPL-3.0-or-later; full text in [LICENSE](LICENSE).

Files from other sources keep their own licenses:

- `third_party/chromium-157`: Chromium source files, BSD-style license (in file headers).
- `src/capture/proto/zkde-screencast-unstable-v1.xml`, `src/giris/proto/fake-input.xml`: KDE plasma-wayland-protocols, LGPL-2.1-or-later.
- `src/giris/proto/*.xml` (wlroots and virtual-keyboard protocols): license in file headers.

The Chrome Remote Desktop host is Google's binary and is not part of this repository; `kur.sh` downloads it from Google's official address to the user's own machine.
