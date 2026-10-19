# openclaw-cpp

C++ rewrite toward **OpenClaw 2026.10.5** features over time.

**1.5.0-dev — not full parity.** [PARITY.md](PARITY.md) · [docs/RELEASE.md](docs/RELEASE.md)

## 1.5 highlights

- Agent loop: **timeout_ms**, **cancel** on gateway stop, **per-session locks**
- `POST /v1/rpc`, `POST /v1/sessions`
- `scripts/integration-smoke.sh` (mock provider, no live LLM)
- Easy catalog gaps → local stores (board/portal/setup/…); still not upstream UI

```bash
cmake --preset conan-release && cmake --build --preset conan-release -j
./build/build/Release/openclaw-cpp self-test
./scripts/integration-smoke.sh
```
