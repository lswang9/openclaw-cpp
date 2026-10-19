/* openclaw-cpp Plugin SDK (C ABI) — Linux .so native plugins.
 * ABI version 1. Not full OpenClaw JS plugin parity.
 */
#ifndef OPENCLAW_PLUGINS_SDK_H
#define OPENCLAW_PLUGINS_SDK_H

#ifdef __cplusplus
extern "C" {
#endif

#define OPENCLAW_PLUGIN_ABI 1

/* Return ABI version the plugin was built against. */
int openclaw_plugin_abi(void);

/* Human-readable plugin id/name (static string ok). */
const char* openclaw_plugin_name(void);

/* Optional init; return 0 on success. */
int openclaw_plugin_init(void);

/* Optional shutdown. */
void openclaw_plugin_shutdown(void);

/* Invoke: method + JSON params string → malloc'd JSON result (caller frees with free()).
 * Return NULL on error; optionally set *err_out to a static message.
 */
char* openclaw_plugin_invoke(const char* method, const char* params_json, const char** err_out);

#ifdef __cplusplus
}
#endif
#endif
