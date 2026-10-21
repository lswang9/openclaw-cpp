#include "openclaw/canvas/a2ui.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

namespace openclaw::canvas::a2ui {
namespace fs = std::filesystem;
using J = nlohmann::json;
namespace {
const char* kKinds[] = {"surfaceUpdate", "dataModelUpdate", "beginRendering", "deleteSurface"};
std::string Esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) { case '<': o += "&lt;"; break; case '>': o += "&gt;"; break; case '&': o += "&amp;"; break;
                     case '"': o += "&quot;"; break; case '\'': o += "&#39;"; break; default: o += c; }
    }
    return o;
}
std::optional<std::string> CheckOne(const J& m) {
    if (!m.is_object()) return "message must be an object";
    if (m.contains("createSurface") || m.contains("updateComponents") || m.contains("updateDataModel"))
        return "A2UI v0.9 messages (createSurface/updateComponents/updateDataModel) are not supported; use v0.8";
    int n = 0; std::string kind;
    for (auto* k : kKinds) if (m.contains(k)) { ++n; kind = k; }
    if (n != 1) return "expected exactly one of surfaceUpdate|dataModelUpdate|beginRendering|deleteSurface";
    const auto& b = m[kind];
    if (!b.is_object() || !b.contains("surfaceId") || !b["surfaceId"].is_string() || b["surfaceId"].get<std::string>().empty())
        return kind + ".surfaceId required";
    if (kind == "surfaceUpdate") {
        if (!b.contains("components") || !b["components"].is_array()) return "surfaceUpdate.components must be an array";
        for (auto& c : b["components"])
            if (!c.is_object() || !c.contains("id") || !c["id"].is_string() || !c.contains("component") || !c["component"].is_object() || c["component"].size() != 1)
                return "surfaceUpdate.components[] needs {id, component:{<Type>:{...}}}";
    }
    if (kind == "beginRendering" && (!b.contains("root") || !b["root"].is_string())) return "beginRendering.root required";
    if (kind == "dataModelUpdate" && (!b.contains("contents") || !b["contents"].is_array())) return "dataModelUpdate.contents must be an array";
    return std::nullopt;
}
J ContentsToJson(const J& contents) {
    J o = J::object();
    for (auto& e : contents) {
        if (!e.is_object() || !e.contains("key")) continue;
        const auto k = e["key"].get<std::string>();
        if (e.contains("valueString")) o[k] = e["valueString"];
        else if (e.contains("valueNumber")) o[k] = e["valueNumber"];
        else if (e.contains("valueBoolean")) o[k] = e["valueBoolean"];
        else if (e.contains("valueMap")) o[k] = ContentsToJson(e["valueMap"]);
    }
    return o;
}
J::json_pointer Ptr(const std::string& path) {
    std::string p = path.empty() || path == "/" ? "" : (path[0] == '/' ? path : "/" + path);
    try { return J::json_pointer(p); } catch (...) { return J::json_pointer(""); }
}
std::string Join(const std::string& scope, const std::string& path) {
    if (path.empty()) return scope;
    if (path[0] == '/') return path;
    return scope + "/" + path;
}
struct Renderer {
    const J& surf;
    const std::string& sid;
    int depth = 0;
    J Lookup(const std::string& scope, const std::string& path) const {
        try { auto p = Ptr(Join(scope, path)); if (surf["data"].contains(p)) return surf["data"][p]; } catch (...) {}
        return nullptr;
    }
    std::string Str(const J& bv, const std::string& scope) const {
        if (bv.is_string()) return bv.get<std::string>();
        if (!bv.is_object()) return "";
        if (bv.contains("path")) {
            auto v = Lookup(scope, bv["path"].get<std::string>());
            if (v.is_string()) return v.get<std::string>();
            if (!v.is_null()) return v.dump();
        }
        if (bv.contains("literalString")) return bv["literalString"].get<std::string>();
        if (bv.contains("literalNumber")) return bv["literalNumber"].dump();
        if (bv.contains("literalBoolean")) return bv["literalBoolean"].get<bool>() ? "true" : "false";
        return "";
    }
    bool Bool(const J& bv, const std::string& scope) const {
        if (bv.is_object() && bv.contains("path")) { auto v = Lookup(scope, bv["path"].get<std::string>()); if (v.is_boolean()) return v.get<bool>(); }
        return bv.is_object() && bv.value("literalBoolean", false);
    }
    std::string Children(const J& ch, const std::string& scope) {
        std::string o;
        if (!ch.is_object()) return o;
        if (ch.contains("explicitList")) for (auto& id : ch["explicitList"]) o += Node(id.get<std::string>(), scope);
        else if (ch.contains("template")) {
            const auto& t = ch["template"];
            const auto base = Join(scope, t.value("dataBinding", std::string()));
            auto list = Lookup("", base);
            if (list.is_object()) for (auto& [k, v] : list.items()) o += Node(t.value("componentId", std::string()), base + "/" + k);
            else if (list.is_array()) for (std::size_t i = 0; i < list.size(); ++i) o += Node(t.value("componentId", std::string()), base + "/" + std::to_string(i));
        }
        return o;
    }
    std::string Node(const std::string& id, const std::string& scope) {
        if (++depth > 64) { --depth; return "<!-- depth -->"; }
        std::string o;
        const auto& comps = surf["components"];
        if (!comps.contains(id)) { --depth; return "<div class=\"a2-missing\">missing component " + Esc(id) + "</div>"; }
        const auto& wrap = comps[id];
        const auto type = wrap.begin().key();
        const auto& p = wrap.begin().value();
        const std::string attr = " data-id=\"" + Esc(id) + "\" data-scope=\"" + Esc(scope) + "\"";
        if (type == "Text") {
            const auto h = p.value("usageHint", std::string("body"));
            const std::string tag = (h.size() == 2 && h[0] == 'h' && h[1] >= '1' && h[1] <= '5') ? h : (h == "caption" ? "small" : "p");
            o = "<" + tag + attr + " class=\"a2-text\">" + Esc(Str(p.value("text", J()), scope)) + "</" + tag + ">";
        } else if (type == "Image") {
            auto url = Str(p.value("url", J()), scope);
            if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 || url.rfind("data:image/", 0) == 0)
                o = "<img" + attr + " class=\"a2-img\" src=\"" + Esc(url) + "\" alt=\"\"/>";
        } else if (type == "Icon") {
            o = "<span" + attr + " class=\"a2-icon\">[" + Esc(Str(p.value("name", J()), scope)) + "]</span>";
        } else if (type == "Row" || type == "Column" || type == "List") {
            const std::string cls = type == "Row" ? "a2-row" : type == "List" ? "a2-list" : "a2-col";
            o = "<div" + attr + " class=\"" + cls + "\">" + Children(p.value("children", J()), scope) + "</div>";
        } else if (type == "Card") {
            o = "<div" + attr + " class=\"a2-card\">" + (p.contains("child") ? Node(p["child"].get<std::string>(), scope) : "") + "</div>";
        } else if (type == "Divider") {
            o = "<hr" + attr + " class=\"a2-div\"/>";
        } else if (type == "Button") {
            o = "<button" + attr + " class=\"a2-btn\" data-surface=\"" + Esc(sid) + "\">" +
                (p.contains("child") ? Node(p["child"].get<std::string>(), scope) : "") + "</button>";
        } else if (type == "CheckBox") {
            o = "<label" + attr + " class=\"a2-check\"><input class=\"a2-in\" data-surface=\"" + Esc(sid) + "\" type=\"checkbox\"" + (Bool(p.value("value", J()), scope) ? " checked" : "") +
                "/> " + Esc(Str(p.value("label", J()), scope)) + "</label>";
        } else if (type == "TextField") {
            o = "<label" + attr + " class=\"a2-field\">" + Esc(Str(p.value("label", J()), scope)) + " <input class=\"a2-in\" data-surface=\"" + Esc(sid) + "\" type=\"text\" value=\"" +
                Esc(Str(p.value("text", J()), scope)) + "\"/></label>";
        } else if (type == "Tabs") {
            o = "<div" + attr + " class=\"a2-tabs\">";
            if (p.contains("tabItems")) for (auto& t : p["tabItems"])
                o += "<section><h4>" + Esc(Str(t.value("title", J()), scope)) + "</h4>" + (t.contains("child") ? Node(t["child"].get<std::string>(), scope) : "") + "</section>";
            o += "</div>";
        } else {
            o = "<div" + attr + " class=\"a2-unsupported\">unsupported component " + Esc(type) + "</div>";
        }
        --depth;
        return o;
    }
};
}  // namespace

