# acolorp

A small, dependency-light tool written in C that picks a color palette from a wallpaper and writes ready-to-use theme files for your desktop, in the spirit of [pywal](https://github.com/dylanaraps/pywal) and [wallust](https://codeberg.org/explosion-mental/wallust).

Run it with an image and you get `color0`–`color7`, a `background` and a `foreground`, written into config files you can include in Sway, Hyprland, kitty, waybar, shell scripts and more. No templates to write, but you can add your own if you want.

## Features

- Single C file, only needs [`stb_image.h`](https://github.com/nothings/stb) to build
- Built-in output files for Sway, Hyprland, kitty, GTK/waybar CSS, web CSS, shell and JSON
- Choose which files to write with `--only`
- Optional custom `.tpl` templates with simple `{{color1}}` placeholders
- Optional compositor reload (`--reload sway|hyprland`)
- Files are written atomically (temp file, then rename), so a config never sees a half-written file
- Never edits your own config files; it only writes into its own output folder

## Build

Clone the repository:

```bash
git clone https://github.com/el10ta1d3rs0n/AColorP.git
cd AColorP
```

Get `stb_image.h` and put it next to `acolorp.c`:

```bash
curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
# or on Arch: sudo pacman -S stb   (then add -I/usr/include/stb to the gcc line)
```

Compile:

```bash
gcc -O2 -Wall -Wextra -o acolorp acolorp.c -lm
```

Optionally install it:

```bash
sudo install -m755 acolorp /usr/local/bin/
```

Supported image formats are whatever `stb_image` reads: JPEG, PNG, BMP, GIF (first frame), TGA, PSD, HDR, PIC and PNM. WebP is not supported.

## Quick start

```bash
./acolorp ~/Wallpapers/wall.png
```

This prints the palette and writes all built-in files to `~/.config/acolorp/`. Then add **one line** to your config (only once):

| Compositor | Line to add |
|---|---|
| Sway | `include ~/.config/acolorp/colors-sway.conf` |
| Hyprland | `source = ~/.config/acolorp/colors-hyprland.conf` |

To test without touching anything, write to a throwaway folder:

```bash
./acolorp --output /tmp/out ~/Wallpapers/wall.png
```

## Usage

```
acolorp [options] image-file

  --output DIR      where files are written (default: ~/.config/acolorp)
  --only LIST       write only these built-in files (comma-separated, e.g. sway,kitty)
                    use "none" to skip all built-in files
  --list            show the built-in files and exit
  --reload NAME     reload compositor afterwards (hyprland or sway)
  --templates DIR   optional: also render your own *.tpl files
```

Examples:

```bash
acolorp wall.png                          # all built-in files
acolorp wall.png --only sway              # just colors-sway.conf
acolorp wall.png --only sway,kitty        # several files
acolorp wall.png --only sway --reload sway
acolorp wall.png --only none --templates ~/mytemplates
acolorp --list
```

## Built-in files

| Short name | File | Use for |
|---|---|---|
| `hyprland` | `colors-hyprland.conf` | Hyprland variables and border colors |
| `sway` | `colors-sway.conf` | Sway variables and `client.*` window colors |
| `gtk` | `colors-gtk.css` | waybar and other GTK-style CSS (`@define-color`) |
| `css` | `colors.css` | web CSS (`var(--color1)`) |
| `sh` | `colors.sh` | shell scripts (`source colors.sh`) |
| `json` | `colors.json` | anything that reads JSON |
| `kitty` | `colors-kitty.conf` | kitty terminal |

## Setup examples

### Sway

Put the include near the top of your config, before anything that uses `$color*`:

```
include ~/.config/acolorp/colors-sway.conf
```

The generated file sets `client.focused`, `client.focused_inactive` and `client.unfocused`. Remove your own `client.*` lines, otherwise a line placed after the include overrides the generated one. You can also use the variables elsewhere, for example in the bar:

```
bar {
    colors {
        statusline         $foreground
        background         $background
        focused_workspace  $color1 $color1 $background
        inactive_workspace $background $background $foreground
    }
}
```

### Hyprland

```ini
source = ~/.config/acolorp/colors-hyprland.conf
```

The generated file defines `$color0`–`$color7`, `$background`, `$foreground`, and sets the active and inactive border colors. Use the variables anywhere else in your config.

### kitty

```
include ~/.config/acolorp/colors-kitty.conf
```

### waybar

GTK CSS does not expand `~` or `$HOME`, so the import needs the full path. This command adds the correct line with your real home directory at the top of your `style.css`:

```bash
sed -i "1i @import \"$HOME/.config/acolorp/colors-gtk.css\";" ~/.config/waybar/style.css
```

Then use the colors in the same file:

```css
window#waybar { background: @background; color: @foreground; }
```

### Shell scripts

```bash
source ~/.config/acolorp/colors.sh
echo "$color1"
```

## Applying a new wallpaper

`acolorp` only generates files. To change the theme with your wallpaper, call it from whatever script sets the wallpaper:

```bash
acolorp "$image" --reload sway
```

`--reload` runs `swaymsg reload` or `hyprctl reload` after the files are written, so the new colors show up immediately. It only re-reads the config; it does not close or restart your windows. Skip it when testing or when you are not inside a running Sway or Hyprland session.

## Custom templates

Point `--templates` at a folder of files ending in `.tpl`. Each one is rendered to the output folder with the `.tpl` suffix removed, so `mytheme.conf.tpl` becomes `mytheme.conf`.

Placeholders:

| Placeholder | Result |
|---|---|
| `{{color0}}` … `{{color7}}` | `#RRGGBB` |
| `{{background}}`, `{{foreground}}` | `#RRGGBB` |
| `{{color1.hex}}` | `RRGGBB` (no `#`, e.g. for Hyprland `rgb(...)`) |
| `{{color1.rgb}}` | `R,G,B` in decimal (e.g. for `rgba(...)`) |

Unknown placeholders are left in the output unchanged.

Example, `border.conf.tpl`:

```
border_color = {{color1}}
rgba_color   = rgba({{color1.rgb}},0.8)
```

## How it works

1. The image is loaded and each pixel is placed into one of 32768 color bins (5 bits per channel).
2. Very dark pixels (brightness below 40) are skipped so shadows do not dominate. If the image is so dark that nothing is left, it retries without the filter.
3. The bins are sorted by pixel count. The most common colors are picked, skipping any color too close to one already picked (minimum distance 32 in RGB).
4. If fewer than 8 distinct colors exist, the missing slots are filled with lighter versions of the ones found.
5. `background` is the darkest palette color scaled down to 20%. `foreground` is the lightest palette color blended 80% toward white.

The palette is picked by pixel count, so a wallpaper that is mostly one tone gives a palette of similar colors. A more varied image gives a more varied palette. You can tune the behavior with the `MIN_COLOR_DISTANCE` and `MIN_BRIGHTNESS` constants at the top of `acolorp.c`.

## Revert

`acolorp` never modifies your config. To go back to normal, remove the `include` or `source` line you added, reload your compositor, and delete the generated files:

```bash
rm -rf ~/.config/acolorp
```

## License

MIT, see [LICENSE](LICENSE). `stb_image.h` is not part of this repository; it is by Sean Barrett and is available under MIT or public domain (see the header of the file).
