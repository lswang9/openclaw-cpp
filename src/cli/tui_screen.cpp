#include "openclaw/cli/tui_screen.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <curl/curl.h>

#include <atomic>
#include <deque>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace openclaw::cli {
namespace {
using json = nlohmann::json;

std::atomic<bool> g_resized{false};
void OnWinch(int) { g_resized = true; }

struct Line { std::string who; std::string text; bool done = true; };  // who: you|agent|system|tool|thinking

// Split assistant text into (thinking, answer): <think>…</think> blocks (an unterminated block is still thinking).
std::pair<std::string, std::string> SplitThinking(const std::string& raw) {
    std::string think, answer;
    std::size_t i = 0;
    while (i < raw.size()) {
        auto o = raw.find("<think>", i);
        if (o == std::string::npos) { answer += raw.substr(i); break; }
        answer += raw.substr(i, o - i);
        auto c = raw.find("</think>", o + 7);
        think += raw.substr(o + 7, c == std::string::npos ? std::string::npos : c - o - 7);
        if (c == std::string::npos) break;
        i = c + 8;
    }
    auto trim = [](std::string s) { auto a = s.find_first_not_of(" \n\t"); if (a == std::string::npos) return std::string(); return s.substr(a, s.find_last_not_of(" \n\t") - a + 1); };
    return {trim(think), trim(answer)};
}
std::string OneLine(std::string s, std::size_t max) {
    for (auto& ch : s) if (ch == '\n' || ch == '\r') ch = ' ';
    if (s.size() > max) { s.resize(max); while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back(); s += "…"; }
    return s;
}
std::size_t CurlSink(char* p, std::size_t s, std::size_t n, void* u) { static_cast<std::string*>(u)->append(p, s * n); return s * n; }

// Display width of a UTF-8 string (wide CJK/emoji count 2), and splitting by width.
int CharWidth(char32_t c) {
    if (c == 0) return 0;
    if (c < 0x1100) return 1;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) ||
        (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}
std::size_t NextCp(const std::string& s, std::size_t i, char32_t* out) {
    unsigned char c = s[i];
    std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) n = 1;
    char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    *out = cp;
    return n;
}
int Width(const std::string& s) { int w = 0; char32_t c; for (std::size_t i = 0; i < s.size();) { i += NextCp(s, i, &c); w += CharWidth(c); } return w; }