Validation ValidateMessages(const J& arr) {
    Validation v;
    if (!arr.is_array()) { v.error = "messages must be an array"; return v; }
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (auto e = CheckOne(arr[i])) { v.error = "message " + std::to_string(i + 1) + ": " + *e; return v; }
        v.messages.push_back(arr[i]);
    }
    if (v.messages.empty()) { v.error = "no A2UI messages"; return v; }
    v.message_count = v.messages.size();
    v.ok = true;
    return v;
}

Validation ValidateJsonl(const std::string& text) {
    Validation v;
    std::istringstream is(text);
    std::string line;
    int ln = 0;
    J arr = J::array();
    while (std::getline(is, line)) {
        ++ln;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        J m;
        try { m = J::parse(line); } catch (...) { v.error = "line " + std::to_string(ln) + ": invalid JSON"; return v; }
        if (auto e = CheckOne(m)) { v.error = "line " + std::to_string(ln) + ": " + *e; return v; }
        arr.push_back(m);
    }
    return ValidateMessages(arr);
}

std::string BuildTextJsonl(const std::string& text, const std::string& sid) {
    J su = {{"surfaceUpdate", {{"surfaceId", sid}, {"components", J::array({
        {{"id", "root"}, {"component", {{"Column", {{"children", {{"explicitList", J::array({"content"})}}}}}}}},
        {{"id", "content"}, {"component", {{"Text", {{"text", {{"literalString", text}}}, {"usageHint", "body"}}}}}}})}}}};
    J br = {{"beginRendering", {{"surfaceId", sid}, {"root", "root"}}}};
    return su.dump() + "\n" + br.dump() + "\n";
}

