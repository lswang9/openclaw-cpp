#pragma once
// Feishu/Lark long-connection binary frames (larksuite SDK `pbbp2.Frame`, protobuf):
//   1 SeqID uint64 · 2 LogID uint64 · 3 service int32 · 4 method int32 (0 control, 1 data)
//   5 headers repeated {1 key string, 2 value string} · 6 payload_encoding · 7 payload_type · 8 payload bytes · 9 LogIDNew
// Control frames carry header type=ping|pong; data frames type=event|card with message_id, sum, seq, trace_id.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openclaw::channels::feishu_frame {

struct Frame {
    std::uint64_t seq_id = 0, log_id = 0;
    std::int32_t service = 0, method = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string payload_encoding, payload_type, payload, log_id_new;
    std::string Header(const std::string& k) const {
        for (auto& [a, b] : headers) if (a == k) return b;
        return {};
    }
    void SetHeader(const std::string& k, const std::string& v) {
        for (auto& [a, b] : headers) if (a == k) { b = v; return; }
        headers.emplace_back(k, v);
    }
};
constexpr std::int32_t kControl = 0, kData = 1;

std::string Encode(const Frame& f);
std::optional<Frame> Decode(const std::string& bytes);
Frame Ping(std::int32_t service_id);
// Ack for a data frame: same frame, payload {"code":200,...}, header biz_rt.
Frame Response(const Frame& in, int code, long long biz_rt_ms);

// Multi-part payloads (sum > 1) are reassembled by message_id.
class Reassembler {
public:
    // Returns the full payload when all `sum` parts have arrived.
    std::optional<std::string> Add(const Frame& f);
private:
    std::map<std::string, std::vector<std::optional<std::string>>> parts_;
};

}  // namespace openclaw::channels::feishu_frame
