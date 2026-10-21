#include "openclaw/clawhub/client.hpp"

#include "openclaw/tools/exec.hpp"
#include "openclaw/tools/runenv.hpp"
#include "openclaw/util.hpp"
#include "openclaw/workspace/workspace.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace openclaw::clawhub {
namespace fs = std::filesystem;
namespace {

nlohmann::json Fail(const std::string& e) { return {{"ok", false}, {"error", e}}; }
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Q(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}
size_t WriteStr(char* p, size_t s, size_t n, void* ud) { static_cast<std::string*>(ud)->append(p, s * n); return s * n; }
size_t WriteFile(char* p, size_t s, size_t n, void* ud) { return std::fwrite(p, s, n, static_cast<FILE*>(ud)) * s; }
size_t Header(char* p, size_t s, size_t n, void* ud) {
    std::string line(p, s * n);
    auto c = line.find(':');
    if (c != std::string::npos) {
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char x) { return std::tolower(x); });
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        (*static_cast<std::map<std::string, std::string>*>(ud))[k] = v;
    }
    return s * n;
}
fs::path TempDir() {
    std::random_device rd;
    auto p = fs::temp_directory_path() / ("oc-clawhub-" + std::to_string(rd()));
    fs::create_directories(p);
    return p;
}
struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; if (!p.empty()) fs::remove_all(p, e); } };

nlohmann::json GetJson(const std::string& path, std::string* err, long* status = nullptr) {
    auto r = Get(RegistryBase() + path);
    if (status) *status = r.status;
    if (!r.error.empty()) { *err = r.error; return nullptr; }
    if (r.status < 200 || r.status >= 300) { *err = "HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 300); return nullptr; }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_discarded()) { *err = "invalid JSON from registry"; return nullptr; }
    return j;
}
// "@owner/slug" → {owner, slug}
std::pair<std::string, std::string> SplitSlug(std::string s) {
    if (s.rfind("clawhub:", 0) == 0) s = s.substr(8);
    if (!s.empty() && s[0] == '@') {
        auto sl = s.find('/');
        if (sl != std::string::npos) return {s.substr(1, sl - 1), s.substr(sl + 1)};
    }
    return {"", s};
}
bool ValidSlug(const std::string& s) {
    if (s.empty() || s.size() > 128 || s == "." || s == "..") return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; });
}
fs::path LockPath(const fs::path& wd) { return wd / ".clawhub" / "lock.json"; }
nlohmann::json ReadLock(const fs::path& wd) {
    std::ifstream f(LockPath(wd));
    auto j = f ? nlohmann::json::parse(f, nullptr, false) : nlohmann::json();
    if (!j.is_object()) j = {{"version", 1}, {"skills", nlohmann::json::object()}};
    if (!j.contains("skills") || !j["skills"].is_object()) j["skills"] = nlohmann::json::object();
    return j;
}
void WriteLock(const fs::path& wd, const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(LockPath(wd).parent_path(), ec);
    auto tmp = LockPath(wd).string() + ".tmp";
    { std::ofstream o(tmp); o << j.dump(2) << "\n"; }
    fs::rename(tmp, LockPath(wd), ec);
}
std::string RejectEscapes(const fs::path& root) {
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(root, fs::directory_options::none, ec))
        if (e.is_symlink()) return "archive contains a symlink (refused): " + fs::relative(e.path(), root, ec).string();
    return "";
}
fs::path FindSkillRoot(const fs::path& ex) {
    std::error_code ec;
    if (fs::exists(ex / "SKILL.md", ec)) return ex;
    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(ex, ec)) if (e.is_directory()) dirs.push_back(e.path());
    if (dirs.size() == 1 && fs::exists(dirs[0] / "SKILL.md", ec)) return dirs[0];
    return {};
}

}  // namespace

