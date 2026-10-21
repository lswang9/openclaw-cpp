#include "openclaw/config/schema.hpp"

#include <mutex>
#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

namespace openclaw::config {
namespace {
using json = nlohmann::json;
const char* const kSchemaText =
#include "upstream_schema.inc"
    ;

std::vector<std::string> SplitPath(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : p) {
        if (c == '.' || c == '[' || c == ']') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
// Resolve local $ref (#/definitions/... or #/$defs/...).
const json& Deref(const json& root, const json& node) {
    const json* cur = &node;
    for (int guard = 0; guard < 16 && cur->is_object() && cur->contains("$ref") && (*cur)["$ref"].is_string(); ++guard) {
        auto ref = (*cur)["$ref"].get<std::string>();
        if (ref.rfind("#/", 0) != 0) break;
        const json* t = &root;
        for (auto& part : SplitPath(ref.substr(2))) { if (!t->is_object() || !t->contains(part)) return *cur; t = &(*t)[part]; }
        cur = t;
    }
    return *cur;
}
// Candidate sub-schemas for a node (itself plus anyOf/oneOf/allOf branches).
void Branches(const json& root, const json& n, std::vector<const json*>& out, int depth = 0) {
    const json& d = Deref(root, n);
    if (!d.is_object() || depth > 6) return;
    out.push_back(&d);
    for (auto k : {"anyOf", "oneOf", "allOf"})
        if (d.contains(k) && d[k].is_array()) for (auto& b : d[k]) Branches(root, b, out, depth + 1);
}
const json* Child(const json& root, const json& n, const std::string& key) {
    std::vector<const json*> bs; Branches(root, n, bs);
    for (auto* b : bs) if (b->contains("properties") && (*b)["properties"].contains(key)) return &Deref(root, (*b)["properties"][key]);
    const bool numeric = !key.empty() && key.find_first_not_of("0123456789") == std::string::npos;
    for (auto* b : bs) {
        if ((key == "*" || numeric) && b->contains("items") && (*b)["items"].is_object()) return &Deref(root, (*b)["items"]);
        if (b->contains("additionalProperties") && (*b)["additionalProperties"].is_object()) return &Deref(root, (*b)["additionalProperties"]);
        if (b->contains("patternProperties") && (*b)["patternProperties"].is_object() && !(*b)["patternProperties"].empty())
            return &Deref(root, (*b)["patternProperties"].begin().value());
    }
    return nullptr;
}
std::string TypeOf(const json& v) {
    if (v.is_null()) return "null"; if (v.is_boolean()) return "boolean"; if (v.is_number_integer() || v.is_number_unsigned()) return "integer";
    if (v.is_number()) return "number"; if (v.is_string()) return "string"; if (v.is_array()) return "array"; return "object";
}
bool TypeMatches(const json& t, const std::string& actual) {
    auto one = [&](const std::string& s) { return s == actual || (s == "number" && actual == "integer"); };
    if (t.is_string()) return one(t.get<std::string>());
    if (t.is_array()) { for (auto& x : t) if (x.is_string() && one(x.get<std::string>())) return true; return false; }
    return true;
}
bool Matches(const json& root, const json& s, const json& v);
void Check(const json& root, const json& schema, const json& v, const std::string& path, json& issues, std::size_t max) {
    if (issues.size() >= max) return;
    std::vector<const json*> bs; Branches(root, schema, bs);
    if (bs.empty()) return;
    const json& s = *bs.front();
    for (auto k : {"anyOf", "oneOf"}) if (s.contains(k) && s[k].is_array() && !s[k].empty()) {
        bool any = false; for (auto& b : s[k]) any = any || Matches(root, b, v);
        if (!any) issues.push_back({{"path", path}, {"code", "invalid_union"}, {"message", "value does not match any allowed shape"}});
        if (!any) return;
        for (auto& b : s[k]) if (Matches(root, b, v)) { Check(root, b, v, path, issues, max); return; }
    }
    const auto actual = TypeOf(v);
    if (s.contains("type") && !TypeMatches(s["type"], actual)) {
        issues.push_back({{"path", path}, {"code", "invalid_type"}, {"message", "expected " + s["type"].dump() + ", got " + actual}});
        return;
    }
    if (s.contains("enum") && s["enum"].is_array()) {
        bool ok = false; for (auto& e : s["enum"]) ok = ok || e == v;
        if (!ok) issues.push_back({{"path", path}, {"code", "invalid_enum"}, {"message", "must be one of " + s["enum"].dump()}});
    }
    if (s.contains("const") && s["const"] != v) issues.push_back({{"path", path}, {"code", "invalid_literal"}, {"message", "must be " + s["const"].dump()}});
    if (v.is_number()) {
        const double d = v.get<double>();
        if (s.contains("minimum") && d < s["minimum"].get<double>()) issues.push_back({{"path", path}, {"code", "too_small"}, {"message", "must be ≥ " + s["minimum"].dump()}});
        if (s.contains("maximum") && d > s["maximum"].get<double>()) issues.push_back({{"path", path}, {"code", "too_big"}, {"message", "must be ≤ " + s["maximum"].dump()}});
    }
    if (v.is_object()) {
        const bool closed = s.contains("additionalProperties") && s["additionalProperties"] == false;
        for (auto& [k, cv] : v.items()) {
            const auto cp = path.empty() ? k : path + "." + k;
            const json* c = nullptr;
            if (s.contains("properties") && s["properties"].contains(k)) c = &s["properties"][k];
            else if (s.contains("additionalProperties") && s["additionalProperties"].is_object()) c = &s["additionalProperties"];
            else if (s.contains("patternProperties") && !s["patternProperties"].empty()) c = &s["patternProperties"].begin().value();
            if (!c) { if (closed) issues.push_back({{"path", cp}, {"code", "unrecognized_key"}, {"message", "unknown config key"}}); continue; }
            Check(root, *c, cv, cp, issues, max);
        }
        if (s.contains("required") && s["required"].is_array())
            for (auto& r : s["required"]) if (r.is_string() && !v.contains(r.get<std::string>()))
                issues.push_back({{"path", path.empty() ? r.get<std::string>() : path + "." + r.get<std::string>()}, {"code", "required"}, {"message", "required"}});
    }
    if (v.is_array() && s.contains("items") && s["items"].is_object())
        for (std::size_t i = 0; i < v.size(); ++i) Check(root, s["items"], v[i], path + "." + std::to_string(i), issues, max);
}
bool Matches(const json& root, const json& s, const json& v) {
    json tmp = json::array();
    Check(root, s, v, "", tmp, 1);
    return tmp.empty();
}
}  // namespace

const json& UpstreamSchemaResponse() {
    static json doc = json::parse(kSchemaText);
    return doc;
}

json LookupSchema(const std::string& path_in) {
    const auto& doc = UpstreamSchemaResponse();
    const json& root = doc["schema"];
    const std::string path = path_in.empty() || path_in == "." ? "." : path_in;
    const json* node = &root;
    if (path != ".")
        for (auto& part : SplitPath(path)) { node = Child(root, *node, part); if (!node) return nullptr; }
    json shallow = json::object();
    for (auto& [k, v] : node->items()) if (k != "properties" && k != "items" && k != "additionalProperties" && k != "anyOf" && k != "oneOf" && k != "allOf") shallow[k] = v;
    json out{{"path", path}, {"schema", shallow}};
    const auto& hints = doc["uiHints"];
    if (hints.is_object() && hints.contains(path)) { out["hint"] = hints[path]; out["hintPath"] = path; }
    json children = json::array();
    std::vector<const json*> bs; Branches(root, *node, bs);
    std::set<std::string> seen;
    for (auto* b : bs) if (b->contains("properties"))
        for (auto& [k, v] : (*b)["properties"].items()) {
            if (!seen.insert(k).second) continue;
            const json& cv = Deref(root, v);
            const auto cp = path == "." ? k : path + "." + k;
            json c{{"key", k}, {"path", cp}, {"type", cv.value("type", json(nullptr))},
                   {"required", b->contains("required") && std::find((*b)["required"].begin(), (*b)["required"].end(), k) != (*b)["required"].end()},
                   {"hasChildren", cv.contains("properties") || cv.contains("items") || cv.contains("additionalProperties") || cv.contains("anyOf")}};
            if (hints.is_object() && hints.contains(cp)) c["hint"] = hints[cp];
            children.push_back(c);
        }
    out["children"] = children;
    return out;
}

std::size_t SchemaPathCount() {
    static std::size_t n = [] {
        std::size_t c = 0;
        std::function<void(const json&, int)> walk = [&](const json& x, int d) {
            if (!x.is_object() || d > 40) return;
            if (x.contains("properties")) for (auto& [k, v] : x["properties"].items()) { ++c; walk(v, d + 1); }
            for (auto k : {"items", "additionalProperties"}) if (x.contains(k) && x[k].is_object()) walk(x[k], d + 1);
            for (auto k : {"anyOf", "oneOf", "allOf"}) if (x.contains(k)) for (auto& v : x[k]) walk(v, d + 1);
        };
        walk(UpstreamSchemaResponse()["schema"], 0);
        return c;
    }();
    return n;
}

json ValidateAgainstSchema(const json& raw, std::size_t max_issues) {
    json issues = json::array();
    const json& root = UpstreamSchemaResponse()["schema"];
    Check(root, root, raw.is_object() ? raw : json::object(), "", issues, max_issues);
    return issues;
}
}  // namespace openclaw::config
