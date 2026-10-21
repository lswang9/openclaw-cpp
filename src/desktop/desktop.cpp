#include "openclaw/desktop/desktop.hpp"

#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace openclaw::desktop {
namespace fs = std::filesystem;
using J = nlohmann::json;
namespace {
const char* kAtspiPy = R"PY(
import gi, json, sys
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi
req = json.loads(sys.stdin.read() or '{}')
MAXN = int(req.get('max_nodes', 400)); APP = req.get('app') or ''
count = [0]
def info(a, path):
    d = {"path": path}
    try: d["role"] = a.get_role_name()
    except Exception: d["role"] = "?"
    try: d["name"] = a.get_name() or ""
    except Exception: d["name"] = ""
    try: d["states"] = [s.value_nick for s in a.get_state_set().get_states()]
    except Exception: pass
    try:
        e = a.get_extents(Atspi.CoordType.SCREEN); d["bounds"] = [e.x, e.y, e.width, e.height]
    except Exception: pass
    try:
        n = Atspi.Action.get_n_actions(a)
        if n: d["actions"] = [Atspi.Action.get_action_name(a, i) for i in range(n)]
    except Exception: pass
    try:
        t = Atspi.Text.get_text(a, 0, -1)
        if t: d["text"] = t[:500]
    except Exception: pass
    return d
def walk(a, path, depth):
    count[0] += 1
    d = info(a, path)
    kids = []
    if depth < 30:
        try: n = a.get_child_count()
        except Exception: n = 0
        for i in range(n):
            if count[0] >= MAXN: d["truncated"] = True; break
            c = a.get_child_at_index(i)
            if c is None: continue
            if depth == 0 and APP and (c.get_name() or '') != APP: continue
            kids.append(walk(c, path + [i], depth + 1))
    if kids: d["children"] = kids
    return d
def find(path):
    a = Atspi.get_desktop(0)
    for i in path:
        a = a.get_child_at_index(int(i))
        if a is None: return None
    return a
op = req.get('op', 'tree')
if op == 'tree':
    print(json.dumps({"ok": True, "tree": walk(Atspi.get_desktop(0), [], 0), "nodes": count[0]}))
else:
    a = find(req.get('path', []))
    if a is None: print(json.dumps({"ok": False, "error": "no node at path"})); sys.exit(0)
    before = info(a, req.get('path', []))
    if 'text' in req:
        try:
            ok = Atspi.EditableText.set_text_contents(a, req['text'])
            print(json.dumps({"ok": bool(ok), "node": before, "did": "set_text"})); sys.exit(0)
        except Exception as e:
            print(json.dumps({"ok": False, "error": "set_text failed: %s" % e})); sys.exit(0)
    act = req.get('action', 0)
    try:
        n = Atspi.Action.get_n_actions(a)
        names = [Atspi.Action.get_action_name(a, i) for i in range(n)]
    except Exception: names = []
    idx = act if isinstance(act, int) else (names.index(act) if act in names else -1)
    if idx < 0 or idx >= len(names):
        print(json.dumps({"ok": False, "error": "action not available", "actions": names})); sys.exit(0)
    ok = Atspi.Action.do_action(a, idx)
    print(json.dumps({"ok": bool(ok), "node": before, "did": names[idx]}))
)PY";

