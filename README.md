# wlim

Vimium-like click hints for Wayland. Press a keybind, get letter labels on every clickable thing, type a label, click happens. Like [Homerow](https://www.homerow.app/) but for Linux. Works on any wlroots-based compositor (dwl, Hyprland, Sway, etc).

## How it works

1. Walks the AT-SPI2 accessibility tree and finds all clickable elements (buttons, links, tabs, inputs, etc)
2. Draws a fullscreen transparent overlay using GTK4 + gtk4-layer-shell
3. Shows letter labels at each element's position
4. You type the letters, overlay closes, uinput clicks that spot
5. Hold Shift while typing the last letter to right-click
6. Hold Ctrl while typing the last letter to middle-click
7. Press `/` to search, type text to filter hints by element name, then Enter to confirm and pick a hint

## The Wayland coordinate problem

This was the hardest part. On Wayland, apps don't know their own screen position. Chromium reports `(0,0)` as its window position regardless of where the compositor actually put it. So all the element coordinates from AT-SPI are window-relative, but we need screen-absolute positions to place hints and click.

The fix: wlim looks at the difference between the window size and the monitor size. If your monitor is 2560x1600 and the window is 2558x1570, that's a 1px gap on each side and a 28px bar at the top. So the window is at `(1, 29)`. This works automatically, change your gaps or bar height and it recalculates.

The one thing it can't detect is which edge your bar is on (Wayland won't tell us). Defaults to top, set `bar_position` in the config if yours is elsewhere.

## Dependencies

- gtk4
- gtk4-layer-shell
- at-spi2 (libatspi)
- pkg-config

On Fedora:
```
sudo dnf install gtk4-devel gtk4-layer-shell-devel at-spi2-core-devel
```

On Gentoo:
```
emerge gui-libs/gtk gui-libs/gtk4-layer-shell app-accessibility/at-spi2-core
```

## Build

```
make
```

## Setup

**Enable accessibility** (needed for AT-SPI to work):
```
gsettings set org.gnome.desktop.interface toolkit-accessibility true
```

**uinput access.** wlim clicks via `/dev/uinput` directly (no ydotool needed). Add a udev rule so your user can access it:
```
echo 'KERNEL=="uinput", GROUP="input", MODE="0660"' | sudo tee /etc/udev/rules.d/99-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**Chromium/Electron apps** need an extra flag to expose their accessibility tree:
```
# add to ~/.config/chromium-flags.conf or equivalent
--force-renderer-accessibility
```

**Compositor keybind.** Bind wlim to a key. For example in dwl's `config.h`:
```c
{ MODKEY, XKB_KEY_semicolon, spawn, {.v = (const char*[]){ "/path/to/wlim", NULL }} },
{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_semicolon, spawn, {.v = (const char*[]){ "/path/to/wlim", "--scroll", NULL }} },
```

## Config

Create `~/.config/wlim/config` to customize. All keys are optional, defaults are used for anything missing.

```
# hint appearance
hint_bg=#2a2a2a
hint_fg=#e0e0e0
hint_fg_dim=#666
hint_border=rgba(255,255,255,0.6)
hint_font_size=11
hint_border_radius=4

# scroll speeds (in wheel ticks)
scroll_speed=1
page_speed=10
jump_speed=200

# bar position for offset inference (top, bottom, none)
bar_position=top
```

## Scroll mode

`wlim --scroll` gives you vim-style keyboard scrolling. Grabs the keyboard and emits scroll events via uinput.

| Key | Action |
|-----|--------|
| `j` / `Down` | Scroll down |
| `k` / `Up` | Scroll up |
| `h` / `Left` | Scroll left |
| `l` / `Right` | Scroll right |
| `d` | Half-page down |
| `u` | Half-page up |
| `G` | Jump to bottom |
| `gg` | Jump to top |
| `Escape` | Exit scroll mode |

## Known issues

- **GTK4 apps on Wayland** report (0,0) for all widget positions via AT-SPI. Known upstream bug. These windows get detected and skipped since there's nothing useful to do with broken coords.
- **Terminal emulators** (kitty, alacritty, foot, etc) don't expose AT-SPI trees. Nothing to hint on.
- **Floating windows.** The offset inference assumes tiling. If a window is floating at some random position the hints will be off. Tiled and fullscreen windows work great though.
- **Multi-window layouts.** The offset is calculated from the focused window's size vs the monitor. Master/stack with multiple windows means the focused window is only half the screen, and the inference might not know which half. Single window and monocle layouts work perfectly.

## License

MIT
