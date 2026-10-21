#pragma once
// `plugins install|uninstall|update|installs`: upstream-style plugin installs into
// ~/.openclaw/extensions/<id> from a local directory, a .tgz tarball, or an npm spec (via `npm pack`).
// Install records live in config `plugins.installs.<id>`.
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

namespace openclaw::plugins {

std::filesystem::path ExtensionsDir();  // OPENCLAW_EXTENSIONS_DIR or $HOME/.openclaw/extensions

struct InstallOptions {
    bool force = false;          // replace an existing install
    bool link = false;           // symlink a local directory instead of copying
    bool install_deps = true;    // npm install --omit=dev --ignore-scripts when package.json has dependencies
};

// spec: directory | *.tgz/*.tar.gz | npm spec (name[@version], file:..., git urls handled by npm).
// Returns {ok, id, installPath, source, version, record} or {ok:false, error}.
nlohmann::json Install(const std::string& spec, const InstallOptions& opts = {});
nlohmann::json Uninstall(const std::string& id);
nlohmann::json Update(const std::string& id);  // re-install from the recorded spec
nlohmann::json ListInstalls();

}  // namespace openclaw::plugins