Host::Host(fs::path file) : file_(std::move(file)) {
    std::ifstream f(file_);
    try { if (f) state_ = J::parse(f); } catch (...) {}
    if (!state_.is_object()) state_ = J::object();
    if (!state_.contains("surfaces")) state_["surfaces"] = J::object();
    if (!state_.contains("rev")) state_["rev"] = 0;
}

void Host::Save() const {
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    std::ofstream(file_, std::ios::trunc) << state_.dump();
}

nlohmann::json Host::Apply(const std::vector<J>& msgs) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& ss = state_["surfaces"];
    for (auto& m : msgs) {
        const auto kind = m.begin().key();
        const auto& b = m.begin().value();
        const auto sid = b["surfaceId"].get<std::string>();
        if (kind == "deleteSurface") { ss.erase(sid); continue; }
        auto& s = ss[sid];
        if (!s.contains("components")) { s["components"] = J::object(); s["data"] = J::object(); s["rendering"] = false; }
        if (kind == "surfaceUpdate") for (auto& c : b["components"]) s["components"][c["id"].get<std::string>()] = c["component"];
        else if (kind == "dataModelUpdate") {
            auto obj = ContentsToJson(b["contents"]);
            const auto path = b.value("path", std::string());
            auto ptr = Ptr(path);
            if (ptr.empty()) { for (auto& [k, v] : obj.items()) s["data"][k] = v; }
            else {
                try {
                    auto& tgt = s["data"][ptr];
                    if (!tgt.is_object()) tgt = J::object();
                    for (auto& [k, v] : obj.items()) tgt[k] = v;
                } catch (...) {}
            }
        } else if (kind == "beginRendering") {
            s["root"] = b["root"]; s["rendering"] = true;
            if (b.contains("styles")) s["styles"] = b["styles"];
            if (b.contains("catalogId")) s["catalogId"] = b["catalogId"];
        }
    }
    state_["rev"] = state_["rev"].get<std::uint64_t>() + 1;
    Save();
    J names = J::array();
    for (auto& [k, v] : ss.items()) names.push_back(k);
    return {{"ok", true}, {"applied", msgs.size()}, {"surfaces", names}, {"rev", state_["rev"]}, {"version", "v0.8"}};
}

nlohmann::json Host::Reset() {
    std::lock_guard<std::mutex> lk(mu_);
    state_["surfaces"] = J::object();
    state_["rev"] = state_["rev"].get<std::uint64_t>() + 1;
    Save();
    return {{"ok", true}, {"rev", state_["rev"]}};
}

nlohmann::json Host::State() const { std::lock_guard<std::mutex> lk(mu_); return state_; }
std::uint64_t Host::Revision() const { std::lock_guard<std::mutex> lk(mu_); return state_["rev"].get<std::uint64_t>(); }

std::string Host::RenderSurface(const std::string& sid) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!state_["surfaces"].contains(sid)) return "";
    const auto& s = state_["surfaces"][sid];
    if (!s.value("rendering", false)) return "";
    Renderer r{s, sid};
    return "<div class=\"a2-surface\" data-surface=\"" + Esc(sid) + "\">" + r.Node(s.value("root", std::string()), "") + "</div>";
}

