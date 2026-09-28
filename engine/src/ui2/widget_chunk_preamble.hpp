#pragma once

// Common private context for the independently compiled widget groups.
#include <kin/ui2/widgets.hpp>
#include <kin/platform/log.hpp>
#include <kin/ui2/context.hpp>

#include "style_internal.hpp"
#include "widget_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kin::ui2 {
using namespace detail;
} // namespace kin::ui2
