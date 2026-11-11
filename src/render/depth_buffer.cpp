#include "render/depth_buffer.h"

#include "core/log.h"

#include <cmath>

#if defined(TL_PLATFORM_ANDROID) || defined(TL_PLATFORM_SWITCH) || defined(TL_PLATFORM_IOS)
#define TL_HAS_GLES_DEPTH_BITS 1
#if defined(TL_PLATFORM_IOS)
#include <OpenGLES/ES2/gl.h>
#else
#include <GLES2/gl2.h>
#endif
#endif

namespace tl::render {

float depth_buffer_units() {
    static float units = 0.0f;
    if (units > 0.0f) return units;
    int bits = 24;
#if defined(TL_HAS_GLES_DEPTH_BITS)
    GLint got = 0;
    glGetIntegerv(GL_DEPTH_BITS, &got);
    if (got > 0) bits = static_cast<int>(got);
#endif
    units = std::ldexp(1.0f, bits);
    TL_LOG_INFO("depth buffer: %d bits", bits);
    return units;
}

}  // namespace tl::render
