#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace centrifugo {

struct ClientInfo {
    std::string user;
    std::string client;
};

struct Publication {
    std::uint64_t offset {0};
    nlohmann::json data;
    std::optional<ClientInfo> info;
    std::unordered_map<std::string, std::string> tags;
    std::string epoch;
};

// A point in a channel's history stream.
struct StreamPosition {
    std::uint64_t offset {0};
    std::string epoch;
};

struct HistoryOptions {
    // 0 returns no publications, only the stream's current position.
    std::int32_t limit {0};
    // Only publications after this position; without it, from the start of retained history.
    std::optional<StreamPosition> since;
    // Newest first instead of oldest first.
    bool reverse {false};
};

struct HistoryResult {
    std::vector<Publication> publications;
    // The stream's current position, not that of the last returned publication.
    std::uint64_t offset {0};
    std::string epoch;
};

}
