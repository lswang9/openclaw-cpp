#include "openclaw/tokenizer/bpe.hpp"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>

namespace openclaw::tokenizer {
namespace fs = std::filesystem;

namespace {

struct Cp { std::uint32_t cp; std::size_t len; };

Cp Decode(const std::string& s, std::size_t i) {
    unsigned char c = s[i];
    if (c < 0x80) return {c, 1};
    std::size_t n = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) return {c, 1};
    std::uint32_t cp = n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : n == 4 ? (c & 0x07) : c;
    for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    return {cp, n};
}

bool IsSpace(std::uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
bool IsNum(std::uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 0x660 && c <= 0x669) || (c >= 0xFF10 && c <= 0xFF19) || c == 0xB2 ||
           c == 0xB3 || c == 0xB9 || (c >= 0xBC && c <= 0xBE) || (c >= 0x2070 && c <= 0x2079) || (c >= 0x2080 && c <= 0x2089) ||
           (c >= 0x2150 && c <= 0x218B) || (c >= 0x2460 && c <= 0x249B) || (c >= 0x3007 && c == 0x3007);
}
bool IsLetter(std::uint32_t c) {
    if (c < 0x80) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    if (IsSpace(c) || IsNum(c)) return false;
    if (c < 0xC0) return c == 0xAA || c == 0xB5 || c == 0xBA;
    if (c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return (c >= 0x2090 && c <= 0x209C) || c == 0x2102 || c == 0x2107 ||
                                             (c >= 0x210A && c <= 0x2113) || c == 0x2115 || (c >= 0x2119 && c <= 0x211D) ||
                                             (c >= 0x2C00);  // letterlike subset; arrows/math/box/dingbats = symbols
    if (c >= 0x3000 && c <= 0x303F) return c == 0x3005 || c == 0x3006 || (c >= 0x3031 && c <= 0x3035) || c == 0x303B || c == 0x303C;
    if (c >= 0xFE10 && c <= 0xFE6F) return false;
    if (c >= 0xFF00 && c <= 0xFF20) return false;
    if (c >= 0xFF3B && c <= 0xFF40) return false;
    if (c >= 0xFF5B && c <= 0xFF65) return false;
    if (c >= 0xFFE0 && c <= 0xFFFF) return false;
    if (c >= 0x1F000 && c <= 0x1FAFF) return false;  // emoji & pictographs
    if (c >= 0xE000 && c <= 0xF8FF) return false;    // private use
    if (c >= 0x300 && c <= 0x36F) return false;      // combining marks (\p{M}, not \p{L})
    if (c == 0x30FB || c == 0x2E80) return false;
    return true;
}

std::string B64Decode(const std::string& in) {
    static int T[256];
    static bool init = false;
    if (!init) {
        for (int& x : T) x = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[static_cast<unsigned char>(a[i])] = i;
        init = true;
    }
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (T[c] < 0) continue;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) { out.push_back(static_cast<char>((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

}  // namespace

std::vector<std::string> PreTokenize(const std::string& s) {
    std::vector<std::string> out;
    const std::size_t n = s.size();
    std::size_t i = 0;
    auto at = [&](std::size_t p) { return p < n ? Decode(s, p) : Cp{0, 0}; };
    while (i < n) {
        Cp c = at(i);
        // 1. contractions
        if (c.cp == '\'' && i + 1 < n) {
            static const char* suf[] = {"re", "ve", "ll", "s", "t", "m", "d"};
            bool hit = false;
            for (auto sf : suf) {
                std::size_t L = std::char_traits<char>::length(sf);
                if (i + 1 + L > n) continue;
                bool m = true;
                for (std::size_t k = 0; k < L; ++k) if ((s[i + 1 + k] | 0x20) != sf[k]) m = false;
                if (m) { out.push_back(s.substr(i, 1 + L)); i += 1 + L; hit = true; break; }
            }
            if (hit) continue;
        }
        // 2. [^\r\n\p{L}\p{N}]?\p{L}+
        {
            std::size_t j = i;
            if (!IsLetter(c.cp) && c.cp != '\r' && c.cp != '\n' && !IsNum(c.cp)) j += c.len;
            std::size_t k = j;
            while (k < n) { Cp d = at(k); if (!IsLetter(d.cp)) break; k += d.len; }
            if (k > j) { out.push_back(s.substr(i, k - i)); i = k; continue; }
        }
        // 3. \p{N}{1,3}
        if (IsNum(c.cp)) {
            std::size_t k = i; int cnt = 0;
            while (k < n && cnt < 3) { Cp d = at(k); if (!IsNum(d.cp)) break; k += d.len; ++cnt; }
            out.push_back(s.substr(i, k - i)); i = k; continue;
        }
        // 4. ' ?[^\s\p{L}\p{N}]+[\r\n]*'
        {
            std::size_t j = i;
            if (c.cp == ' ') j += 1;
            std::size_t k = j;
            while (k < n) { Cp d = at(k); if (IsSpace(d.cp) || IsLetter(d.cp) || IsNum(d.cp)) break; k += d.len; }
            if (k > j) {
                while (k < n && (s[k] == '\r' || s[k] == '\n')) ++k;
                out.push_back(s.substr(i, k - i)); i = k; continue;
            }
        }
        // whitespace alternatives
        if (IsSpace(c.cp)) {
            std::size_t k = i, last_nl = std::string::npos;
            std::vector<std::size_t> starts;
            while (k < n) { Cp d = at(k); if (!IsSpace(d.cp)) break; starts.push_back(k); if (d.cp == '\r' || d.cp == '\n') last_nl = k; k += d.len; }
            if (last_nl != std::string::npos) { out.push_back(s.substr(i, last_nl + 1 - i)); i = last_nl + 1; continue; }  // 5
            if (k >= n) { out.push_back(s.substr(i, k - i)); i = k; continue; }  // 6 at end
            if (starts.size() >= 2) { std::size_t e = starts.back(); out.push_back(s.substr(i, e - i)); i = e; continue; }  // 6 backtrack
            out.push_back(s.substr(i, k - i)); i = k; continue;  // 7
        }
        out.push_back(s.substr(i, c.len)); i += c.len;  // safety
    }
    return out;
}

std::unique_ptr<Bpe> Bpe::FromRanks(std::unordered_map<std::string, std::uint32_t> ranks) {
    auto b = std::unique_ptr<Bpe>(new Bpe());
    b->ranks_ = std::move(ranks);
    return b;
}

std::unique_ptr<Bpe> Bpe::LoadTiktoken(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return nullptr;
    std::unordered_map<std::string, std::uint32_t> ranks;
    ranks.reserve(110000);
    std::string line;
    while (std::getline(in, line)) {
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        try { ranks.emplace(B64Decode(line.substr(0, sp)), static_cast<std::uint32_t>(std::stoul(line.substr(sp + 1)))); } catch (...) {}
    }
    if (ranks.size() < 256) return nullptr;
    return FromRanks(std::move(ranks));
}

void Bpe::EncodePiece(const std::string& piece, std::vector<std::uint32_t>& out) const {
    if (auto it = ranks_.find(piece); it != ranks_.end()) { out.push_back(it->second); return; }
    std::vector<std::string> parts;
    for (char ch : piece) parts.emplace_back(1, ch);
    const auto kMax = std::numeric_limits<std::uint32_t>::max();
    while (parts.size() > 1) {
        std::uint32_t best = kMax; std::size_t bi = 0;
        for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
            auto it = ranks_.find(parts[k] + parts[k + 1]);
            if (it != ranks_.end() && it->second < best) { best = it->second; bi = k; }
        }
        if (best == kMax) break;
        parts[bi] += parts[bi + 1];
        parts.erase(parts.begin() + static_cast<long>(bi) + 1);
    }
    for (auto& p : parts) {
        auto it = ranks_.find(p);
        out.push_back(it != ranks_.end() ? it->second : kMax);  // unknown byte (incomplete vocab): sentinel
    }
}

std::vector<std::uint32_t> Bpe::Encode(const std::string& text) const {
    std::vector<std::uint32_t> out;
    for (const auto& p : PreTokenize(text)) EncodePiece(p, out);
    return out;
}

fs::path DefaultPath() {
    if (const char* p = std::getenv("OPENCLAW_TOKENIZER_PATH"); p && *p) return p;
    const char* h = std::getenv("HOME");
    return fs::path(h && *h ? h : "/tmp") / ".openclaw" / "tokenizers" / "cl100k_base.tiktoken";
}

const Bpe* Default() {
    static std::once_flag once;
    static std::unique_ptr<Bpe> inst;
    std::call_once(once, [] { inst = Bpe::LoadTiktoken(DefaultPath()); });
    return inst.get();
}

std::size_t CountTokens(const std::string& text) {
    if (const Bpe* b = Default()) return b->Count(text);
    return (text.size() + 3) / 4;
}

const char* Backend() { return Default() ? "cl100k_base" : "heuristic"; }

}  // namespace openclaw::tokenizer