std::string Host::RenderPage() const {
    std::vector<std::string> ids;
    std::uint64_t rev;
    { std::lock_guard<std::mutex> lk(mu_); for (auto& [k, v] : state_["surfaces"].items()) ids.push_back(k); rev = state_["rev"]; }
    std::string body;
    for (auto& id : ids) body += RenderSurface(id);
    if (body.empty()) body = "<p class=\"a2-empty\">No A2UI surface is rendering. Push v0.8 JSONL with canvas.a2ui.pushJSONL.</p>";
    return "<!doctype html><html><head><meta charset=\"utf-8\"><title>OpenClaw A2UI</title><style>"
           "body{font-family:system-ui,sans-serif;margin:16px;background:#0f1115;color:#e6e6e6}.a2-row{display:flex;gap:8px;align-items:center}"
           ".a2-col,.a2-list{display:flex;flex-direction:column;gap:8px}.a2-card{border:1px solid #333;border-radius:8px;padding:12px;background:#171a21}"
           ".a2-btn{padding:6px 12px;border-radius:6px;border:1px solid #555;background:#2a62ff;color:#fff;cursor:pointer}.a2-btn *{margin:0;color:inherit}"
           ".a2-img{max-width:100%}.a2-unsupported,.a2-missing{color:#e88}</style></head><body data-rev=\"" + std::to_string(rev) + "\">" + body +
           "<script>document.addEventListener('click',async e=>{const b=e.target.closest('.a2-btn');if(!b)return;"
           "const r=await fetch('/__openclaw__/a2ui/action',{method:'POST',headers:{'Content-Type':'application/json'},"
           "body:JSON.stringify({surfaceId:b.dataset.surface,componentId:b.dataset.id,scope:b.dataset.scope})});b.dataset.status=r.status;});"
           "document.addEventListener('change',async e=>{const i=e.target.closest('.a2-in');if(!i)return;const w=i.closest('[data-id]');"
           "await fetch('/__openclaw__/a2ui/input',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({surfaceId:i.dataset.surface,"
           "componentId:w.dataset.id,scope:w.dataset.scope,value:i.type==='checkbox'?i.checked:i.value})});});"
           "let rev=document.body.dataset.rev;setInterval(async()=>{try{const j=await (await fetch('/__openclaw__/a2ui/state')).json();"
           "if(String(j.rev)!==rev)location.reload();}catch(e){}},1500);</script></body></html>";
}

nlohmann::json Host::SetInput(const std::string& sid, const std::string& cid, const std::string& scope, const J& value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& ss = state_["surfaces"];
    if (!ss.contains(sid) || !ss[sid]["components"].contains(cid)) return {{"ok", false}, {"error", "no such component"}};
    auto& s = ss[sid];
    const auto& w = s["components"][cid];
    const auto type = w.begin().key();
    const auto& p = w.begin().value();
    std::string key = type == "TextField" ? "text" : type == "CheckBox" ? "value" : "";
    if (key.empty()) return {{"ok", false}, {"error", "component is not an input"}};
    if (!p.contains(key) || !p[key].is_object() || !p[key].contains("path")) return {{"ok", false}, {"error", "input has no bound path"}};
    if (type == "TextField" && !value.is_string()) return {{"ok", false}, {"error", "TextField value must be a string"}};
    if (type == "CheckBox" && !value.is_boolean()) return {{"ok", false}, {"error", "CheckBox value must be boolean"}};
    const auto path = Join(scope, p[key]["path"].get<std::string>());
    try { s["data"][Ptr(path)] = value; } catch (...) { return {{"ok", false}, {"error", "bad path"}}; }
    Save();  // no rev bump: avoid reloading the page under the user's cursor
    return {{"ok", true}, {"path", path}, {"value", value}};
}

std::optional<nlohmann::json> Host::ResolveAction(const std::string& sid, const std::string& cid, const std::string& scope) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!state_["surfaces"].contains(sid)) return std::nullopt;
    const auto& s = state_["surfaces"][sid];
    if (!s["components"].contains(cid)) return std::nullopt;
    const auto& w = s["components"][cid];
    if (w.begin().key() != "Button") return std::nullopt;
    const auto& p = w.begin().value();
    if (!p.contains("action")) return std::nullopt;
    const auto& a = p["action"];
    Renderer r{s, sid};
    J ctx = J::object();
    if (a.contains("context") && a["context"].is_array())
        for (auto& e : a["context"]) {
            if (!e.contains("key")) continue;
            const auto& v = e.value("value", J());
            J out;
            if (v.is_object() && v.contains("path")) out = r.Lookup(scope, v["path"].get<std::string>());
            else if (v.is_object() && v.contains("literalNumber")) out = v["literalNumber"];
            else if (v.is_object() && v.contains("literalBoolean")) out = v["literalBoolean"];
            else out = r.Str(v, scope);
            ctx[e["key"].get<std::string>()] = out;
        }
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return J{{"userAction", {{"name", a.value("name", std::string())}, {"surfaceId", sid}, {"sourceComponentId", cid},
                             {"timestamp", ts}, {"context", ctx}}}};
}

}  // namespace openclaw::canvas::a2ui
