#pragma once

// Whether ui2 is drawing inside a Context mirror scope (Context::push_mirror):
// align_rect leaves Start and End alone there, as the scope reflects them.

namespace kin::ui2::mirror_detail {

int depth();
void enter();
void leave();

} // namespace kin::ui2::mirror_detail
