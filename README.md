<div align="center">

> *"Fiat lux, fiat X11, fiat sessio."*
> ***Ave, Sesare! Ave, imperator!***
❤️‍🔥

</div>

# Sesar

> *"A desktop in pure C, with no absurd dependencies."*

**A complete desktop environment in a single small program.** Written in plain C on top of Xlib and FreeType. No Python, GTK, Qt, Electron or Node.

---

> ⚠️ **Experimental.** Sesar was built for [Gladiator](https://github.com/LexusYTG/gladiator) (a Linux desktop on Android) and has only been tested there. You're free to use it elsewhere, but there are no guarantees. Use at your own risk.

---

## What it is

One binary, `sesar-shell`, that takes the place of the usual pile of desktop tools (a menu launcher, a status bar, a wallpaper setter, and more), each of which normally brings its own dependencies and config files. Sesar handles all of it on its own and sets up [JWM](https://joewing.net/projects/jwm/) as the window manager underneath.

## Why it exists

A desktop on a phone has to be light, self-contained and easy to install. Pulling in a full toolkit like GTK or Qt, plus half a dozen helper programs, works against all three. Sesar does the whole job in one file.

## What you get

- **Start menu** with search, categories and your installed apps (`Super + Space`)
- **Power dialog** to restart the window manager or end the session (`Super + Esc`)
- **System HUD** showing CPU, RAM and battery, always on top
- **Synthwave wallpaper**, drawn on the fly: hexagon logo, horizon, perspective grid, stars
- **Neon theme** used everywhere (cyan, magenta, violet, glowing edges), including your terminal
- **Taskbar** with workspace switcher, window list and clock, plus a right-click menu on the desktop
- **App discovery** that reads standard `.desktop` entries, including translated names
- **Unicode text** with accents, symbols and basic emoji

## How it works

Sesar draws everything itself. Each button, panel, border and glow is painted pixel by pixel onto its own canvas, with smooth edges, and then sent to the screen. Text is rendered with FreeType.

Because there's no toolkit and no theme engine, the neon look is the same on every system and needs no special compositor. Sesar's pop-up windows (menu, power dialog, HUD) are kept out of the window manager's way, stay on top, and have bevelled corners.

## Commands

| Command | What it does |
|---|---|
| `sesar-shell setup` | Generates the JWM config (taskbar, theme, shortcuts, right-click menu) |
| `sesar-shell session` | Applies the terminal theme and starts the desktop |
| `sesar-shell menu` | Opens the start menu |
| `sesar-shell power` | Opens the power / session dialog |
| `sesar-shell hud` | Shows the system HUD |
| `sesar-shell wallpaper` | Paints the wallpaper |
| `sesar-shell xres` | Applies the neon theme to xterm |
| `sesar-shell appmenu` | Prints the app list as a JWM menu |
| `sesar-shell files` | Opens the available file manager |
| `sesar-shell uninstall` | Restores your previous JWM setup |

## Building

You need Xlib, Xext and FreeType2.

```sh
# Ubuntu / Debian
apt install build-essential pkg-config libx11-dev libxext-dev libfreetype-dev

cc -O2 -o sesar-shell sesar-shell.c \
    $(pkg-config --cflags --libs x11 xext freetype2) -lm
```

## Settings

| Variable | Meaning |
|---|---|
| `SESAR_SCALE` | UI size. Default: chosen automatically from your screen. |
| `SESAR_FONT` | Path to a regular TTF font. Default: searches `$PREFIX/share/sesar`, then `/usr/share/fonts`. |
| `SESAR_FONT_BOLD` | Path to a bold TTF font. |
| `SESAR_TERM` | Default terminal. Default: `xterm`. |
| `SESAR_TRAY` | Height of the taskbar, in pixels. |

## Fixed since the first version

The wallpaper no longer disappears. Previously it was freed while the desktop was still using it, which caused a black screen whenever something forced a redraw.

## License

MIT. See `LICENSE`.
