#include "openclaw/cli/tui_screen.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace openclaw::cli {
namespace {
using json = nlohmann::json;

std::atomic<bool> g_resized{false};
void OnWinch(int) { g_resized = true; }

struct Line { std::string who; std::string text; };

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
    std::string status_ = "connected";
    std::thread worker_;
    int spin_ = 0;
};

void Screen::LoadHistory() {
    std::string e;
    auto h = b_.rpc("chat.history", {{"sessionKey", b_.session}, {"limit", b_.history_limit}}, 15, &e);
    std::lock_guard<std::mutex> l(mu_);
    lines_.clear();
    if (h.is_null()) { lines_.push_back({"system", "history unavailable: " + e}); return; }
    for (auto& m : h.value("messages", json::array())) {
        const std::string role = m.value("role", "");
        if (role == "user" || role == "assistant") lines_.push_back({role == "user" ? "you" : "agent", TextOf(m)});
    }
    dirty_ = true;
}

void Screen::Draw() {
    std::lock_guard<std::mutex> l(mu_);
    std::string o = "\x1b[?25l\x1b[H";
    // header
    static const char* spin = "|/-\\";
    std::string head = " openclaw tui | " + b_.session + " | " + (busy_ ? std::string("thinking ") + spin[spin_ % 4] : status_) + " | " + b_.base;
    std::string hcut; { int w = 0; char32_t c; for (std::size_t i = 0; i < head.size();) { std::size_t n = NextCp(head, i, &c); if (w + CharWidth(c) > cols_) break; hcut += head.substr(i, n); w += CharWidth(c); i += n; } }
    o += "\x1b[7m" + hcut + std::string(std::max(0, cols_ - Width(hcut)), ' ') + "\x1b[0m";
    // transcript
    std::vector<std::pair<std::string, std::string>> rows;  // (style, text)
    for (auto& ln : lines_) {
        const std::string prefix = ln.who == "you" ? "you: " : ln.who == "agent" ? "agent: " : "* ";
        const std::string style = ln.who == "you" ? "\x1b[1;36m" : ln.who == "agent" ? "\x1b[0m" : "\x1b[2m";
        auto wrapped = Wrap(prefix + ln.text, cols_);
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
    // footer: separator + input
    std::string sep = scroll_ > 0 ? " -- scrolled " + std::to_string(scroll_) + " (PgDn) -- " : " Enter send | /help | PgUp/PgDn scroll | Ctrl+C abort/quit ";
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
    if (busy_) { Add("system", "still waiting for the previous reply (Ctrl+C aborts)"); return; }
    Add("you", text);
    busy_ = true;
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, text] {
        json p = b_.send_extra; p["sessionKey"] = b_.session; p["message"] = text;
        std::string e;
        auto r = b_.rpc("chat.send", p, b_.timeout_s, &e);
        if (r.is_null()) Add("system", "error: " + e);
        else Add("agent", r.value("text", r.dump()));
        busy_ = false; dirty_ = true;
    });
}

bool Screen::Command(const std::string& line) {
    auto rpc_note = [&](const std::string& m, const json& p, const std::string& okmsg) {
        std::string e; auto r = b_.rpc(m, p, 10, &e); Add("system", r.is_null() ? m + " failed: " + e : okmsg); return r;
    };
    if (line == "/exit" || line == "/quit") return false;
    if (line == "/help") { Add("system", "/session <key>  /sessions  /history  /new  /abort  /status  /clear  /exit"); return true; }
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
    if (!b_.initial_message.empty()) StartSend(b_.initial_message);
    int rc = 0;
    std::string esc;
    try {
        while (true) {
            if (g_resized.exchange(false)) { Size(); Out("\x1b[2J"); dirty_ = true; }
            if (busy_) { ++spin_; dirty_ = true; }
            if (dirty_) Draw();
            pollfd pf{STDIN_FILENO, POLLIN, 0};
            if (poll(&pf, 1, 120) <= 0) continue;
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
    if (worker_.joinable()) worker_.join();
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
