# Computer capture + input (1.10)

## Screenshot

| Condition | `impl` | Engine examples |
|-----------|--------|-----------------|
| `OPENCLAW_SCREENSHOT_FORCE_SIM=1` or `OPENCLAW_COMPUTER_FORCE_SIM=1` | `real-sim` | placeholder 1×1 PNG |
| No `DISPLAY` / `WAYLAND_DISPLAY` | `real-sim` | placeholder |
| Display + tool works | **`real`** | `ffmpeg-x11grab`, `scrot`, `import`, `gnome-screenshot`, `grim` |

Tools tried (order): scrot → import → gnome-screenshot → grim → ffmpeg x11grab.

## Input (click / type / move / key)

| Condition | `impl` | Engine |
|-----------|--------|--------|
| `OPENCLAW_INPUT_FORCE_SIM=1` or `OPENCLAW_COMPUTER_FORCE_SIM=1` | `real-sim` | event-log only |
| No display / no tool | `real-sim` | event-log |
| Display + **xdotool** (or ydotool) | **`real`** | `xdotool` / `ydotool` |

Gateway methods: `computer.click`, `computer.type`, `computer.move`, `computer.key`, plus `computer.invoke` actions.

Canvas / desktop launch remain **`real-sim`**.

## Env

| Variable | Effect |
|----------|--------|
| `OPENCLAW_COMPUTER_FORCE_SIM` | Force sim for **both** screenshot and input |
| `OPENCLAW_SCREENSHOT_FORCE_SIM` | Screenshot placeholder only |
| `OPENCLAW_INPUT_FORCE_SIM` | Input event-log only |

Install tip: `xdotool` (X11), `ffmpeg` for screenshots.
