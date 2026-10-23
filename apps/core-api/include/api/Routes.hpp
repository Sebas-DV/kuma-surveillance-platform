#pragma once

#include <memory>

#include "persistence/ApiKeyRepository.hpp"
#include "persistence/DeviceRepository.hpp"
#include "persistence/UserRepository.hpp"
#include "persistence/VideoTokenRepository.hpp"

namespace api {

struct Services {
    std::shared_ptr<persistence::DeviceRepository> devices;
    std::shared_ptr<persistence::UserRepository> users;
    std::shared_ptr<persistence::ApiKeyRepository> keys;
    std::shared_ptr<persistence::VideoTokenRepository> tokens;
};

void registerRoutes(const Services& services);

}
