# Layton2loader-specific build settings, included by the top-level CMakeLists.
#
# The game's cutscenes are H.264/AAC in MP4 and the engine composites them
# itself, so the loader only has to decode them (see movie.cpp).

set(FFMPEG_MIN_DIR ${PROJ_SOURCE_DIR}/third_party/ffmpeg-min)

if(EXISTS ${FFMPEG_MIN_DIR}/lib/libavcodec.a)
    # Purpose-built FFmpeg: h264 + aac decoders and the mov demuxer only, with
    # --disable-everything --disable-autodetect so it links no codec libraries
    # of its own. Static, so the loader ships without any libav* .so files --
    # the distro build needs 133 shared libraries for these same two codecs.
    # Built by projects/layton2loader/tools/build_ffmpeg_min.sh
    target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${FFMPEG_MIN_DIR}/include)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
        # order matters for static archives: dependents before dependencies
        ${FFMPEG_MIN_DIR}/lib/libavformat.a
        ${FFMPEG_MIN_DIR}/lib/libavcodec.a
        ${FFMPEG_MIN_DIR}/lib/libswscale.a
        ${FFMPEG_MIN_DIR}/lib/libswresample.a
        ${FFMPEG_MIN_DIR}/lib/libavutil.a
        m atomic
    )
    message(STATUS "Layton2loader: cutscenes via bundled static FFmpeg")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale libswresample)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::FFMPEG)
    message(WARNING "Layton2loader: using the system FFmpeg, which needs many .so files on "
                    "the target. Run projects/layton2loader/tools/build_ffmpeg_min.sh "
                    "for a self-contained build.")
endif()
