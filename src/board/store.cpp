#include "openclaw/board/store.hpp"
#include "openclaw/persist/state_db.hpp"

#include <chrono>
#include <fstream>

namespace openclaw::board {
namespace fs = std::filesystem;

namespace {

nlohmann::json DefaultBoard() {
    return {{"columns",
             nlohmann::json::array({{{"id", "todo"}, {"title", "Todo"}, {"order", 0}},
                                    {{"id", "doing"}, {"title", "Doing"}, {"order", 1}},
                                    {{"id", "done"}, {"title", "Done"}, {"order", 2}}})},
            {"cards", nlohmann::json::array()},
            {"version", 1}};
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Store::Store(fs::path path) : path_(std::move(path)) {
    Load();
    if (!data_.contains("columns") || !data_["columns"].is_array() || data_["columns"].empty()) {
        data_ = DefaultBoard();
        Save();
    }
}

void Store::Load() {
    auto text = persist::StateRead(path_);
    if (!text) {
        data_ = DefaultBoard();
        return;
    }
    try {
        data_ = nlohmann::json::parse(*text);
        if (!data_.is_object()) data_ = DefaultBoard();
        if (!data_.contains("cards") || !data_["cards"].is_array()) data_["cards"] = nlohmann::json::array();
        if (!data_.contains("columns") || !data_["columns"].is_array()) {
            data_["columns"] = DefaultBoard()["columns"];
        }
        // recover seq from ids
        for (const auto& c : data_["cards"]) {
            auto id = c.value("id", "");
            if (id.rfind("card-", 0) == 0) {
                try {
                    int n = std::stoi(id.substr(5));
                    if (n >= seq_) seq_ = n + 1;
                } catch (...) {
                }
            }
        }
    } catch (...) {
        data_ = DefaultBoard();
    }
}

void Store::Save() const {
    persist::StateWrite(path_, data_.dump(2) + "\n");
}

std::string Store::NextId(const std::string& prefix) {
    return prefix + std::to_string(seq_++);
}

nlohmann::json* Store::FindCard(const std::string& id) {
    if (!data_.contains("cards")) return nullptr;
    for (auto& c : data_["cards"]) {
        if (c.value("id", "") == id) return &c;
    }
    return nullptr;
}

const nlohmann::json* Store::FindCard(const std::string& id) const {
    if (!data_.contains("cards")) return nullptr;
    for (const auto& c : data_["cards"]) {
        if (c.value("id", "") == id) return &c;
    }
    return nullptr;
}

nlohmann::json Store::Get() const {
    std::lock_guard lock(mu_);
    return data_;
}

nlohmann::json Store::Status() const {
    std::lock_guard lock(mu_);
    return {{"path", path_.string()},
            {"columns", data_.value("columns", nlohmann::json::array()).size()},
            {"cards", data_.value("cards", nlohmann::json::array()).size()},
            {"impl", "real"},
            {"note", "local JSON board — not full OpenClaw board UX"}};
}

nlohmann::json Store::EnsureDefaults() {
    std::lock_guard lock(mu_);
    if (!data_.contains("columns") || data_["columns"].empty()) {
        data_ = DefaultBoard();
        Save();
    }
    return data_;
}

nlohmann::json Store::SetBoard(const nlohmann::json& board) {
    std::lock_guard lock(mu_);
    data_ = board.is_object() ? board : DefaultBoard();
    if (!data_.contains("columns")) data_["columns"] = DefaultBoard()["columns"];
    if (!data_.contains("cards")) data_["cards"] = nlohmann::json::array();
    Save();
    return data_;
}

nlohmann::json Store::AddColumn(const std::string& title, const std::string& id) {
    std::lock_guard lock(mu_);
    auto& cols = data_["columns"];
    std::string cid = id.empty() ? NextId("col-") : id;
    int order = static_cast<int>(cols.size());
    cols.push_back({{"id", cid}, {"title", title.empty() ? cid : title}, {"order", order}});
    Save();
    return {{"ok", true}, {"column", cols.back()}, {"board", data_}};
}

nlohmann::json Store::AddCard(const std::string& column_id, const std::string& title,
                              const std::string& body) {
    std::lock_guard lock(mu_);
    std::string col = column_id.empty() ? "todo" : column_id;
    bool col_ok = false;
    for (const auto& c : data_["columns"]) {
        if (c.value("id", "") == col) {
            col_ok = true;
            break;
        }
    }
    if (!col_ok && !data_["columns"].empty()) col = data_["columns"][0].value("id", "todo");

    int order = 0;
    for (const auto& card : data_["cards"]) {
        if (card.value("column_id", "") == col) order = std::max(order, card.value("order", 0) + 1);
    }
    nlohmann::json card{{"id", NextId("card-")},
                        {"column_id", col},
                        {"title", title.empty() ? "Untitled" : title},
                        {"body", body},
                        {"order", order},
                        {"updated_ms", NowMs()}};
    data_["cards"].push_back(card);
    Save();
    return {{"ok", true}, {"card", card}, {"board", data_}};
}

nlohmann::json Store::UpdateCard(const std::string& id, const nlohmann::json& patch) {
    std::lock_guard lock(mu_);
    auto* c = FindCard(id);
    if (!c) return {{"ok", false}, {"error", "card not found"}, {"id", id}};
    if (patch.contains("title")) (*c)["title"] = patch["title"];
    if (patch.contains("body")) (*c)["body"] = patch["body"];
    if (patch.contains("column_id")) (*c)["column_id"] = patch["column_id"];
    if (patch.contains("order")) (*c)["order"] = patch["order"];
    (*c)["updated_ms"] = NowMs();
    Save();
    return {{"ok", true}, {"card", *c}, {"board", data_}};
}

nlohmann::json Store::MoveCard(const std::string& id, const std::string& column_id, int order) {
    std::lock_guard lock(mu_);
    auto* c = FindCard(id);
    if (!c) return {{"ok", false}, {"error", "card not found"}, {"id", id}};
    if (!column_id.empty()) (*c)["column_id"] = column_id;
    if (order >= 0) (*c)["order"] = order;
    (*c)["updated_ms"] = NowMs();
    Save();
    return {{"ok", true}, {"card", *c}, {"board", data_}};
}

nlohmann::json Store::DeleteCard(const std::string& id) {
    std::lock_guard lock(mu_);
    auto& cards = data_["cards"];
    nlohmann::json next = nlohmann::json::array();
    bool found = false;
    for (auto& c : cards) {
        if (c.value("id", "") == id) {
            found = true;
            continue;
        }
        next.push_back(c);
    }
    if (!found) return {{"ok", false}, {"error", "card not found"}, {"id", id}};
    cards = next;
    Save();
    return {{"ok", true}, {"deleted", id}, {"board", data_}};
}

}  // namespace openclaw::board