// Word-wrap one paragraph to `cols` display columns.
std::vector<std::string> Wrap(const std::string& text, int cols) {
    std::vector<std::string> out;
    if (cols < 4) cols = 4;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string cur; int w = 0; std::size_t last_space = std::string::npos; int w_at_space = 0;
        char32_t c;
        for (std::size_t i = 0; i < para.size();) {
            std::size_t n = NextCp(para, i, &c);
            int cw = c == '\t' ? 1 : CharWidth(c);
            if (w + cw > cols) {
                if (last_space != std::string::npos && last_space > 0) {
                    out.push_back(cur.substr(0, last_space));
                    cur = cur.substr(last_space + 1); w -= w_at_space + 1;
                } else { out.push_back(cur); cur.clear(); w = 0; }
                last_space = std::string::npos;
            }
            if (c == ' ') { last_space = cur.size(); w_at_space = w; }
            cur += c == '\t' ? std::string(" ") : para.substr(i, n); w += cw; i += n;
        }
        out.push_back(cur);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

std::string TextOf(const json& m) {
    std::string text;
    if (m.contains("content") && m["content"].is_string()) text = m["content"];
    else if (m.contains("content") && m["content"].is_array()) { for (auto& p : m["content"]) if (p.is_object() && p.value("type", "") == "text") text += p.value("text", ""); }
    else text = m.value("text", "");
    return text;
}

struct Picker { std::string kind, title; std::vector<std::pair<std::string, std::string>> items; int sel = 0; };

class Screen {
public:
    explicit Screen(TuiBackend& b) : b_(b) {}
    int Run();

private:
    void Size() { winsize ws{}; if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { cols_ = ws.ws_col; rows_ = ws.ws_row; } }
    void Out(const std::string& s) { auto w = ::write(STDOUT_FILENO, s.data(), s.size()); (void)w; }
    void Add(const std::string& who, const std::string& text) { std::lock_guard<std::mutex> l(mu_); lines_.push_back({who, text}); scroll_ = 0; dirty_ = true; }
    void LoadHistory();
    void Draw();
    void Submit(const std::string& text);
    bool Command(const std::string& text);  // false → exit
    void StartSend(const std::string& text);
    void EventLoop();
    void OnEvent(const json& ev);
    void UpdateStreamLines();  // mu_ held
    void OpenModelPicker();
    void OpenAgentPicker();
    void PickerChoose();
    std::string AgentOfSession() const { auto s = b_.session; if (s.rfind("agent:", 0) == 0) { auto e = s.find(':', 6); return s.substr(6, e == std::string::npos ? std::string::npos : e - 6); } return "main"; }

    TuiBackend& b_;
    int cols_ = 80, rows_ = 24;
    std::vector<Line> lines_;
    std::mutex mu_;
    std::string input_;
    std::size_t cursor_ = 0;  // byte offset in input_
    std::vector<std::string> hist_;
    std::size_t hist_pos_ = 0;
    int scroll_ = 0;  // rows scrolled up from bottom
    std::atomic<bool> busy_{false};
    std::atomic<bool> dirty_{true};
    std::atomic<bool> stop_{false};
    std::atomic<bool> live_{false};  // event stream connected
    std::string status_ = "connected";
    std::string model_;  // session model override chosen in the picker ("" = default)
    std::thread worker_, events_;
    int spin_ = 0;
    // streaming state (mu_)
    std::string stream_raw_;
    int stream_agent_ = -1, stream_think_ = -1;
    std::vector<int> open_tools_;
    bool show_thinking_ = false, expand_tools_ = false;
    std::deque<std::string> queue_;
    std::optional<Picker> picker_;
};

void Screen::LoadHistory() {
    std::string e;
    auto h = b_.rpc("chat.history", {{"sessionKey", b_.session}, {"limit", b_.history_limit}}, 15, &e);
    std::lock_guard<std::mutex> l(mu_);
    lines_.clear();
    if (h.is_null()) { lines_.push_back({"system", "history unavailable: " + e}); return; }
    for (auto& m : h.value("messages", json::array())) {
        const std::string role = m.value("role", "");
        if (role == "user") lines_.push_back({"you", TextOf(m)});
        else if (role == "assistant") {
            auto [think, answer] = SplitThinking(TextOf(m));
            if (!think.empty()) lines_.push_back({"thinking", think});
            if (m.contains("tool_calls") && m["tool_calls"].is_array())
                for (auto& tc : m["tool_calls"]) if (tc.is_object()) lines_.push_back({"tool", "✓ " + tc.value("function", json::object()).value("name", tc.value("name", std::string("tool")))});
            if (!answer.empty()) lines_.push_back({"agent", answer});
        }
    }
    dirty_ = true;
}

void Screen::Draw() {
    std::lock_guard<std::mutex> l(mu_);
    std::string o = "\x1b[?25l\x1b[H";
    // header
    static const char* spin = "|/-\\";
    std::string head = " openclaw tui | " + b_.session + " | model " + (model_.empty() ? std::string("default") : model_) + " | " +
                       (busy_ ? std::string("thinking ") + spin[spin_ % 4] : status_) + (live_ ? " | live" : "") +
                       (queue_.empty() ? std::string() : " | queued " + std::to_string(queue_.size())) + " | " + b_.base;
    std::string hcut; { int w = 0; char32_t c; for (std::size_t i = 0; i < head.size();) { std::size_t n = NextCp(head, i, &c); if (w + CharWidth(c) > cols_) break; hcut += head.substr(i, n); w += CharWidth(c); i += n; } }
    o += "\x1b[7m" + hcut + std::string(std::max(0, cols_ - Width(hcut)), ' ') + "\x1b[0m";
    // transcript
    std::vector<std::pair<std::string, std::string>> rows;  // (style, text)
    for (auto& ln : lines_) {
        std::string body = ln.text, prefix, style;
        if (ln.who == "you") { prefix = "you: "; style = "\x1b[1;36m"; }
        else if (ln.who == "agent") { prefix = "agent: "; style = "\x1b[0m"; if (!ln.done) body += "▌"; }
        else if (ln.who == "thinking") {
            style = "\x1b[2;3m";
            if (show_thinking_) prefix = "thinking: ";
            else { prefix = ""; body = "▸ thinking (" + std::to_string(Width(ln.text)) + " chars, Ctrl+T to show)" + (ln.done ? "" : " …"); }
        } else if (ln.who == "tool") {
            style = "\x1b[33m";
            if (!expand_tools_) { auto nl = body.find('\n'); if (nl != std::string::npos) body = body.substr(0, nl) + "  (Ctrl+O to expand)"; }
        } else { prefix = "* "; style = "\x1b[2m"; }
        auto wrapped = Wrap(prefix + body, cols_);
        for (auto& w : wrapped) rows.push_back({style, w});
        rows.push_back({"", ""});
    }
    const int pane = std::max(1, rows_ - 3);
    const int total = static_cast<int>(rows.size());
    scroll_ = std::max(0, std::min(scroll_, std::max(0, total - pane)));
    const int first = std::max(0, total - pane - scroll_);
    for (int r = 0; r < pane; ++r) {
        o += "\x1b[" + std::to_string(r + 2) + ";1H\x1b[2K";
        const int idx = first + r;
        if (idx < total) o += rows[idx].first + rows[idx].second + "\x1b[0m";
    }
    if (picker_) {  // centered overlay
        const int w = std::min(cols_ - 4, 60), h = std::min(pane, static_cast<int>(picker_->items.size()) + 2);
        const int top = 2 + std::max(0, (pane - h) / 2), left = std::max(1, (cols_ - w) / 2);
        const int visible = h - 2;
        const int off = std::max(0, std::min(picker_->sel - visible + 1, static_cast<int>(picker_->items.size()) - visible));
        auto pad = [&](std::string s) { std::string cut; int cw = 0; char32_t c; for (std::size_t i = 0; i < s.size();) { std::size_t n = NextCp(s, i, &c); if (cw + CharWidth(c) > w - 2) break; cut += s.substr(i, n); cw += CharWidth(c); i += n; } return cut + std::string(std::max(0, w - 2 - cw), ' '); };
        o += "\x1b[" + std::to_string(top) + ";" + std::to_string(left) + "H\x1b[7m " + pad(picker_->title + " (↑/↓ Enter, Esc)") + " \x1b[0m";
        for (int r = 0; r < visible; ++r) {
            const int idx = off + r;
            const bool sel = idx == picker_->sel;
            o += "\x1b[" + std::to_string(top + 1 + r) + ";" + std::to_string(left) + "H" + (sel ? "\x1b[1;7m>" : "\x1b[0m ") +
                 pad(idx < static_cast<int>(picker_->items.size()) ? picker_->items[idx].first : "") + " \x1b[0m";
        }
        o += "\x1b[" + std::to_string(top + h - 1) + ";" + std::to_string(left) + "H\x1b[7m" + std::string(w, ' ') + "\x1b[0m";
    }
    // footer: separator + input
    std::string sep = scroll_ > 0 ? " -- scrolled " + std::to_string(scroll_) + " (PgDn) -- " : " Enter send | /help | Ctrl+P model | Ctrl+G agent | Ctrl+T thinking | Ctrl+O tools | Ctrl+C abort/quit ";
    o += "\x1b[" + std::to_string(rows_ - 1) + ";1H\x1b[2K\x1b[2m" + sep.substr(0, cols_) + "\x1b[0m";
    // input with horizontal scrolling around the cursor
    const std::string before = input_.substr(0, cursor_);
    const int avail = std::max(4, cols_ - 3);
    std::string shown = input_; int cur_w = Width(before);
    if (Width(input_) > avail) {
        // drop leading code points until the cursor fits
        std::size_t start = 0; char32_t c;
        while (cur_w > avail - 1 && start < cursor_) { std::size_t n = NextCp(input_, start, &c); cur_w -= CharWidth(c); start += n; }
        shown = input_.substr(start);
        std::string cut; int w = 0;
        for (std::size_t i = 0; i < shown.size();) { std::size_t n = NextCp(shown, i, &c); if (w + CharWidth(c) > avail) break; cut += shown.substr(i, n); w += CharWidth(c); i += n; }
        shown = cut;
    }
    o += "\x1b[" + std::to_string(rows_) + ";1H\x1b[2K\x1b[1m> \x1b[0m" + shown;
    o += "\x1b[" + std::to_string(rows_) + ";" + std::to_string(3 + cur_w) + "H\x1b[?25h";
    Out(o);
    dirty_ = false;
}

void Screen::StartSend(const std::string& text) {
    if (busy_) {
        std::lock_guard<std::mutex> l(mu_);
        queue_.push_back(text);
        lines_.push_back({"system", "queued (" + std::to_string(queue_.size()) + "): " + OneLine(text, 60)});
        dirty_ = true;
        return;
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        lines_.push_back({"you", text});
        stream_raw_.clear(); stream_agent_ = stream_think_ = -1; open_tools_.clear();
        scroll_ = 0; dirty_ = true;
    }
    busy_ = true;
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, text] {
        json p = b_.send_extra; p["sessionKey"] = b_.session; p["message"] = text;
        std::string e;
        auto r = b_.rpc("chat.send", p, b_.timeout_s, &e);
        std::string next;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (r.is_null()) {
                if (stream_agent_ >= 0) lines_[stream_agent_].done = true;
                lines_.push_back({"system", "error: " + e});
            } else {
                stream_raw_ = r.value("text", r.dump());  // the final reply is authoritative
                UpdateStreamLines();
                if (stream_agent_ >= 0) lines_[stream_agent_].done = true;
                if (stream_think_ >= 0) lines_[stream_think_].done = true;
            }
            for (int i : open_tools_) if (i < static_cast<int>(lines_.size())) lines_[i].done = true;
            stream_agent_ = stream_think_ = -1; open_tools_.clear();
            if (!queue_.empty()) { next = queue_.front(); queue_.pop_front(); }
            scroll_ = 0;
        }
        busy_ = false; dirty_ = true;
        if (!next.empty()) {
            // hand the queued message to a fresh worker from this thread's exit path
            std::thread([this, next] { while (busy_ && !stop_) std::this_thread::sleep_for(std::chrono::milliseconds(5)); if (!stop_) StartSend(next); }).detach();
        }
    });
}

