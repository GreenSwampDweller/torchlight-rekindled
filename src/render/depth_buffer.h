// The window's depth buffer, as a GL depth bias is counted in units of its resolution.
#pragma once

namespace tl::render {

/// 2^bits of the window's depth buffer. The desktop's GL is asked for 24; a GLES window is
/// asked what it got, once, with its context current.
float depth_buffer_units();

}  // namespace tl::render
