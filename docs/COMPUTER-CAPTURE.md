# Computer screenshot capture (1.9)

## Behavior

| Condition | `impl` | Engine examples |
|-----------|--------|-----------------|
| `OPENCLAW_SCREENSHOT_FORCE_SIM=1` | `real-sim` | placeholder 1×1 PNG |
| No `DISPLAY` / `WAYLAND_DISPLAY` | `real-sim` | placeholder |
| Display set + tool works | **`real`** | `ffmpeg-x11grab`, `scrot`, `import`, `gnome-screenshot`, `grim` |
| Display set but all tools fail | `real-sim` | placeholder + `attempts` |

Mouse/keyboard/canvas remain **`real-sim`** (no OS input injection).

## Tools tried (in order)

1. `scrot`
2. ImageMagick `import -window root`
3. `gnome-screenshot -f`
4. `grim` (if Wayland)
5. `ffmpeg -f x11grab` (X11)

Install tip (Debian/Ubuntu): `scrot` or `ffmpeg` (often already present).