void Screen::UpdateStreamLines() {
    auto [think, answer] = SplitThinking(stream_raw_);
    if (!think.empty()) {
        if (stream_think_ < 0) { lines_.push_back({"thinking", "", false}); stream_think_ = static_cast<int>(lines_.size()) - 1; }
        lines_[stream_think_].text = think;
        lines_[stream_think_].done = stream_raw_.find("</think>") != std::string::npos;
    }
    if (!answer.empty()) {
        if (stream_agent_ < 0) { lines_.push_back({"agent", "", false}); stream_agent_ = static_cast<int>(lines_.size()) - 1; }
        lines_[stream_agent_].text = answer;
    }
    dirty_ = true;
}

void Screen::OnEvent(const json& ev) {
    const auto name = ev.value("event", std::string());
    const auto& pl = ev.contains("payload") && ev["payload"].is_object() ? ev["payload"] : json::object();
    if (pl.value("sessionKey", std::string()) != b_.session) return;
    std::lock_guard<std::mutex> l(mu_);
    if (!busy_) return;
    if (name == "agent.delta") {
        // a new assistant turn after a tool call starts a fresh answer line
        if (stream_agent_ >= 0 && !open_tools_.empty() && stream_agent_ < open_tools_.back()) { lines_[stream_agent_].done = true; stream_agent_ = -1; stream_raw_.clear(); stream_think_ = -1; }
        stream_raw_ += pl.value("delta", std::string());
        UpdateStreamLines();
    } else if (name == "agent.tool") {
        const auto tool = pl.value("tool", std::string("tool"));
        if (pl.value("phase", "") == "start") {
            if (stream_agent_ >= 0) { lines_[stream_agent_].done = true; }
            lines_.push_back({"tool", "⚙ " + tool + " " + OneLine(pl.value("args", json::object()).dump(), 120), false});
            open_tools_.push_back(static_cast<int>(lines_.size()) - 1);
        } else {
            for (auto it = open_tools_.rbegin(); it != open_tools_.rend(); ++it) {
                auto& ln = lines_[*it];
                if (ln.done || ln.text.find("⚙ " + tool) != 0) continue;
                const auto err = pl.value("error", std::string());
                ln.text = (err.empty() ? "✓ " : "✗ ") + tool + " (" + std::to_string(pl.value("durationMs", 0LL)) + " ms)" + ln.text.substr(std::string("⚙ ").size() + tool.size()) +
                          "\n" + (err.empty() ? pl.value("result", std::string()) : "error: " + err);
                ln.done = true;
                break;
            }
        }
        dirty_ = true;
    }
}

