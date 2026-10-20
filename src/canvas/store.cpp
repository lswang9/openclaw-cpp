#include "openclaw/canvas/store.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

namespace openclaw::canvas {
namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// nlohmann value() throws type_error.306 if the key exists but is null.
std::string StrField(const nlohmann::json& j, const char* key, const std::string& fallback = {}) {
    if (!j.contains(key) || j[key].is_null()) return fallback;
    if (j[key].is_string()) return j[key].get<std::string>();
    return fallback;
}

nlohmann::json DefaultDoc(const std::string& id, const std::string& title) {
    return {{"id", id},
            {"title", title},
            {"html", "<h1>" + title + "</h1><p>Local canvas (not full A2UI).</p>"},
            {"widgets", nlohmann::json::array()},
            {"meta", {{"impl", "real"}, {"note", "disk-backed local canvas"}}},
            {"updated_ms", NowMs()},
            {"created_ms", NowMs()}};
}

}  // namespace

Store::Store(std::filesystem::path dir) : dir_(std::move(dir)) {
    std::filesystem::create_directories(dir_);
    LoadIndex();
    LoadEvents();
    if (!index_.contains("docs") || !index_["docs"].is_array() || index_["docs"].empty()) {
        auto id = NextId();
        auto doc = DefaultDoc(id, "sim-canvas");
        PersistDoc(doc);
        index_ = {{"version", 1},
                  {"active_id", id},
                  {"docs", nlohmann::json::array({{{"id", id},
                                                   {"title", doc["title"]},
                                                   {"updated_ms", doc["updated_ms"]}}})}};
        SaveIndex();
        RecordEvent("canvas.bootstrap", {{"id", id}}, id);
    }
}

std::filesystem::path Store::DocPath(const std::string& id) const {
    return dir_ / (id + ".json");
}

void Store::LoadIndex() {
    auto p = dir_ / "index.json";
    if (!std::filesystem::exists(p)) {
        index_ = {{"version", 1}, {"active_id", ""}, {"docs", nlohmann::json::array()}};
        return;
    }
    std::ifstream in(p);
    try {
        in >> index_;
    } catch (...) {
        index_ = {{"version", 1}, {"active_id", ""}, {"docs", nlohmann::json::array()}};
    }
    if (!index_.contains("docs") || !index_["docs"].is_array())
        index_["docs"] = nlohmann::json::array();
    seq_ = static_cast<int>(index_["docs"].size()) + 1;
}

void Store::SaveIndex() const {
    std::ofstream out(dir_ / "index.json");
    out << index_.dump(2);
}

void Store::LoadEvents() {
    events_.clear();
    auto p = dir_ / "events.json";
    if (!std::filesystem::exists(p)) return;
    std::ifstream in(p);
    try {
        nlohmann::json j;
        in >> j;
        if (j.is_array()) {
            for (auto& e : j) events_.push_back(std::move(e));
        }
    } catch (...) {
        events_.clear();
    }
}

void Store::SaveEvents() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : events_) arr.push_back(e);
    std::ofstream out(dir_ / "events.json");
    out << arr.dump(2);
}

std::string Store::NextId() { return "canvas-" + std::to_string(seq_++); }

nlohmann::json* Store::Find(const std::string& id) {
    if (!index_.contains("docs") || !index_["docs"].is_array()) return nullptr;
    for (auto& d : index_["docs"]) {
        if (StrField(d, "id") == id) return &d;
    }
    return nullptr;
}

const nlohmann::json* Store::Find(const std::string& id) const {
    if (!index_.contains("docs") || !index_["docs"].is_array()) return nullptr;
    for (const auto& d : index_["docs"]) {
        if (StrField(d, "id") == id) return &d;
    }
    return nullptr;
}

void Store::PersistDoc(const nlohmann::json& doc) const {
    auto id = StrField(doc, "id");
    if (id.empty()) return;
    std::ofstream out(DocPath(id));
    out << doc.dump(2);
    // Also write .html sibling for easy inspection
    if (doc.contains("html") && doc["html"].is_string()) {
        std::ofstream html(dir_ / (id + ".html"));
        html << doc["html"].get<std::string>();
    }
}

nlohmann::json Store::LoadDocFile(const std::string& id) const {
    auto p = DocPath(id);
    if (!std::filesystem::exists(p)) return nlohmann::json::object();
    std::ifstream in(p);
    try {
        nlohmann::json j;
        in >> j;
        return j;
    } catch (...) {
        return nlohmann::json::object();
    }
}