std::string RegistryBase() {
    std::string b = EnvOr("CLAWHUB_REGISTRY", EnvOr("CLAWDHUB_REGISTRY", "https://clawhub.ai"));
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}
fs::path DefaultWorkdir() {
    auto w = EnvOr("CLAWHUB_WORKDIR", "");
    return w.empty() ? workspace::Workspace::DefaultRoot() : fs::path(w);
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

HttpResult Get(const std::string& url, const fs::path& to_file) {
    HttpResult r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        r = HttpResult{};
        CURL* c = curl_easy_init();
        if (!c) { r.error = "curl init failed"; return r; }
        curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Accept: application/json, application/zip, application/octet-stream");
        std::string tok = EnvOr("CLAWHUB_TOKEN", "");
        if (!tok.empty()) hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + tok).c_str());
        FILE* fp = nullptr;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "openclaw-cpp-clawhub/1");
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, Header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
        if (!to_file.empty()) {
            fp = std::fopen(to_file.c_str(), "wb");
            if (!fp) { r.error = "cannot write " + to_file.string(); curl_slist_free_all(hdrs); curl_easy_cleanup(c); return r; }
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteFile);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
        } else {
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteStr);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
        }
        auto rc = curl_easy_perform(c);
        if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
        if (fp) std::fclose(fp);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        if (!to_file.empty() && r.status >= 400) {  // error bodies are plain text
            std::ifstream f(to_file);
            std::stringstream ss; ss << f.rdbuf(); r.body = ss.str().substr(0, 2000);
        }
        if (r.status != 429) break;
        int wait = 1;
        if (auto it = r.headers.find("retry-after"); it != r.headers.end()) wait = std::atoi(it->second.c_str());
        else if (auto it2 = r.headers.find("ratelimit-reset"); it2 != r.headers.end()) wait = std::atoi(it2->second.c_str());
        if (wait > 10) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, wait) * 1000 + 50 * attempt));
    }
    return r;
}

nlohmann::json Search(const std::string& q, int limit) {
    std::string err;
    auto j = GetJson("/api/v1/search?q=" + UrlEncode(q) + "&limit=" + std::to_string(limit) + "&nonSuspiciousOnly=true", &err);
    if (j.is_null()) return Fail(err);
    return {{"ok", true}, {"registry", RegistryBase()}, {"results", j.value("results", nlohmann::json::array())}};
}

nlohmann::json SkillInfo(const std::string& spec) {
    auto [owner, slug] = SplitSlug(spec);
    if (!ValidSlug(slug)) return Fail("invalid skill slug: " + spec);
    std::string err;
    auto j = GetJson("/api/v1/skills/" + UrlEncode(slug) + (owner.empty() ? "" : "?ownerHandle=" + UrlEncode(owner)), &err);
    if (j.is_null()) return Fail(err);
    j["ok"] = true;
    return j;
}

nlohmann::json InstallSkill(const std::string& spec, const std::string& version_in, const fs::path& wd, bool force) {
    auto [owner, slug] = SplitSlug(spec);
    if (!ValidSlug(slug)) return Fail("invalid skill slug: " + spec);
    auto info = SkillInfo(spec);
    if (!info.value("ok", false)) return info;
    if (info.contains("moderation") && info["moderation"].is_object() && info["moderation"].value("isMalwareBlocked", false))
        return Fail("skill is blocked by ClawHub moderation (malware)");
    std::string version = version_in;
    if (version.empty() && info.contains("latestVersion") && info["latestVersion"].is_object())
        version = info["latestVersion"].value("version", "");
    if (version.empty()) return Fail("skill has no public version");
    const auto tmp = TempDir();
    Cleanup cl{tmp};
    auto zip = tmp / "skill.zip";
    std::string url = RegistryBase() + "/api/v1/download?slug=" + UrlEncode(slug) + "&version=" + UrlEncode(version) +
                      (owner.empty() ? "" : "&ownerHandle=" + UrlEncode(owner));
    auto r = Get(url, zip);
    if (!r.error.empty()) return Fail(r.error);
    if (r.status != 200) return Fail("download HTTP " + std::to_string(r.status) + ": " + r.body);
    if (auto ct = r.headers["content-type"]; ct.find("json") != std::string::npos)
        return Fail("skill is a GitHub source handoff (sourceRef public-github); not supported by this client yet");
    auto ex = tmp / "x";
    fs::create_directories(ex);
    auto pr = tools::RunProcess("unzip -qq -o " + Q(zip.string()) + " -d " + Q(ex.string()), tmp.string(), 60000);
    if (pr.exit_code != 0) return Fail("unzip failed: " + pr.out);
    if (auto e = RejectEscapes(ex); !e.empty()) return Fail(e);
    auto root = FindSkillRoot(ex);
    if (root.empty()) return Fail("archive has no SKILL.md");
    std::error_code ec;
    const auto dest = wd / "skills" / slug;
    fs::create_directories(dest.parent_path(), ec);
    if (fs::exists(dest, ec)) {
        if (!force) return Fail("skill already installed: " + slug + " (use --force or update)");
        fs::remove_all(dest, ec);
    }
    const auto staging = dest.string() + ".staging";
    fs::remove_all(staging, ec);
    fs::copy(root, staging, fs::copy_options::recursive, ec);
    if (ec) return Fail("copy failed: " + ec.message());
    fs::create_directories(fs::path(staging) / ".clawhub", ec);
    nlohmann::json origin{{"registry", RegistryBase()}, {"slug", slug}, {"owner", owner}, {"version", version},
                          {"installedAt", NowMs()}};
    { std::ofstream o(fs::path(staging) / ".clawhub" / "origin.json"); o << origin.dump(2) << "\n"; }
    fs::rename(staging, dest, ec);
    if (ec) return Fail("finalize failed: " + ec.message());
    auto lock = ReadLock(wd);
    lock["skills"][slug] = {{"version", version}, {"installedAt", NowMs()}, {"owner", owner}, {"registry", RegistryBase()},
                            {"fingerprint", Fingerprint(dest)}};
    WriteLock(wd, lock);
    return {{"ok", true}, {"slug", slug}, {"version", version}, {"path", dest.string()}, {"lock", LockPath(wd).string()}};
}

