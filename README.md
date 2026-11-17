# hyprland-zoomparent

yabai's `--toggle zoom-parent` for Hyprland's dwindle layout: the focused
window grows over the area of its parent split, covering its sibling, and
shrinks back on the next toggle.

Built for Hyprland 0.57 (Lua config).

## Install with hyprpm

```sh
hyprpm update
hyprpm add https://github.com/Zaxiure/hyprland-zoomparent.git
hyprpm enable zoomparent
```

hyprpm rebuilds it against the new headers when you run `hyprpm update` after
a Hyprland update. Load enabled plugins at startup with `hyprpm reload -n`.

## Use

```lua
hl.bind(mainMod .. " + z", hl.dsp.layout("zoomparent"))
```

## Build by hand

```sh
make -C zoomparent all
hyprctl plugin load "$PWD/zoomparent/zoomparent.so"
```
