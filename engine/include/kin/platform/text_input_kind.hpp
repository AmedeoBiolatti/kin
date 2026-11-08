#pragma once

#include <kin/core/types.hpp>

namespace kin {

// What a text field takes, so a phone shows the keyboard for it: letters, or
// its number pad (Decimal: digits and a decimal point). A desktop keyboard is
// the same for each.
enum class TextInputKind : u8 {
    Text,
    Number,
    Decimal,
};

} // namespace kin
