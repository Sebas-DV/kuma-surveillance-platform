#pragma once

#include <json/json.h>

#include <string>

#include "persistence/Models.hpp"

namespace api {

[[nodiscard]] std::string toIso(persistence::TimePoint time);

[[nodiscard]] Json::Value toJson(const persistence::Channel& channel);
[[nodiscard]] Json::Value toJson(const persistence::Device& device);
[[nodiscard]] Json::Value toJson(const persistence::User& user);
[[nodiscard]] Json::Value toJson(const persistence::ApiKey& key);

}
