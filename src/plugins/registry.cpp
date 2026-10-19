#include "openclaw/plugins/registry.hpp"
#include "openclaw/plugins/sdk.h"

#include <dlfcn.h>
#include <fstream>

namespace openclaw::plugins {
namespace fs = std::filesystem;

Registry::~Registry() { UnloadNatives(); }

void Registry::Scan(const fs::path& root) {
    UnloadNatives();
    plugins_.clear();
    root_ = root;
    if (!fs::exists(root) || !fs::is_directory(root)) return;
    for (const auto& e : fs::directory_iterator(root)) {
        if (!e.is_directory()) continue;
        Plugin p;
        p.id = e.path().filename().string();
        p.name = p.id;
        p.path = e.path();
        p.enabled = false;
        const auto manifest = e.path() / "plugin.json";
        if (fs::exists(manifest)) {
            try {
                std::ifstream in(manifest);
                nlohmann::json j;
                in >> j;
                p.name = j.value("name", p.name);
                p.enabled = j.value("enabled", false);
                p.native_lib = j.value("native", "");
                p.abi = j.value("abi", 0);
            } catch (...) {
            }
        }
        plugins_.push_back(std::move(p));
    }
}

bool Registry::SetEnabled(const std::string& id, bool enabled) {
    for (auto& p : plugins_) {
        if (p.id != id) continue;
        p.enabled = enabled;
        auto manifest = p.path / "plugin.json";
        nlohmann::json j = nlohmann::json::object();
        if (fs::exists(manifest)) {
            try {
                std::ifstream in(manifest);
                in >> j;
            } catch (...) {
            }
        }
        j["name"] = p.name;
        j["enabled"] = enabled;
        if (!p.native_lib.empty()) j["native"] = p.native_lib;
        if (p.abi) j["abi"] = p.abi;
        std::ofstream out(manifest);
        out << j.dump(2) << '\n';
        return true;
    }
    return false;
}

int Registry::LoadNatives() {
    UnloadNatives();
    int n = 0;
    for (auto& p : plugins_) {
        if (!p.enabled || p.native_lib.empty()) continue;
        fs::path so = p.path / p.native_lib;
        if (!fs::exists(so)) {
            // also try build tree sibling
            so = p.path / ("lib" + p.id + ".so");
        }
        if (!fs::exists(so)) continue;
        void* dl = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!dl) continue;
        NativeHandle h;
        h.id = p.id;
        h.dl = dl;
        h.invoke = reinterpret_cast<NativeHandle::InvokeFn>(dlsym(dl, "openclaw_plugin_invoke"));
        auto abi = reinterpret_cast<NativeHandle::AbiFn>(dlsym(dl, "openclaw_plugin_abi"));
        auto init = reinterpret_cast<NativeHandle::InitFn>(dlsym(dl, "openclaw_plugin_init"));
        h.shutdown = reinterpret_cast<NativeHandle::ShutdownFn>(dlsym(dl, "openclaw_plugin_shutdown"));
        if (!h.invoke || !abi || abi() != OPENCLAW_PLUGIN_ABI) {
            dlclose(dl);
            continue;
        }
        if (init && init() != 0) {
            dlclose(dl);
            continue;
        }
        p.native_loaded = true;
        p.abi = OPENCLAW_PLUGIN_ABI;
        natives_.push_back(h);
        ++n;
    }
    return n;
}

void Registry::UnloadNatives() {
    for (auto& h : natives_) {
        if (h.shutdown) h.shutdown();
        if (h.dl) dlclose(h.dl);
    }
    natives_.clear();
    for (auto& p : plugins_) p.native_loaded = false;
}

nlohmann::json Registry::InvokeNative(const std::string& id, const std::string& method,
                                      const nlohmann::json& params) {
    for (auto& h : natives_) {
        if (h.id != id || !h.invoke) continue;
        const char* err = nullptr;
        char* raw = h.invoke(method.c_str(), params.dump().c_str(), &err);
        if (!raw) {
            return {{"ok", false}, {"error", err ? err : "invoke failed"}, {"id", id}};
        }
        nlohmann::json out;
        try {
            out = nlohmann::json::parse(raw);
        } catch (...) {
            out = {{"ok", true}, {"raw", raw}};
        }
        free(raw);
        return out;
    }
    return {{"ok", false}, {"error", "native plugin not loaded"}, {"id", id}};
}

nlohmann::json Registry::ListJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : plugins_) {
        arr.push_back({{"id", p.id},
                       {"name", p.name},
                       {"enabled", p.enabled},
                       {"path", p.path.string()},
                       {"native", p.native_lib},
                       {"native_loaded", p.native_loaded},
                       {"abi", p.abi}});
    }
    return arr;
}

}  // namespace openclaw::plugins
