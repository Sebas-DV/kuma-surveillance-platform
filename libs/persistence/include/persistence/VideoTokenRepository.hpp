#pragma once

#include <memory>
#include <string_view>

#include "domain/Result.hpp"
#include "persistence/Database.hpp"
#include "persistence/Models.hpp"

namespace persistence {

inline constexpr std::chrono::seconds kMinVideoTokenTtl{5};
inline constexpr std::chrono::seconds kMaxVideoTokenTtl{300};
inline constexpr std::chrono::hours kViewingSession{8};

class VideoTokenRepository {
public:
    explicit VideoTokenRepository(std::shared_ptr<Database> database);

    [[nodiscard]] domain::Result<IssuedVideoToken> issue(const VideoTokenRequest& request);
    [[nodiscard]] domain::Result<VideoGrant> consume(std::string_view token, std::string_view clientAddress);
    [[nodiscard]] domain::Result<std::size_t> purgeExpired();

private:
    std::shared_ptr<Database> database_;
};

}