nlohmann::json Store::Status() const {
    std::lock_guard lock(mu_);
    return {{"impl", "real"},
            {"dir", dir_.string()},
            {"docs", (index_.contains("docs") && index_["docs"].is_array()) ? index_["docs"].size() : 0},
            {"active_id", StrField(index_, "active_id")},
            {"events", events_.size()},
            {"a2ui", false},
            {"note", "local disk canvas — not full upstream canvas/A2UI"}};
}

nlohmann::json Store::List() const {
    std::lock_guard lock(mu_);
    return {{"ok", true},
            {"impl", "real"},
            {"docs", index_.value("docs", nlohmann::json::array())},
            {"active_id", index_.value("active_id", "")}};
}

nlohmann::json Store::Create(const nlohmann::json& spec) {
    std::lock_guard lock(mu_);
    std::string id;
    if (spec.contains("id") && spec["id"].is_string() && !spec["id"].get<std::string>().empty())
        id = spec["id"].get<std::string>();
    else
        id = NextId();
    if (Find(id)) id = NextId();
    auto title = spec.value("title", "canvas");
    auto doc = DefaultDoc(id, title);
    if (spec.contains("html") && spec["html"].is_string()) doc["html"] = spec["html"];
    if (spec.contains("widgets")) doc["widgets"] = spec["widgets"];
    if (spec.contains("meta") && spec["meta"].is_object()) doc["meta"].update(spec["meta"]);
    PersistDoc(doc);
    index_["docs"].push_back(
        {{"id", id}, {"title", doc["title"]}, {"updated_ms", doc["updated_ms"]}});
    if (StrField(index_, "active_id").empty() || spec.value("activate", true))
        index_["active_id"] = id;
    SaveIndex();
    events_.push_back(
        {{"ts_ms", NowMs()}, {"kind", "canvas.create"}, {"canvas_id", id}, {"detail", {{"title", title}}}});
    if (events_.size() > 500) events_.erase(events_.begin());
    SaveEvents();
    return {{"ok", true}, {"impl", "real"}, {"document", doc}};
}

nlohmann::json Store::Get(const std::string& id) const {
    std::lock_guard lock(mu_);
    std::string want = id;
    if (want.empty()) want = StrField(index_, "active_id");
    if (want.empty() || !Find(want))
        return {{"ok", false}, {"error", "not found"}, {"id", id}};
    auto doc = LoadDocFile(want);
    if (doc.empty()) return {{"ok", false}, {"error", "doc file missing"}, {"id", want}};
    return {{"ok", true}, {"impl", "real"}, {"document", doc}};
}

nlohmann::json Store::Patch(const std::string& id, const nlohmann::json& patch) {
    std::lock_guard lock(mu_);
    std::string want = id.empty() ? StrField(index_, "active_id") : id;
    auto* meta = Find(want);
    if (!meta) return {{"ok", false}, {"error", "not found"}, {"id", id}};
    auto doc = LoadDocFile(want);
    if (doc.empty()) doc = DefaultDoc(want, StrField(*meta, "title", "canvas"));
    if (patch.contains("title")) doc["title"] = patch["title"];
    if (patch.contains("html")) doc["html"] = patch["html"];
    if (patch.contains("widgets")) doc["widgets"] = patch["widgets"];
    if (patch.contains("meta") && patch["meta"].is_object()) {
        if (!doc["meta"].is_object()) doc["meta"] = nlohmann::json::object();
        doc["meta"].update(patch["meta"]);
    }
    // merge arbitrary top-level keys except id
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.key() == "id" || it.key() == "title" || it.key() == "html" || it.key() == "widgets" ||
            it.key() == "meta")
            continue;
        doc[it.key()] = it.value();
    }
    doc["updated_ms"] = NowMs();
    PersistDoc(doc);
    (*meta)["title"] = doc["title"];
    (*meta)["updated_ms"] = doc["updated_ms"];
    SaveIndex();
    events_.push_back({{"ts_ms", NowMs()},
                       {"kind", "canvas.patch"},
                       {"canvas_id", want},
                       {"detail", {{"keys", [&] {
                                        nlohmann::json k = nlohmann::json::array();
                                        for (auto it = patch.begin(); it != patch.end(); ++it)
                                            k.push_back(it.key());
                                        return k;
                                    }()}}}});
    if (events_.size() > 500) events_.erase(events_.begin());
    SaveEvents();
    return {{"ok", true}, {"impl", "real"}, {"document", doc}};
}

nlohmann::json Store::Put(const std::string& id, const nlohmann::json& document) {
    nlohmann::json patch = document;
    if (document.contains("document") && document["document"].is_object())
        patch = document["document"];
    return Patch(id, patch);
}

