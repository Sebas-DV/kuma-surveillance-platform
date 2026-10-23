#pragma once

#include "domain/Result.hpp"

namespace dahua {

[[nodiscard]] domain::Error loginErrorFromNative(int nativeCode);

}
