#include "openclaw/plugins/install.hpp"
#include "openclaw/clawhub/client.hpp"

#include "openclaw/config/config.hpp"
#include "openclaw/tools/runenv.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <regex>

namespace openclaw::plugins {
namespace fs = std::filesystem;

namespace {
std::string Q(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
nlohmann::json ReadJson(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return nullptr;
    try { return nlohmann::json::parse(in); } catch (...) { return nullptr; }
}
bool ValidId(const std::string& id) {
    static const std::regex re("^[A-Za-z0-9@][A-Za-z0-9._@/-]{0,127}$");
    return std::regex_match(id, re) && id.find("..") == std::string::npos;
}
std::string DirName(const std::string& id) {  // scoped npm ids → safe folder
    std::string d = id;
    if (!d.empty() && d[0] == '@') d.erase(0, 1);
    for (auto& c : d) if (c == '/') c = '_';
    return d;
}
fs::path TempDir() {
    std::random_device rd;
    auto p = fs::temp_directory_path() / ("oc-plugin-install-" + std::to_string(rd()));
    fs::create_directories(p);
    return p;
}
nlohmann::json Fail(const std::string& e) { return {{"ok", false}, {"error", e}}; }

// Locate plugin root inside an extracted tree (npm tarballs use package/).
fs::path FindRoot(const fs::path& dir) {
    if (fs::exists(dir / "openclaw.plugin.json") || fs::exists(dir / "package.json") || fs::exists(dir / "plugin.json")) return dir;
    if (fs::exists(dir / "package")) return dir / "package";
    for (auto& e : fs::directory_iterator(dir))
        if (e.is_directory() && (fs::exists(e.path() / "openclaw.plugin.json") || fs::exists(e.path() / "package.json"))) return e.path();
    return {};
}

void SaveInstallRecord(const std::string& id, const nlohmann::json& rec) {
    const auto path = config::DefaultPath();
    auto lr = config::Load(path);
    auto raw = lr.error.empty() && lr.snap.raw.is_object() ? lr.snap.raw : nlohmann::json::object();
    if (rec.is_null()) config::UnsetPath(raw, "plugins.installs." + id);
    else config::SetPath(raw, "plugins.installs." + id, rec);
    // enable on install (upstream: plugins.entries.<id>.enabled)
    if (!rec.is_null() && !config::GetPath(raw, "plugins.entries." + id + ".enabled"))
        config::SetPath(raw, "plugins.entries." + id + ".enabled", true);
    if (rec.is_null()) config::UnsetPath(raw, "plugins.entries." + id);
    auto wr = config::SavePreserving(path, raw);
    if (!wr.value("ok", false)) config::SaveRaw(path, config::ToFileDoc(lr.snap, raw));
}
}  // namespace

fs::path ExtensionsDir() {
    if (const char* d = std::getenv("OPENCLAW_EXTENSIONS_DIR"); d && *d) return d;
    const char* h = std::getenv("HOME");
    return fs::path(h && *h ? h : "/tmp") / ".openclaw" / "extensions";
}

nlohmann::json Install(const std::string& spec, const InstallOptions& opts) {
    if (spec.empty()) return Fail("missing spec");
    std::error_code ec;
    if (spec.rfind("clawhub:", 0) == 0) {
        const auto ctmp = TempDir();
        struct C { fs::path p; ~C() { std::error_code e; fs::remove_all(p, e); } } cc{ctmp};
        auto f = clawhub::FetchPackage(spec, ctmp);
        if (!f.value("ok", false)) return f;
        auto r = Install(f["path"].get<std::string>(), opts);
        if (!r.value("ok", false)) return r;
        auto rec = r["record"];
        rec["source"] = "clawhub";
        rec["spec"] = spec;
        rec["clawhub"] = {{"registry", clawhub::RegistryBase()}, {"package", f["name"]}, {"version", f["version"]}, {"artifact", f["artifact"]}};
        SaveInstallRecord(r["id"].get<std::string>(), rec);
        r["record"] = rec;
        r["source"] = "clawhub";
        return r;
    }
    const auto tmp = TempDir();
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{tmp};
    std::string source;
    fs::path root;
    fs::path local(spec);
    if (fs::is_directory(local, ec)) {
        source = "path";
        root = fs::canonical(local, ec);
    } else {
        fs::path tarball;
        if (fs::is_regular_file(local, ec) && (spec.ends_with(".tgz") || spec.ends_with(".tar.gz") || spec.ends_with(".tar"))) {
            source = "archive";
            tarball = fs::canonical(local, ec);
        } else {
            source = "npm";
            auto pr = tools::RunProcess("npm pack --silent --ignore-scripts --pack-destination " + Q(tmp.string()) + " " + Q(spec),
                                        tmp.string(), 180000, 64 * 1024);
            if (pr.exit_code != 0) return Fail("npm pack failed: " + pr.out);
            for (auto& e : fs::directory_iterator(tmp)) if (e.path().extension() == ".tgz") tarball = e.path();
            if (tarball.empty()) return Fail("npm pack produced no tarball");
        }
        const auto ex = tmp / "x";
        fs::create_directories(ex);
        auto pr = tools::RunProcess("tar -xf " + Q(tarball.string()) + " -C " + Q(ex.string()) + " --no-same-owner", tmp.string(), 60000);
        if (pr.exit_code != 0) return Fail("extract failed: " + pr.out);
        // refuse archives with absolute paths or .. (tar already strips leading /; check result stays under ex)
        for (auto& e : fs::recursive_directory_iterator(ex, fs::directory_options::none, ec))
            if (e.is_symlink()) {
                auto t = fs::weakly_canonical(e.path().parent_path() / fs::read_symlink(e.path(), ec), ec);
                if (t.string().rfind(ex.string(), 0) != 0) return Fail("archive contains symlink escaping the package: " + e.path().string());
            }
        root = FindRoot(ex);
    }
    if (root.empty()) return Fail("no plugin found (need openclaw.plugin.json or package.json)");
    auto manifest = ReadJson(root / "openclaw.plugin.json");
    auto pkg = ReadJson(root / "package.json");
    if (manifest.is_null() && !pkg.is_object()) return Fail("missing openclaw.plugin.json / package.json");
    if (manifest.is_null() && !(pkg.contains("openclaw") && pkg["openclaw"].is_object()))
        return Fail("package is not an OpenClaw plugin (no openclaw.plugin.json and no package.json#openclaw)");
    std::string id = manifest.is_object() ? manifest.value("id", "") : "";
    if (id.empty() && pkg.is_object()) id = pkg.value("name", "");
    if (!ValidId(id)) return Fail("invalid plugin id: " + id);
    const std::string version = pkg.is_object() ? pkg.value("version", "") : (manifest.is_object() ? manifest.value("version", "") : "");

    const auto dest = ExtensionsDir() / DirName(id);
    fs::create_directories(dest.parent_path(), ec);
    if (fs::exists(dest, ec) || fs::is_symlink(dest, ec)) {
        if (!opts.force) return Fail("plugin already installed: " + id + " (use --force)");
        fs::remove_all(dest, ec);
    }
    if (opts.link && source == "path") {
        fs::create_directory_symlink(root, dest, ec);
        if (ec) return Fail("symlink failed: " + ec.message());
    } else {
        const auto staging = dest.string() + ".staging";
        fs::remove_all(staging, ec);
        fs::copy(root, staging, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) return Fail("copy failed: " + ec.message());
        const bool has_deps = pkg.is_object() && pkg.contains("dependencies") && pkg["dependencies"].is_object() &&
                              !pkg["dependencies"].empty();
        if (has_deps && opts.install_deps && !fs::exists(fs::path(staging) / "node_modules")) {
            auto pr = tools::RunProcess("npm install --omit=dev --ignore-scripts --no-audit --no-fund --silent", staging, 300000, 64 * 1024);
            if (pr.exit_code != 0) { fs::remove_all(staging, ec); return Fail("npm install failed: " + pr.out); }
        }
        fs::rename(staging, dest, ec);
        if (ec) return Fail("finalize failed: " + ec.message());
    }
    nlohmann::json rec{{"source", source}, {"spec", source == "npm" ? spec : fs::absolute(spec).string()},
                       {"installPath", dest.string()}, {"version", version}, {"installedAt", NowMs()}};
    if (opts.link) rec["link"] = true;
    SaveInstallRecord(id, rec);
    return {{"ok", true}, {"id", id}, {"installPath", dest.string()}, {"source", source}, {"version", version}, {"record", rec}};
}

nlohmann::json Uninstall(const std::string& id) {
    auto raw = config::LoadOrDefault(config::DefaultPath()).raw;
    auto rec = config::GetPath(raw, "plugins.installs." + id);
    fs::path dest = rec && rec->is_object() && rec->contains("installPath") ? fs::path((*rec)["installPath"].get<std::string>())
                                                                            : ExtensionsDir() / DirName(id);
    if (dest.parent_path() != ExtensionsDir()) return Fail("refusing to remove path outside extensions dir: " + dest.string());
    std::error_code ec;
    bool existed = fs::exists(dest, ec) || fs::is_symlink(dest, ec);
    if (fs::is_symlink(dest, ec)) fs::remove(dest, ec); else fs::remove_all(dest, ec);
    if (!existed && !rec) return Fail("not installed: " + id);
    SaveInstallRecord(id, nullptr);
    return {{"ok", true}, {"id", id}, {"removed", dest.string()}};
}

nlohmann::json Update(const std::string& id) {
    auto raw = config::LoadOrDefault(config::DefaultPath()).raw;
    auto rec = config::GetPath(raw, "plugins.installs." + id);
    if (!rec || !rec->is_object()) return Fail("no install record for " + id);
    InstallOptions o; o.force = true; o.link = rec->value("link", false);
    auto r = Install(rec->value("spec", ""), o);
    if (r.value("ok", false)) r["previousVersion"] = rec->value("version", "");
    return r;
}

nlohmann::json ListInstalls() {
    auto raw = config::LoadOrDefault(config::DefaultPath()).raw;
    auto v = config::GetPath(raw, "plugins.installs");
    return {{"ok", true}, {"dir", ExtensionsDir().string()}, {"installs", v && v->is_object() ? *v : nlohmann::json::object()}};
}

}  // namespace openclaw::plugins