nlohmann::json ListSkills(const fs::path& wd) {
    auto lock = ReadLock(wd);
    return {{"ok", true}, {"workdir", wd.string()}, {"skills", lock["skills"]}};
}

nlohmann::json UninstallSkill(const std::string& spec, const fs::path& wd) {
    auto [owner, slug] = SplitSlug(spec);
    if (!ValidSlug(slug)) return Fail("invalid skill slug");
    std::error_code ec;
    auto lock = ReadLock(wd);
    bool had = lock["skills"].contains(slug);
    fs::remove_all(wd / "skills" / slug, ec);
    lock["skills"].erase(slug);
    WriteLock(wd, lock);
    return had ? nlohmann::json{{"ok", true}, {"removed", slug}} : Fail("not installed via ClawHub: " + slug);
}

nlohmann::json UpdateSkills(const fs::path& wd, const std::string& only) {
    auto lock = ReadLock(wd);
    nlohmann::json out = nlohmann::json::array();
    bool all_ok = true;
    for (auto& [slug, rec] : lock["skills"].items()) {
        if (!only.empty() && SplitSlug(only).second != slug) continue;
        std::string owner = rec.value("owner", "");
        auto info = SkillInfo(owner.empty() ? slug : "@" + owner + "/" + slug);
        if (!info.value("ok", false)) { out.push_back({{"slug", slug}, {"ok", false}, {"error", info.value("error", "")}}); all_ok = false; continue; }
        std::string latest = info.contains("latestVersion") && info["latestVersion"].is_object() ? info["latestVersion"].value("version", "") : "";
        std::string cur = rec.value("version", "");
        if (latest.empty() || latest == cur) { out.push_back({{"slug", slug}, {"ok", true}, {"status", "up-to-date"}, {"version", cur}}); continue; }
        // local edits guard: refuse to clobber a modified install unless forced via reinstall
        auto fp = Fingerprint(wd / "skills" / slug);
        if (rec.contains("fingerprint") && rec["fingerprint"] != fp) {
            out.push_back({{"slug", slug}, {"ok", false}, {"status", "locally-modified"}, {"version", cur}, {"latest", latest}});
            all_ok = false;
            continue;
        }
        auto r = InstallSkill(owner.empty() ? slug : "@" + owner + "/" + slug, latest, wd, true);
        out.push_back({{"slug", slug}, {"ok", r.value("ok", false)}, {"status", r.value("ok", false) ? "updated" : "failed"},
                       {"from", cur}, {"to", latest}, {"error", r.value("error", "")}});
        all_ok = all_ok && r.value("ok", false);
    }
    return {{"ok", all_ok}, {"results", out}};
}

std::string Fingerprint(const fs::path& dir) {
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> files;
    for (auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        auto rel = fs::relative(e.path(), dir, ec).generic_string();
        if (rel.rfind(".clawhub/", 0) == 0 || rel.find("/.clawhub/") != std::string::npos) continue;
        files.emplace_back(rel, tools::exec::FileSha256(e.path()));
    }
    std::sort(files.begin(), files.end());
    std::string acc;
    for (auto& [p, h] : files) acc += p + ":" + h + "\n";
    // sha256 of the manifest text
    auto t = TempDir();
    Cleanup cl{t};
    { std::ofstream o(t / "m", std::ios::binary); o << acc; }
    return tools::exec::FileSha256(t / "m");
}

