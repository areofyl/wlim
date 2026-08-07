# wlim

vimium-like click hints for wayland. press a keybind, get letter labels on every clickable thing, type a label, click happens. like [homerow](https://www.homerow.app/) but for linux. works on any wlroots-based compositor (dwl, hyprland, sway, etc).

![concept: yellow hint labels over browser UI elements](https://img.shields.io/badge/status-works%20on%20my%20machine-yellow)

## how it works

1. reads the AT-SPI2 accessibility tree of whatever window is focused
2. finds all clickable elements (buttons, links, tabs, inputs, etc)
3. draws a fullscreen transparent overlay using GTK4 + gtk4-layer-shell
4. shows letter labels at each element's position
5. you type the letters, overlay closes, uinput clicks that spot
6. hold shift while typing the last letter to right-click instead
7. hold ctrl while typing the last letter to middle-click instead
8. press `/` to search — type text to filter hints by element name, then press Enter and pick a hint



## dependencies

- gtk4
- gtk4-layer-shell
- at-spi2 (libatspi)
- pkg-config

on fedora:
```
sudo dnf install gtk4-devel gtk4-layer-shell-devel at-spi2-core-devel
```

on gentoo:
```
emerge gui-libs/gtk gui-libs/gtk4-layer-shell app-accessibility/at-spi2-core
```

## build

```
make
```

## setup

**enable accessibility** (needed for AT-SPI to work):
```
gsettings set org.gnome.desktop.interface toolkit-accessibility true
```

**uinput access** — wlim clicks via `/dev/uinput` directly (no ydotool needed). add a udev rule so your user can access it:
```
echo 'KERNEL=="uinput", GROUP="input", MODE="0660"' | sudo tee /etc/udev/rules.d/99-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**chromium/electron apps** need an extra flag to expose their accessibility tree:
```
# add to ~/.config/chromium-flags.conf or equivalent
--force-renderer-accessibility
```

or set the flatpak env if you're using flatpak chromium:
```
flatpak override --user --env=ACCESSIBILITY_ENABLED=1 io.github.nickvision.chromium
```

**compositor keybind** — bind wlim to a key in your compositor config. for example in dwl's `config.h`:
```c
{ MODKEY, XKB_KEY_semicolon, spawn, {.v = (const char*[]){ "/path/to/wlim", NULL }} },
{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_semicolon, spawn, {.v = (const char*[]){ "/path/to/wlim", "--scroll", NULL }} },
```

## config

create `~/.config/wlim/config` to customize. all keys are optional — defaults are used for anything missing.

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
```

## scroll mode

`wlim --scroll` gives you vim-style keyboard scrolling. it grabs the keyboard and emits scroll events via uinput.

| key | action |
|-----|--------|
| `j` / `Down` | scroll down |
| `k` / `Up` | scroll up |
| `h` / `Left` | scroll left |
| `l` / `Right` | scroll right |
| `d` | half-page down |
| `u` | half-page up |
| `G` | jump to bottom |
| `gg` | jump to top |
| `Escape` | exit scroll mode |

## known issues / caveats

- **GTK4 apps on wayland** report (0,0) for all widget positions via AT-SPI (known upstream bug). these windows are skipped since there's no generic way to query window geometry on wayland.
- **terminal emulators** (kitty, alacritty, foot, etc) don't expose AT-SPI trees. nothing to hint on.
- **chromium/electron** coords may be window-relative on some compositors — hints might land at wrong positions if the window isn't at (0,0).

## how the sausage is made

the hard parts were:
- GTK4 on wayland won't give you real widget coordinates through AT-SPI (known upstream bug). these windows are detected and skipped.
- the overlay window has to use `rgba(0,0,0,0.01)` background instead of fully transparent, because wayland compositors drop pointer events on fully transparent surfaces.
- the click has to happen *after* the overlay is fully unmapped by the compositor, otherwise it hits the overlay instead of the target. there's a 150ms sleep for this.
- screen bounds for uinput are read from GDK's monitor API — no compositor-specific IPC needed.

## license

MIT
