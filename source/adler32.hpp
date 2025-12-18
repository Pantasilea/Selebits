#pragma once

#include <cstdint>
#include <span>

namespace sel {
    std::uint32_t adler32(std::span<const std::uint8_t> bytes);
}
