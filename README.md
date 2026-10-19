# openclaw-cpp

OpenClaw **C++ rewrite** aiming at **OpenClaw 2026.10.5** feature coverage over time.

**1.4.0-dev — not full parity.** Read [PARITY.md](PARITY.md) (sim vs real vs missing).

## 1.4 highlights

- **`real-sim`**: computer / canvas / desktop via `SimulationBackend` (placeholder PNG + action log)
- **browser.open_url** / tool `browser_open_url`: curl + title (not Playwright)
- **mcp.app.***: list/call tools + resources via `mcp::Client`
- PARITY.md rewritten for brutal honesty

```bash
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
```
