#include "openclaw/auth/keychain.hpp"

#include "openclaw/util.hpp"

#include <dlfcn.h>

#include <mutex>

namespace openclaw::auth::keychain {
namespace {
// Mirrors libsecret's public ABI (secret-schema.h); stable since libsecret 0.x.
struct SchemaAttr { const char* name; int type; };  // SECRET_SCHEMA_ATTRIBUTE_STRING = 0
struct Schema {
    const char* name;
    int flags;  // SECRET_SCHEMA_NONE = 0
    SchemaAttr attributes[32];
    int reserved;
    void* reserved1; void* reserved2; void* reserved3; void* reserved4; void* reserved5; void* reserved6; void* reserved7;
};
struct GErr { unsigned domain; int code; char* message; };

using StoreFn = int (*)(const Schema*, const char*, const char*, const char*, void*, GErr**, ...);
using LookupFn = char* (*)(const Schema*, void*, GErr**, ...);
using ClearFn = int (*)(const Schema*, void*, GErr**, ...);
using FreeFn = void (*)(char*);
using ErrFreeFn = void (*)(GErr*);

const Schema kSchema{"ai.openclaw.AuthProfile", 0, {{"profile", 0}, {"field", 0}, {nullptr, 0}}, 0,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

struct Lib {
    void* h = nullptr;
    StoreFn store = nullptr; LookupFn lookup = nullptr; ClearFn clear = nullptr; FreeFn pfree = nullptr; ErrFreeFn efree = nullptr;
    bool ok = false;
    std::string why;
};
std::once_flag g_once;
Lib g_lib;
std::mutex g_mu;

std::string TakeErr(GErr* e) {
    if (!e) return {};
    std::string m = e->message ? e->message : "error";
    if (g_lib.efree) g_lib.efree(e);
    return m;
}

void Init() {
    const std::string mode = EnvOr("OPENCLAW_KEYCHAIN", "auto");
    if (mode == "off" || mode == "file") { g_lib.why = "disabled (OPENCLAW_KEYCHAIN=" + mode + ")"; return; }
    if (EnvOr("DBUS_SESSION_BUS_ADDRESS", "").empty()) { g_lib.why = "no D-Bus session bus"; return; }
    g_lib.h = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib.h) { g_lib.why = "libsecret-1.so.0 not found"; return; }
    g_lib.store = reinterpret_cast<StoreFn>(dlsym(g_lib.h, "secret_password_store_sync"));
    g_lib.lookup = reinterpret_cast<LookupFn>(dlsym(g_lib.h, "secret_password_lookup_sync"));
    g_lib.clear = reinterpret_cast<ClearFn>(dlsym(g_lib.h, "secret_password_clear_sync"));
    g_lib.pfree = reinterpret_cast<FreeFn>(dlsym(g_lib.h, "secret_password_free"));
    g_lib.efree = reinterpret_cast<ErrFreeFn>(dlsym(g_lib.h, "g_error_free"));
    if (!g_lib.efree) if (void* g = dlopen("libglib-2.0.so.0", RTLD_NOW)) g_lib.efree = reinterpret_cast<ErrFreeFn>(dlsym(g, "g_error_free"));
    if (!g_lib.store || !g_lib.lookup || !g_lib.clear || !g_lib.pfree) { g_lib.why = "libsecret symbols missing"; return; }
    // Probe: the Secret Service must accept a write (fails when no daemon / locked collection).
    GErr* e = nullptr;
    if (!g_lib.store(&kSchema, "default", "openclaw probe", "1", nullptr, &e, "profile", "__probe__", "field", "probe", nullptr)) {
        g_lib.why = "secret service unavailable: " + TakeErr(e);
        return;
    }
    g_lib.clear(&kSchema, nullptr, &e, "profile", "__probe__", "field", "probe", nullptr);
    TakeErr(e);
    g_lib.ok = true;
}
}  // namespace

bool Available() { std::call_once(g_once, Init); return g_lib.ok; }
std::string Backend() { return Available() ? "libsecret" : "file"; }
std::string UnavailableReason() { Available(); return g_lib.ok ? "" : g_lib.why; }

bool Store(const std::string& id, const std::string& field, const std::string& secret) {
    if (!Available()) return false;
    std::lock_guard l(g_mu);
    GErr* e = nullptr;
    const std::string label = "OpenClaw " + id + " " + field;
    int ok = g_lib.store(&kSchema, "default", label.c_str(), secret.c_str(), nullptr, &e, "profile", id.c_str(), "field", field.c_str(), nullptr);
    TakeErr(e);
    return ok != 0;
}

std::optional<std::string> Lookup(const std::string& id, const std::string& field) {
    if (!Available()) return std::nullopt;
    std::lock_guard l(g_mu);
    GErr* e = nullptr;
    char* p = g_lib.lookup(&kSchema, nullptr, &e, "profile", id.c_str(), "field", field.c_str(), nullptr);
    TakeErr(e);
    if (!p) return std::nullopt;
    std::string s = p;
    g_lib.pfree(p);
    return s;
}

bool Clear(const std::string& id, const std::string& field) {
    if (!Available()) return false;
    std::lock_guard l(g_mu);
    GErr* e = nullptr;
    int ok = g_lib.clear(&kSchema, nullptr, &e, "profile", id.c_str(), "field", field.c_str(), nullptr);
    TakeErr(e);
    return ok != 0;
}

}  // namespace openclaw::auth::keychain
