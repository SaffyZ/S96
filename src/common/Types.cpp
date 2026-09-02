#include "common/Types.hpp"
#include <cstdint>

namespace ps96 {
static_assert(sizeof(u8) == 1);
static_assert(sizeof(u16) == 2);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(u64) == 8);
}