nlohmann::json Store::Remove(const std::string& id) {
    std::lock_guard lock(mu_);
    if (!Find(id)) return {{"ok", false}, {"error", "not found"}, {"id", id}};
    auto& docs = index_["docs"];
    docs.erase(std::remove_if(docs.begin(), docs.end(),
                              [&](const nlohmann::json& d) { return StrField(d, "id") == id; }),
               docs.end());
    std::error_code ec;
    std::filesystem::remove(DocPath(id), ec);
    std::filesystem::remove(dir_ / (id + ".html"), ec);
    if (StrField(index_, "active_id") == id) {
        index_["active_id"] = docs.empty() ? "" : StrField(docs.back(), "id");
    }
    SaveIndex();
    events_.push_back(
        {{"ts_ms", NowMs()}, {"kind", "canvas.delete"}, {"canvas_id", id}, {"detail", nlohmann::json::object()}});
    if (events_.size() > 500) events_.erase(events_.begin());
    SaveEvents();
    return {{"ok", true}, {"impl", "real"}, {"deleted", id}, {"active_id", index_.value("active_id", "")}};
}

nlohmann::json Store::RecordEvent(const std::string& kind, const nlohmann::json& detail,
                                  const std::string& canvas_id) {
    std::lock_guard lock(mu_);
    nlohmann::json e{{"ts_ms", NowMs()},
                     {"kind", kind},
                     {"canvas_id", canvas_id.empty() ? StrField(index_, "active_id") : canvas_id},
                     {"detail", detail}};
    events_.push_back(e);
    if (events_.size() > 500) events_.erase(events_.begin());
    SaveEvents();
    return {{"ok", true}, {"impl", "real"}, {"event", e}};
}

nlohmann::json Store::Events(std::size_t limit, const std::string& canvas_id) const {
    std::lock_guard lock(mu_);
    nlohmann::json arr = nlohmann::json::array();
    for (auto it = events_.rbegin(); it != events_.rend() && arr.size() < limit; ++it) {
        if (!canvas_id.empty() && StrField(*it, "canvas_id") != canvas_id) continue;
        arr.push_back(*it);
    }
    return {{"ok", true}, {"impl", "real"}, {"events", arr}, {"total", events_.size()}};
}

nlohmann::json Store::ActiveView() {
    auto g = Get("");
    nlohmann::json doc = nlohmann::json::object();
    if (g.contains("document") && g["document"].is_object()) doc = g["document"];
    if (!g.value("ok", false)) {
        return nlohmann::json{{"mode", "local"}, {"impl", "real"}, {"document", doc}};
    }
    RecordEvent("canvas.view", nlohmann::json::object(), StrField(doc, "id"));
    return nlohmann::json{{"mode", "local"},
                          {"impl", "real"},
                          {"document", doc},
                          {"persisted", true}};
}

nlohmann::json Store::ActivePut(const nlohmann::json& params) {
    std::string id;
    {
        std::lock_guard lock(mu_);
        id = StrField(index_, "active_id");
    }
    if (id.empty()) {
        auto c = Create(params);
        return {{"mode", "local"},
                {"impl", "real"},
                {"document", (c.contains("document") && c["document"].is_object()) ? c["document"] : nlohmann::json::object()},
                {"saved", true},
                {"persisted", true}};
    }
    auto p = Patch(id, params.contains("document") ? params["document"] : params);
    return {{"mode", "local"},
            {"impl", "real"},
            {"document", (p.contains("document") && p["document"].is_object()) ? p["document"] : nlohmann::json::object()},
            {"saved", p.value("ok", false)},
            {"persisted", true}};
}

nlohmann::json Store::ActivePreview() {
    auto g = Get("");
    nlohmann::json doc = nlohmann::json::object();
    if (g.contains("document") && g["document"].is_object()) doc = g["document"];
    RecordEvent("canvas.preview", nlohmann::json::object(), StrField(doc, "id"));
    std::string html = doc.contains("html") && doc["html"].is_string() ? doc["html"].get<std::string>() : "";
    auto widgets = doc.contains("widgets") && doc["widgets"].is_array() ? doc["widgets"]
                                                                        : nlohmann::json::array();
    return {{"mode", "local"},
            {"impl", "real"},
            {"preview",
             {{"title", StrField(doc, "title")},
              {"widget_count", widgets.size()},
              {"html_len", html.size()},
              {"id", StrField(doc, "id")}}},
            {"note", "HTML/JSON preview — not A2UI compositor"},
            {"persisted", true}};
}

}  // namespace openclaw::canvas
