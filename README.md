# wlim

vimium-like click hints for wayland. press a keybind, get letter labels on every clickable thing, type a label, click happens. like [homerow](https://www.homerow.app/) but for linux/hyprland.

![concept: yellow hint labels over browser UI elements](https://img.shields.io/badge/status-works%20on%20my%20machine-yellow)

## how it works

1. reads the AT-SPI2 accessibility tree of whatever window is focused
2. finds all clickable elements (buttons, links, tabs, inputs, etc)
3. draws a fullscreen transparent overlay using GTK4 + gtk4-layer-shell
4. shows letter labels at each element's position
5. you type the letters, overlay closes, ydotool clicks that spot

## dependencies

- gtk4
- gtk4-layer-shell
- at-spi2 (libatspi)
- ydotool (+ ydotoold running)
- hyprctl (hyprland)
- pkg-config

on fedora:
```
sudo dnf install gtk4-devel gtk4-layer-shell-devel at-spi2-core-devel ydotool
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

**ydotool daemon** needs to be running and the socket needs to be accessible:
```
sudo systemctl enable --now ydotool
sudo chmod a+rw /tmp/.ydotool_socket
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

**hyprland keybind** — add to your hyprland config:
```
bind = $mainMod, semicolon, exec, /path/to/wlim
```

## known issues / caveats

- **GTK4 apps on wayland** report (0,0) for all widget positions via AT-SPI. wlim detects this and falls back to distributing hints in a grid over the window. not ideal but functional.
- **terminal emulators** (kitty, alacritty, foot, etc) don't expose AT-SPI trees. nothing to hint on.
- **ydotool coordinate scaling** — the ratio between ydotool absolute coords and screen pixels depends on your monitor setup. currently hardcoded to 2.375 which worked on my dual-monitor hyprland config (2560x1600 + 3840x2160). if clicks land wrong, you'll need to tweak `YDOTOOL_RATIO` in `wlim.c`.
- only tested on hyprland. should work on other wlroots compositors that support gtk4-layer-shell but idk.

## how the sausage is made

the hard parts were:
- GTK4 on wayland won't give you real widget coordinates through AT-SPI (known upstream bug). had to detect broken coords and fall back to a grid layout using window geometry from `hyprctl`.
- ydotool absolute positioning doesn't map 1:1 to screen pixels on hyprland — there's a scale factor that depends on your monitor configuration.
- the overlay window has to use `rgba(0,0,0,0.01)` background instead of fully transparent, because wayland compositors drop pointer events on fully transparent surfaces.
- the click has to happen *after* the overlay is fully unmapped by the compositor, otherwise it hits the overlay instead of the target. there's a 150ms sleep for this.

## license

MIT
