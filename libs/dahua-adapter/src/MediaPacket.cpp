#include "dahua/MediaPacket.hpp"

namespace dahua {

DeviceDateTime decodeDeviceDateTime(const std::uint32_t packed) noexcept {
    DeviceDateTime result;
    result.second = static_cast<int>(packed & 0x3FU);
    result.minute = static_cast<int>((packed >> 6U) & 0x3FU);
    result.hour = static_cast<int>((packed >> 12U) & 0x1FU);
    result.day = static_cast<int>((packed >> 17U) & 0x1FU);
    result.month = static_cast<int>((packed >> 22U) & 0x0FU);
    result.year = static_cast<int>((packed >> 26U) & 0x3FU) + 2000;
    return result;
}

}
