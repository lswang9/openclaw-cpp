#pragma once
// ClawHub registry client (https://clawhub.ai, /api/v1): search, skill info/install/update/list,
// fingerprint resolve ("sync"), and plugin package artifact fetch for `plugins install clawhub:<pkg>`.
// Registry: CLAWHUB_REGISTRY (or legacy CLAWDHUB_REGISTRY) else https://clawhub.ai.
// Skills install into <workdir>/skills/<slug>; versions recorded in <workdir>/.clawhub/lock.json.
#include <nlohmann/json.hpp>
#include <filesystem>
#include <map>
#include <string>

namespace openclaw::clawhub {

std::string RegistryBase();
std::filesystem::path DefaultWorkdir();  // CLAWHUB_WORKDIR or the agent workspace root

struct HttpResult {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;  // lower-cased names
    std::string error;
};
// GET with optional Bearer CLAWHUB_TOKEN; honours 429 Retry-After (≤10s, up to 2 retries).
HttpResult Get(const std::string& url, const std::filesystem::path& to_file = {});
std::string UrlEncode(const std::string& s);

nlohmann::json Search(const std::string& q, int limit = 10);
nlohmann::json SkillInfo(const std::string& slug);
nlohmann::json InstallSkill(const std::string& slug, const std::string& version, const std::filesystem::path& workdir, bool force);
nlohmann::json UpdateSkills(const std::filesystem::path& workdir, const std::string& slug /* "" = all */);
nlohmann::json ListSkills(const std::filesystem::path& workdir);
nlohmann::json UninstallSkill(const std::string& slug, const std::filesystem::path& workdir);
// For each skill dir under <workdir>/skills: fingerprint → GET /api/v1/resolve → {match, latest, status}.
nlohmann::json SyncSkills(const std::filesystem::path& workdir);
std::string Fingerprint(const std::filesystem::path& dir);

// Resolve a plugin package version, check /security (blockedFromDownload), download the artifact
// (npm-pack .tgz preferred, verified against artifactSha256; legacy zip extracted, package/ root).
// Returns {ok, path, version, artifact{kind, sha256}, name} with path inside `tmp`.
nlohmann::json FetchPackage(const std::string& spec, const std::filesystem::path& tmp);

}  // namespace openclaw::clawhub
