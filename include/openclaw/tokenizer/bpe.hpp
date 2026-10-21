#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace openclaw::tokenizer {

// Byte-level BPE compatible with tiktoken rank files (cl100k_base layout: "<base64 bytes> <rank>" per line),
// using a hand-written cl100k pre-tokenizer (no regex engine). Unicode letter classes are approximated:
// non-ASCII code points count as letters except common punctuation/space/symbol blocks.
class Bpe {
public:
    static std::unique_ptr<Bpe> LoadTiktoken(const std::filesystem::path& file);  // nullptr on failure
    static std::unique_ptr<Bpe> FromRanks(std::unordered_map<std::string, std::uint32_t> ranks);
    std::vector<std::uint32_t> Encode(const std::string& text) const;
    std::size_t Count(const std::string& text) const { return Encode(text).size(); }
    std::size_t VocabSize() const { return ranks_.size(); }

private:
    void EncodePiece(const std::string& piece, std::vector<std::uint32_t>& out) const;
    std::unordered_map<std::string, std::uint32_t> ranks_;
};

// cl100k-style pre-tokenization (exposed for tests).
std::vector<std::string> PreTokenize(const std::string& text);

// Process-wide tokenizer: OPENCLAW_TOKENIZER_PATH, else ~/.openclaw/tokenizers/cl100k_base.tiktoken.
// Falls back to a chars/4 heuristic when no vocabulary file is present.
const Bpe* Default();
std::filesystem::path DefaultPath();
std::size_t CountTokens(const std::string& text);
const char* Backend();  // "cl100k_base" | "heuristic"

}  // namespace openclaw::tokenizer
