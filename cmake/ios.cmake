# iOS platform configuration. Build OGRE separately with its iOS toolchain.
set(TL_OGRE_BUILD_DIR "${CMAKE_SOURCE_DIR}/build-ogre-ios-xcode" CACHE PATH "iOS OGRE build")
set(TL_IOS_DEPENDENCIES_DIR "${TL_OGRE_BUILD_DIR}/Dependencies" CACHE PATH
    "iOS OGRE dependency build (FreeType headers and libraries)")
if(EXISTS "${TL_OGRE_BUILD_DIR}/lib")
    set(TL_HAVE_OGRE TRUE)
else()
    set(TL_HAVE_OGRE FALSE)
    message(STATUS "No iOS OGRE at ${TL_OGRE_BUILD_DIR}; renderer will not be linked")
endif()
set(TL_OGRE_LIBS OgreMainStatic RenderSystem_GLES2Static OgreGLSupportStatic
    Codec_STBIStatic OgreRTShaderSystemStatic OgreOverlayStatic)
set(TL_IOS_PLATFORM_DIR iphoneos)
if(IOS_PLATFORM STREQUAL "SIMULATOR")
    set(TL_IOS_PLATFORM_DIR iphonesimulator)
endif()
set(TL_IOS_OGRE_LIB_DIR "${TL_OGRE_BUILD_DIR}/lib/${TL_IOS_PLATFORM_DIR}/Release")
function(tl_configure_platform target)
    target_compile_definitions(${target} PRIVATE TL_PLATFORM_IOS=1)
    if(NOT TL_HAVE_OGRE)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE TL_HAVE_OGRE=1 OGRE_STATIC_LIB TL_OGRE_MEDIA_DIR="")
    target_include_directories(${target} SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/external/ogre/Components/Overlay/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/OgreMain/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GLES2/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GLES2/include/EAGL"
        "${CMAKE_SOURCE_DIR}/external/ogre/RenderSystems/GLSupport/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/Components/RTShaderSystem/include"
        "${CMAKE_SOURCE_DIR}/external/ogre/PlugIns/STBICodec/include"
        "${TL_OGRE_BUILD_DIR}/include")
target_link_directories(${target} PRIVATE "${TL_IOS_OGRE_LIB_DIR}" "${TL_IOS_DEPENDENCIES_DIR}/lib")
target_include_directories(${target} SYSTEM PRIVATE "${TL_IOS_DEPENDENCIES_DIR}/include"
    "${TL_IOS_DEPENDENCIES_DIR}/include/freetype2")
set(TL_IOS_FREETYPE "${TL_IOS_DEPENDENCIES_DIR}/lib/libfreetype.a")
    target_link_libraries(${target} PRIVATE ${TL_OGRE_LIBS} "${TL_IOS_FREETYPE}" z
        "-framework UIKit" "-framework Foundation" "-framework QuartzCore" "-framework OpenGLES"
        "-framework CoreGraphics" "-framework CoreFoundation" "-framework AVFoundation" "-framework AudioToolbox"
        "-framework GameController")
endfunction()
