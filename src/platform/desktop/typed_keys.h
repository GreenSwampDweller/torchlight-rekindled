// What the desktop's keyboard types into the window, shared by its event pump and its input.
#pragma once

namespace tl::core {

/// One UTF-16 unit of a WM_CHAR, from the Windows event pump (platform_frame.cpp). Kept only
/// while a line is being typed (`typing()`).
void note_typed_unit(unsigned unit);

}  // namespace tl::core
