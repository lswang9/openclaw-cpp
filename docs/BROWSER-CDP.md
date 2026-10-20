# Browser / CDP (openclaw-cpp 1.6)

## What is real vs simulated

| Path | When | `impl` | Capabilities |
|------|------|--------|--------------|
| **CDP** | Chrome/Chromium listening on remote debugging port, or `OPENCLAW_BROWSER_CDP_MOCK=1` | `real` (live) / `real-sim` (mock) | navigate, evaluate JS, PNG screenshot, title/URL |
| **curl-session** | CDP down / unreachable | `real-sim` | HTTP(S) fetch, cookie jar, redirects, `<title>`, link/form extract — **not** a live DOM |

This is **not** Playwright and **not** full OpenClaw browser automation.

## Start Chrome for live CDP

```bash
# dedicated profile so it does not fight your daily Chrome
google-chrome --headless=new --remote-debugging-port=9222 \
  --user-data-dir=/tmp/oc-chrome-cdp --no-first-run --disable-gpu \
  about:blank &

export OPENCLAW_BROWSER_CDP_URL=http://127.0.0.1:9222
# Discovery uses PUT /json/new (Chrome 155+) then /json/list
# optional explicit page WS:
# export OPENCLAW_BROWSER_CDP_URL=ws://127.0.0.1:9222/devtools/page/<id>
```

Then gateway / tools:

- RPC: `browser.navigate`, `browser.evaluate`, `browser.screenshot`, `browser.status`, `browser.open_url`
- Tools: `browser`, `browser_open_url`, `browser_evaluate`, `browser_screenshot`

## Mock (CI / self-test)

```bash
export OPENCLAW_BROWSER_CDP_MOCK=1
./build/build/Release/openclaw-cpp self-test
```

No Chromium required; navigate/eval/screenshot return canned results.

## Env

| Variable | Meaning |
|----------|---------|
| `OPENCLAW_BROWSER_CDP_URL` | `http://HOST:PORT` (discover via `/json/new` or `/json/list`) or full `ws://…` page URL. Default `http://127.0.0.1:9222`. |
| `OPENCLAW_BROWSER_CDP_MOCK` | `1`/`true` — in-process fake CDP. |

## Honest limits

- No bundled browser binary; you must start Chrome/Chromium yourself (or use mock).
- No multi-tab orchestration UI, no Playwright selectors, no network interception API.
- curl fallback does not execute JavaScript.
