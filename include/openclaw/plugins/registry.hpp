#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace openclaw::plugins {

struct Plugin {
    std::string id;
    std::string name;
    bool enabled = false;
    std::filesystem::path path;
    std::string native_lib;  // basename of .so if any
    bool native_loaded = false;
    int abi = 0;
};

class Registry {
public:
    ~Registry();
    void Scan(const std::filesystem::path& root);
    // dlopen native libs for enabled plugins that declare "native".
    int LoadNatives();
    void UnloadNatives();
    bool SetEnabled(const std::string& id, bool enabled);
    // Invoke native plugin method; returns JSON.
    nlohmann::json InvokeNative(const std::string& id, const std::string& method,
                                const nlohmann::json& params);
    std::vector<Plugin> List() const { return plugins_; }
    nlohmann::json ListJson() const;
    std::filesystem::path Root() const { return root_; }

private:
    struct NativeHandle {
        std::string id;
        void* dl = nullptr;
        using InvokeFn = char* (*)(const char*, const char*, const char**);
        using NameFn = const char* (*)();
        using AbiFn = int (*)();
        using InitFn = int (*)();
        using ShutdownFn = void (*)();
        InvokeFn invoke = nullptr;
        ShutdownFn shutdown = nullptr;
    };
    std::filesystem::path root_;
    std::vector<Plugin> plugins_;
    std::vector<NativeHandle> natives_;
};

}  // namespace openclaw::plugins