nlohmann::json SyncSkills(const fs::path& wd) {
    std::error_code ec;
    auto lock = ReadLock(wd);
    nlohmann::json out = nlohmann::json::array();
    for (auto& e : fs::directory_iterator(wd / "skills", ec)) {
        if (!e.is_directory() || !fs::exists(e.path() / "SKILL.md", ec)) continue;
        const auto slug = e.path().filename().string();
        if (!ValidSlug(slug)) continue;
        auto fp = Fingerprint(e.path());
        std::string err;
        long st = 0;
        auto j = GetJson("/api/v1/resolve?slug=" + UrlEncode(slug) + "&hash=" + fp, &err, &st);
        nlohmann::json row{{"slug", slug}, {"fingerprint", fp}, {"tracked", lock["skills"].contains(slug)}};
        if (j.is_null()) { row["status"] = st == 404 ? "unknown-to-registry" : "error"; if (st != 404) row["error"] = err; }
        else {
            std::string match = j.contains("match") && j["match"].is_object() ? j["match"].value("version", "") : "";
            std::string latest = j.contains("latestVersion") && j["latestVersion"].is_object() ? j["latestVersion"].value("version", "") : "";
            row["match"] = match;
            row["latest"] = latest;
            row["status"] = match.empty() ? "modified-or-unpublished" : (match == latest ? "up-to-date" : "outdated");
            if (!match.empty() && !lock["skills"].contains(slug)) {  // adopt untracked matching installs into the lock
                lock["skills"][slug] = {{"version", match}, {"installedAt", NowMs()}, {"registry", RegistryBase()}, {"fingerprint", fp}, {"adopted", true}};
                row["adopted"] = true;
            }
        }
        out.push_back(row);
    }
    WriteLock(wd, lock);
    return {{"ok", true}, {"registry", RegistryBase()}, {"skills", out}};
}

nlohmann::json FetchPackage(const std::string& spec_in, const fs::path& tmp) {
    std::string spec = spec_in.rfind("clawhub:", 0) == 0 ? spec_in.substr(8) : spec_in;
    std::string name = spec, version;
    if (auto at = spec.rfind('@'); at != std::string::npos && at > 0) { name = spec.substr(0, at); version = spec.substr(at + 1); }
    if (name.empty()) return Fail("missing package name");
    const std::string ename = UrlEncode(name);
    std::string err;
    if (version.empty() || version == "latest") {
        auto p = GetJson("/api/v1/packages/" + ename, &err);
        if (p.is_null()) return Fail("package lookup failed: " + err);
        const auto& pk = p.contains("package") && p["package"].is_object() ? p["package"] : p;
        version = pk.value("latestVersion", "");
        if (version.empty() && p.contains("latestVersion") && p["latestVersion"].is_object()) version = p["latestVersion"].value("version", "");
        if (version.empty() && pk.contains("tags") && pk["tags"].is_object()) version = pk["tags"].value("latest", "");
        if (version.empty()) return Fail("package has no published version");
    }
    const std::string base = "/api/v1/packages/" + ename + "/versions/" + UrlEncode(version);
    auto sec = GetJson(base + "/security", &err);
    if (!sec.is_null() && sec.contains("trust") && sec["trust"].is_object() && sec["trust"].value("blockedFromDownload", false))
        return Fail("ClawHub blocked this release: " + sec["trust"].value("reasons", nlohmann::json::array()).dump());
    auto art = GetJson(base + "/artifact", &err);
    if (art.is_null()) return Fail("artifact resolve failed: " + err);
    const auto& a = art.contains("artifact") && art["artifact"].is_object() ? art["artifact"] : art;
    const std::string kind = a.value("kind", "legacy-zip");
    std::string want = a.value("sha256", "");
    if (want.empty() && !sec.is_null() && sec.contains("release")) want = sec["release"].value("artifactSha256", "");
    fs::path file = tmp / (kind == "npm-pack" ? "package.tgz" : "package.zip");
    auto r = Get(RegistryBase() + base + "/artifact/download", file);
    if (!r.error.empty()) return Fail(r.error);
    if (r.status != 200) return Fail("artifact download HTTP " + std::to_string(r.status) + ": " + r.body);
    std::string got = tools::exec::FileSha256(file);
    if (auto h = r.headers.find("x-clawhub-artifact-sha256"); h != r.headers.end() && want.empty()) want = h->second;
    if (!want.empty() && got != want) return Fail("artifact sha256 mismatch (got " + got + ", want " + want + ")");
    nlohmann::json meta{{"kind", kind}, {"sha256", got}, {"verified", !want.empty()}};
    if (kind == "npm-pack") return {{"ok", true}, {"path", file.string()}, {"name", name}, {"version", version}, {"artifact", meta}};
    auto ex = tmp / "zx";
    fs::create_directories(ex);
    auto pr = tools::RunProcess("unzip -qq -o " + Q(file.string()) + " -d " + Q(ex.string()), tmp.string(), 60000);
    if (pr.exit_code != 0) return Fail("unzip failed: " + pr.out);
    if (auto e = RejectEscapes(ex); !e.empty()) return Fail(e);
    std::error_code ec;
    fs::path root = fs::exists(ex / "package", ec) ? ex / "package" : ex;
    return {{"ok", true}, {"path", root.string()}, {"name", name}, {"version", version}, {"artifact", meta}};
}

}  // namespace openclaw::clawhub
