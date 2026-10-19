#include "openclaw/plugins/sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int openclaw_plugin_abi(void) { return OPENCLAW_PLUGIN_ABI; }
const char* openclaw_plugin_name(void) { return "sample_echo"; }
int openclaw_plugin_init(void) { return 0; }
void openclaw_plugin_shutdown(void) {}

char* openclaw_plugin_invoke(const char* method, const char* params_json, const char** err_out) {
    if (err_out) *err_out = NULL;
    if (!method) method = "";
    if (!params_json) params_json = "{}";
    /* Build compact JSON: {"ok":true,"method":"...","echo":...} — escape minimally by wrapping raw */
    size_t need = strlen(method) + strlen(params_json) + 64;
    char* out = (char*)malloc(need);
    if (!out) {
        if (err_out) *err_out = "oom";
        return NULL;
    }
    snprintf(out, need, "{\"ok\":true,\"plugin\":\"sample_echo\",\"method\":\"%s\",\"params\":%s}",
             method, params_json);
    return out;
}