void Screen::EventLoop() {
    std::string url = b_.base;
    if (url.rfind("http://", 0) == 0) url = "ws://" + url.substr(7); else if (url.rfind("https://", 0) == 0) url = "wss://" + url.substr(8);
    if (url.size() > 7 && url.substr(url.size() - 7) == "/v1/rpc") url.resize(url.size() - 7);
    if (url.empty() || url.back() != '/') url += "/";
    while (!stop_) {
        CURL* c = curl_easy_init();
        if (!c) return;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
        curl_slist* h = nullptr;
        if (!b_.token.empty()) { h = curl_slist_append(h, ("Authorization: Bearer " + b_.token).c_str()); curl_easy_setopt(c, CURLOPT_HTTPHEADER, h); }
        if (curl_easy_perform(c) == CURLE_OK) {
            live_ = true; dirty_ = true;
            std::string acc; char buf[65536];
            while (!stop_) {
                size_t n = 0; const curl_ws_frame* meta = nullptr;
                auto rc = curl_ws_recv(c, buf, sizeof buf, &n, &meta);
                if (rc == CURLE_AGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
                if (rc != CURLE_OK || (meta && (meta->flags & CURLWS_CLOSE))) break;
                if (meta && (meta->flags & CURLWS_PING)) { size_t s = 0; (void)curl_ws_send(c, buf, n, &s, 0, CURLWS_PONG); continue; }
                acc.append(buf, n);
                if (meta && meta->bytesleft > 0) continue;
                try { auto ev = json::parse(acc); if (ev.value("type", "") == "event") OnEvent(ev); } catch (...) {}
                acc.clear();
            }
            live_ = false; dirty_ = true;
        }
        curl_easy_cleanup(c);
        if (h) curl_slist_free_all(h);
        for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Screen::OpenModelPicker() {
    std::string e;
    auto r = b_.rpc("models.list", json::object(), 10, &e);
    Picker pk{"model", "Select model for " + b_.session, {{"default (agent setting)", ""}}, 0};
    for (auto& m : r.is_object() ? r.value("data", r.value("models", json::array())) : json::array()) {
        const auto id = m.is_string() ? m.get<std::string>() : m.value("id", std::string());
        if (!id.empty()) pk.items.push_back({id + (id == model_ ? "  (current)" : ""), id});
    }
    for (std::size_t i = 0; i < pk.items.size(); ++i) if (pk.items[i].second == model_) pk.sel = static_cast<int>(i);
    std::lock_guard<std::mutex> l(mu_);
    if (r.is_null()) { lines_.push_back({"system", "models.list failed: " + e}); dirty_ = true; return; }
    picker_ = pk; dirty_ = true;
}

void Screen::OpenAgentPicker() {
    std::string e;
    auto r = b_.rpc("agents.list", json::object(), 10, &e);
    Picker pk{"agent", "Switch agent", {}, 0};
    const auto cur = AgentOfSession();
    for (auto& a : r.is_object() ? r.value("agents", json::array()) : json::array()) {
        const auto id = a.value("id", std::string());
        if (id.empty()) continue;
        std::string label = id;
        if (auto n = a.value("name", std::string()); !n.empty() && n != id) label += " — " + n;
        if (id == cur) { label += "  (current)"; pk.sel = static_cast<int>(pk.items.size()); }
        pk.items.push_back({label, id});
    }
    std::lock_guard<std::mutex> l(mu_);
    if (r.is_null() || pk.items.empty()) { lines_.push_back({"system", "agents.list failed: " + (e.empty() ? std::string("no agents") : e)}); dirty_ = true; return; }
    picker_ = pk; dirty_ = true;
}

void Screen::PickerChoose() {
    Picker pk;
    { std::lock_guard<std::mutex> l(mu_); if (!picker_) return; pk = *picker_; picker_.reset(); dirty_ = true; }
    if (pk.items.empty()) return;
    const auto value = pk.items[pk.sel].second;
    if (pk.kind == "model") {
        std::string e;
        auto r = b_.rpc("sessions.patch", {{"key", b_.session}, {"model", value.empty() ? json(nullptr) : json(value)}}, 10, &e);
        if (r.is_null()) { Add("system", "sessions.patch failed: " + e); return; }
        model_ = value;
        Add("system", "model for this session: " + (value.empty() ? std::string("default") : value));
    } else {
        b_.session = "agent:" + value + ":main";
        model_.clear();
        LoadHistory();
        Add("system", "agent " + value + " (session " + b_.session + ")");
    }
}

bool Screen::Command(const std::string& line) {
    auto rpc_note = [&](const std::string& m, const json& p, const std::string& okmsg) {
        std::string e; auto r = b_.rpc(m, p, 10, &e); Add("system", r.is_null() ? m + " failed: " + e : okmsg); return r;
    };
    if (line == "/exit" || line == "/quit") return false;
    if (line == "/help") { Add("system", "/session <key>  /sessions  /model [name]  /agent [id]  /queue [clear]  /thinking on|off  /tools expand|collapse  /history  /new  /abort  /status  /clear  /exit"); return true; }
    if (line == "/model") { OpenModelPicker(); return true; }
    if (line.rfind("/model ", 0) == 0) {
        const auto m = line.substr(7);
        std::string e; auto r = b_.rpc("sessions.patch", {{"key", b_.session}, {"model", m}}, 10, &e);
        if (r.is_null()) Add("system", "sessions.patch failed: " + e); else { model_ = m; Add("system", "model for this session: " + m); }
        return true;
    }
    if (line == "/agent" || line == "/agents") { OpenAgentPicker(); return true; }
    if (line.rfind("/agent ", 0) == 0) { b_.session = "agent:" + line.substr(7) + ":main"; model_.clear(); LoadHistory(); Add("system", "session " + b_.session); return true; }
    if (line == "/queue") {
        std::lock_guard<std::mutex> l(mu_);
        std::string txt = queue_.empty() ? "queue empty" : "queued:";
        for (auto& q : queue_) txt += "\n  " + OneLine(q, 100);
        lines_.push_back({"system", txt}); dirty_ = true; return true;
    }
    if (line == "/queue clear") { std::lock_guard<std::mutex> l(mu_); queue_.clear(); lines_.push_back({"system", "queue cleared"}); dirty_ = true; return true; }
    if (line == "/thinking on" || line == "/thinking off") { show_thinking_ = line == "/thinking on"; dirty_ = true; return true; }
    if (line == "/tools expand" || line == "/tools collapse") { expand_tools_ = line == "/tools expand"; dirty_ = true; return true; }
    if (line == "/history") { LoadHistory(); return true; }
    if (line == "/clear") { std::lock_guard<std::mutex> l(mu_); lines_.clear(); dirty_ = true; return true; }
    if (line == "/abort") { rpc_note("chat.abort", {{"sessionKey", b_.session}}, "abort requested"); return true; }
    if (line == "/new" || line == "/reset") { rpc_note("sessions.reset", {{"key", b_.session}}, "session reset"); LoadHistory(); return true; }
    if (line == "/status") { std::string e; auto s = b_.rpc("status", json::object(), 10, &e); Add("system", s.is_null() ? e : s.dump(2)); return true; }
    if (line == "/sessions") {
        std::string e; auto s = b_.rpc("sessions.list", json::object(), 10, &e);
        std::string txt = "sessions:";
        auto arr = s.is_object() ? s.value("sessions", json::array()) : s;
        if (arr.is_array()) for (auto& x : arr) txt += "\n  " + (x.is_string() ? x.get<std::string>() : x.value("key", x.dump()));
        Add("system", s.is_null() ? "sessions.list failed: " + e : txt);
        return true;
    }
    if (line.rfind("/session ", 0) == 0) {
        b_.session = line.substr(9);
        if (b_.session.find(':') == std::string::npos) b_.session = "agent:main:" + b_.session;
        LoadHistory(); Add("system", "session " + b_.session);
        return true;
    }
    Add("system", "unknown command " + line + " (/help)");
    return true;
}

void Screen::Submit(const std::string& text) {
    if (text.empty()) return;
    if (hist_.empty() || hist_.back() != text) hist_.push_back(text);
    hist_pos_ = hist_.size();
    if (text[0] == '/') { if (!Command(text)) throw 0; return; }
    StartSend(text);
}

int Screen::Run() {
    termios orig{};
    if (tcgetattr(STDIN_FILENO, &orig) != 0) return -1;
    termios raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP | BRKINT);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    struct sigaction sa{}; sa.sa_handler = OnWinch; sigaction(SIGWINCH, &sa, nullptr);
    Out("\x1b[?1049h\x1b[H\x1b[2J");
    Size();
    LoadHistory();
    events_ = std::thread([this] { EventLoop(); });
    for (int i = 0; i < 20 && !live_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (!b_.initial_message.empty()) StartSend(b_.initial_message);
    int rc = 0;
    std::string esc;
    try {
        while (true) {
            if (g_resized.exchange(false)) { Size(); Out("\x1b[2J"); dirty_ = true; }
            if (busy_) { ++spin_; dirty_ = true; }
            if (dirty_) Draw();
            pollfd pf{STDIN_FILENO, POLLIN, 0};
            if (poll(&pf, 1, 120) <= 0) {
                if (esc == "\x1b") { esc.clear(); std::lock_guard<std::mutex> l(mu_); if (picker_) { picker_.reset(); dirty_ = true; } }  // lone Esc
                continue;
            }
            char buf[256];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) { if (n == 0) break; continue; }
            for (ssize_t i = 0; i < n; ++i) {
                unsigned char ch = buf[i];
                if (!esc.empty()) {
                    esc += static_cast<char>(ch);
                    if (esc.size() == 2 && ch != '[' && ch != 'O') { esc.clear(); continue; }
                    if (esc.size() >= 3 && ((ch >= 'A' && ch <= 'Z') || ch == '~' || (ch >= 'a' && ch <= 'z'))) {
                        const auto seq = esc.substr(1); esc.clear();
                        const int page = std::max(1, rows_ - 4);
                        {
                            std::lock_guard<std::mutex> l(mu_);
                            if (picker_) {
                                const int n = static_cast<int>(picker_->items.size());
                                if (seq == "[A" && n) picker_->sel = (picker_->sel + n - 1) % n;
                                else if (seq == "[B" && n) picker_->sel = (picker_->sel + 1) % n;
                                dirty_ = true;
                                continue;
                            }
                        }
                        if (seq == "[D" && cursor_ > 0) { do --cursor_; while (cursor_ > 0 && (static_cast<unsigned char>(input_[cursor_]) & 0xC0) == 0x80); }
                        else if (seq == "[C" && cursor_ < input_.size()) { char32_t c; cursor_ += NextCp(input_, cursor_, &c); }
                        else if (seq == "[H" || seq == "OH" || seq == "[1~") cursor_ = 0;
                        else if (seq == "[F" || seq == "OF" || seq == "[4~") cursor_ = input_.size();
                        else if (seq == "[A" && hist_pos_ > 0) { input_ = hist_[--hist_pos_]; cursor_ = input_.size(); }
                        else if (seq == "[B") { if (hist_pos_ + 1 < hist_.size()) input_ = hist_[++hist_pos_]; else { hist_pos_ = hist_.size(); input_.clear(); } cursor_ = input_.size(); }
                        else if (seq == "[5~") scroll_ += page;
                        else if (seq == "[6~") scroll_ = std::max(0, scroll_ - page);
                        else if (seq == "[3~" && cursor_ < input_.size()) { char32_t c; input_.erase(cursor_, NextCp(input_, cursor_, &c)); }
                        dirty_ = true;
                    }
                    continue;
                }
                if (ch == 0x1b) { esc = "\x1b"; continue; }
                bool picking; { std::lock_guard<std::mutex> l(mu_); picking = picker_.has_value(); }
                if (picking) {
                    if (ch == '\r' || ch == '\n') PickerChoose();
                    else if (ch == 3 || ch == 'q') { std::lock_guard<std::mutex> l(mu_); picker_.reset(); dirty_ = true; }
                    continue;
                }
                if (ch == 16) { OpenModelPicker(); continue; }   // Ctrl+P
                if (ch == 7) { OpenAgentPicker(); continue; }    // Ctrl+G
                if (ch == 20) { show_thinking_ = !show_thinking_; dirty_ = true; continue; }  // Ctrl+T
                if (ch == 15) { expand_tools_ = !expand_tools_; dirty_ = true; continue; }    // Ctrl+O
                if (ch == 3) {  // Ctrl+C: abort a running turn, else quit
                    if (busy_) { std::string e; b_.rpc("chat.abort", {{"sessionKey", b_.session}}, 10, &e); Add("system", "abort requested"); }
                    else throw 0;
                    continue;
                }
                if (ch == 4 && input_.empty()) throw 0;  // Ctrl+D on empty line
                if (ch == 12) { Out("\x1b[2J"); dirty_ = true; continue; }  // Ctrl+L
                if (ch == 21) { input_.erase(0, cursor_); cursor_ = 0; dirty_ = true; continue; }  // Ctrl+U
                if (ch == 1) { cursor_ = 0; dirty_ = true; continue; }
                if (ch == 5) { cursor_ = input_.size(); dirty_ = true; continue; }
                if (ch == '\r' || ch == '\n') { auto t = input_; input_.clear(); cursor_ = 0; dirty_ = true; Submit(t); continue; }
                if (ch == 127 || ch == 8) {
                    if (cursor_ > 0) { std::size_t s = cursor_; do --s; while (s > 0 && (static_cast<unsigned char>(input_[s]) & 0xC0) == 0x80); input_.erase(s, cursor_ - s); cursor_ = s; }
                    dirty_ = true; continue;
                }
                if (ch >= 0x20 || ch == '\t') { input_.insert(cursor_, 1, static_cast<char>(ch)); ++cursor_; dirty_ = true; }
            }
        }
    } catch (int) { rc = 0; }
    stop_ = true;
    { std::lock_guard<std::mutex> l(mu_); queue_.clear(); }
    if (worker_.joinable()) worker_.join();
    while (busy_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (worker_.joinable()) worker_.join();
    if (events_.joinable()) events_.join();
    Out("\x1b[?25h\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    return rc;
}

}  // namespace

int RunFullscreenTui(TuiBackend& b) {
    Screen s(b);
    return s.Run();
}

}  // namespace openclaw::cli