bool Has(const char* bin) {
    std::string c = std::string("command -v ") + bin + " >/dev/null 2>&1";
    return std::system(c.c_str()) == 0;
}
std::string Env(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }
// argv exec with optional stdin, captured stdout, timeout.
int Run(const std::vector<std::string>& argv, const std::string& in, std::string& out, int timeout_ms = 10000) {
    int ip[2], op[2];
    if (pipe(ip) || pipe(op)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(ip[0], 0); dup2(op[1], 1);
        int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 2);
        close(ip[1]); close(op[0]);
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);
    }
    close(ip[0]); close(op[1]);
    if (!in.empty()) { auto w = write(ip[1], in.data(), in.size()); (void)w; }
    close(ip[1]);
    fcntl(op[0], F_SETFL, O_NONBLOCK);
    char buf[8192];
    const auto t0 = std::chrono::steady_clock::now();
    int status = 0;
    for (;;) {
        ssize_t n;
        while ((n = read(op[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<std::size_t>(n));
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { while ((n = read(op[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<std::size_t>(n)); break; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) {
            kill(pid, SIGKILL); waitpid(pid, &status, 0); close(op[0]); return -2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    close(op[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
std::string Trim(std::string s) { while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back(); return s; }
bool LaunchAllowed(const std::string& app, std::string& why) {
    if (Env("OPENCLAW_DESKTOP_LAUNCH") == "1") return true;
    const auto list = Env("OPENCLAW_DESKTOP_APPS");
    const auto base = fs::path(app).filename().string();
    std::stringstream ss(list); std::string x;
    while (std::getline(ss, x, ',')) if (!x.empty() && x == base) return true;
    why = "desktop.launch denied: set OPENCLAW_DESKTOP_APPS (comma list of app basenames) or OPENCLAW_DESKTOP_LAUNCH=1";
    return false;
}
J AtspiCall(const J& req) {
    const auto script = (fs::temp_directory_path() / ("oc-atspi-" + std::to_string(getpid()) + ".py")).string();
    if (!fs::exists(script)) std::ofstream(script) << kAtspiPy;
    std::string out;
    int rc = Run({"python3", script}, req.dump(), out, 15000);
    if (rc != 0) return {{"ok", false}, {"error", rc == -2 ? "atspi helper timed out" : "atspi helper failed (rc=" + std::to_string(rc) + ")"}};
    try { return J::parse(out); } catch (...) { return {{"ok", false}, {"error", "bad atspi output"}}; }
}
}  // namespace

Controller& Default() { static Controller c; return c; }

Controller::~Controller() {
    for (int p : pids_) { kill(-p, SIGTERM); }
}

bool Controller::Available() const { return !Env("DISPLAY").empty() && Env("OPENCLAW_DESKTOP_FORCE_SIM") != "1" && Has("xdotool"); }
bool Controller::A11yAvailable() const {
    static int cached = -1;
    if (cached < 0) {
        std::string out;
        cached = Run({"python3", "-c", "import gi; gi.require_version('Atspi','2.0'); from gi.repository import Atspi"}, "", out, 8000) == 0;
    }
    return cached == 1 && !Env("DISPLAY").empty();
}

J Controller::Status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {{"available", Available()}, {"a11y", A11yAvailable()}, {"display", Env("DISPLAY")}, {"launched", pids_},
            {"launchPolicy", Env("OPENCLAW_DESKTOP_LAUNCH") == "1" ? "any" : (Env("OPENCLAW_DESKTOP_APPS").empty() ? "deny" : "allowlist")}};
}

J Controller::Launch(const J& p) {
    const auto app = p.value("app", p.value("name", std::string()));
    if (app.empty()) return {{"ok", false}, {"error", "app required"}};
    std::string why;
    if (!LaunchAllowed(app, why)) return {{"ok", false}, {"error", why}};
    std::vector<std::string> argv{app};
    if (p.contains("args") && p["args"].is_array()) for (auto& a : p["args"]) argv.push_back(a.is_string() ? a.get<std::string>() : a.dump());
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);
    }
    if (pid < 0) return {{"ok", false}, {"error", "fork failed"}};
    { std::lock_guard<std::mutex> lk(mu_); pids_.push_back(pid); }
    J windows = J::array();
    const int wait_ms = p.value("wait_ms", 5000);
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(wait_ms)) {
        int st; if (waitpid(pid, &st, WNOHANG) == pid) return {{"ok", false}, {"error", "process exited"}, {"pid", pid}, {"exit", WIFEXITED(st) ? WEXITSTATUS(st) : -1}};
        std::string out;
        Run({"xdotool", "search", "--onlyvisible", "--pid", std::to_string(pid)}, "", out, 3000);
        std::stringstream ss(out); std::string id;
        while (ss >> id) windows.push_back(id);
        if (!windows.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return {{"ok", true}, {"pid", pid}, {"app", app}, {"windows", windows}, {"mode", "live"}};
}

J Controller::Windows() const {
    std::string out;
    Run({"xdotool", "search", "--onlyvisible", "--name", ""}, "", out, 5000);
    std::string active;
    Run({"xdotool", "getactivewindow"}, "", active, 3000);
    active = Trim(active);
    J arr = J::array();
    std::stringstream ss(out); std::string id;
    while (ss >> id) {
        std::string name, pid, geo;
        Run({"xdotool", "getwindowname", id}, "", name, 3000);
        Run({"xdotool", "getwindowpid", id}, "", pid, 3000);
        Run({"xdotool", "getwindowgeometry", "--shell", id}, "", geo, 3000);
        J w{{"id", id}, {"name", Trim(name)}, {"active", id == active}};
        if (!Trim(pid).empty()) w["pid"] = std::atoi(Trim(pid).c_str());
        std::stringstream gs(geo); std::string l;
        while (std::getline(gs, l)) {
            auto eq = l.find('=');
            if (eq == std::string::npos) continue;
            auto k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k == "X") w["x"] = std::atoi(v.c_str()); else if (k == "Y") w["y"] = std::atoi(v.c_str());
            else if (k == "WIDTH") w["width"] = std::atoi(v.c_str()); else if (k == "HEIGHT") w["height"] = std::atoi(v.c_str());
        }
        if (w.value("width", 0) <= 1 && w["name"] == "") continue;
        arr.push_back(w);
    }
    return {{"ok", true}, {"windows", arr}, {"active", active}};
}

J Controller::Focus(const J& p) {
    const auto id = p.value("window", std::string());
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) return {{"ok", false}, {"error", "numeric window id required"}};
    std::string out;
    int rc = Run({"xdotool", "windowactivate", "--sync", id}, "", out, 4000);
    if (rc != 0) rc = Run({"xdotool", "windowfocus", "--sync", id}, "", out, 4000);
    return {{"ok", rc == 0}, {"window", id}};
}

J Controller::Close(const J& p) {
    const auto id = p.value("window", std::string());
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) return {{"ok", false}, {"error", "numeric window id required"}};
    std::string out;
    int rc = Run({"xdotool", "windowclose", id}, "", out, 4000);
    if (rc != 0) rc = Run({"xdotool", "windowkill", id}, "", out, 4000);
    return {{"ok", rc == 0}, {"window", id}};
}

J Controller::Release(const J& p) {
    std::lock_guard<std::mutex> lk(mu_);
    J killed = J::array();
    const int only = p.value("pid", 0);
    for (auto it = pids_.begin(); it != pids_.end();) {
        if (only && *it != only) { ++it; continue; }
        kill(-*it, SIGTERM); kill(*it, SIGTERM);
        int st; bool gone = false;
        for (int i = 0; i < 30 && !gone; ++i) { gone = waitpid(*it, &st, WNOHANG) == *it; if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        if (!gone) { kill(-*it, SIGKILL); kill(*it, SIGKILL); waitpid(*it, &st, 0); }
        killed.push_back(*it);
        it = pids_.erase(it);
    }
    if (only && killed.empty()) return {{"ok", false}, {"error", "pid not launched by desktop controller"}};
    return {{"ok", true}, {"released", killed}};
}

J Controller::A11yTree(const J& p) const {
    if (!A11yAvailable()) return {{"ok", false}, {"error", "AT-SPI unavailable (need DISPLAY, python3-gi, gir1.2-atspi-2.0, a11y bus)"}};
    J req{{"op", "tree"}, {"max_nodes", std::min(p.value("max_nodes", 400), 2000)}};
    if (p.contains("app")) req["app"] = p["app"];
    return AtspiCall(req);
}

J Controller::A11yAction(const J& p) {
    if (!A11yAvailable()) return {{"ok", false}, {"error", "AT-SPI unavailable"}};
    if (!p.contains("path") || !p["path"].is_array()) return {{"ok", false}, {"error", "path (array of child indexes) required"}};
    J req{{"op", "action"}, {"path", p["path"]}};
    if (p.contains("text")) req["text"] = p["text"];
    else req["action"] = p.contains("action") ? p["action"] : J(0);
    return AtspiCall(req);
}

J Controller::Observe(const J& p) const {
    J o{{"ok", true}, {"mode", "live"}, {"windows", Windows()["windows"]}};
    if (p.value("a11y", true) && A11yAvailable()) o["a11y"] = A11yTree({{"max_nodes", p.value("max_nodes", 200)}});
    return o;
}

}  // namespace openclaw::desktop
