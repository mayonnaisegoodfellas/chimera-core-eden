# The native reference: the core's driver over Eden's libraries, the GPU
# bridge's host half in the same process. Built by Eden's own CMake with
# CHIMERA_HEADLESS (patch 0001), which adds this directory in place of the
# frontends; CMAKE_SOURCE_DIR is extern/eden.

find_package(OpenGL REQUIRED COMPONENTS EGL)

set(W ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GEN ${W}/../build/generated-gl)
if(DEFINED ENV{MINIBOX_DIR})
	set(MB "$ENV{MINIBOX_DIR}" CACHE PATH "miniBox checkout")
else()
	set(MB "$ENV{HOME}/chimera/extern/chimera-common-minibox" CACHE PATH "miniBox checkout")
endif()

# The bridge's host half and its own glad (glad 2) as ONE object whose only
# global symbols are chimera_gl_host_*: Eden's renderer has glad 1, and the
# two define the same glad_gl* pointers.
add_library(glhost-parts OBJECT ${W}/gl-host.c ${W}/glad/src/gl.c)
target_compile_definitions(glhost-parts PRIVATE CHIMERA_GL_BRIDGE)
target_include_directories(glhost-parts PRIVATE ${W}/glad/include ${GEN} ${MB}/source/gl ${MB}/source/host)
set(GLHOST ${CMAKE_CURRENT_BINARY_DIR}/glhost.o)
add_custom_command(OUTPUT ${GLHOST}
	COMMAND ld -r -o ${GLHOST}.tmp $<TARGET_OBJECTS:glhost-parts>
	COMMAND objcopy --wildcard --keep-global-symbol=chimera_gl_host_* ${GLHOST}.tmp ${GLHOST}
	DEPENDS glhost-parts $<TARGET_OBJECTS:glhost-parts>
	COMMAND_EXPAND_LISTS)

add_executable(eden-run-native
	run-native.cpp
	${W}/driver/eden_driver.cpp
	${W}/driver/kept_images.cpp
	${W}/driver/vma.cpp
	${W}/driver/wbx-entry.cpp
	${W}/driver/memfs.cpp
	${GEN}/gl-bridge-guest.cpp
	${GLHOST}
)
set_source_files_properties(${GLHOST} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_include_directories(eden-run-native PRIVATE ${CMAKE_SOURCE_DIR}/src ${W}/driver ${MB}/source/gl ${GEN})
# memfs defines both spellings of the C library's file calls (pread and
# pread64); Eden's _FILE_OFFSET_BITS=64 would make them one symbol
set_source_files_properties(${W}/driver/memfs.cpp PROPERTIES COMPILE_OPTIONS "-U_FILE_OFFSET_BITS")
# the generated wrappers want glad 2's types: built apart from Eden's glad
set_source_files_properties(${GEN}/gl-bridge-guest.cpp PROPERTIES INCLUDE_DIRECTORIES "${W}/glad/include;${GEN};${MB}/source/gl")
target_link_libraries(eden-run-native PRIVATE common core hid_core audio_core video_core frontend_common glad fmt::fmt
	GPUOpen::VulkanMemoryAllocator Vulkan::Headers ZLIB::ZLIB OpenGL::EGL)
# our FFmpeg is static: the libraries once more after everything, in their
# order of dependence
target_link_libraries(eden-run-native PRIVATE
	${FFMPEG_DIR}/lib/libavfilter.a ${FFMPEG_DIR}/lib/libswscale.a ${FFMPEG_DIR}/lib/libavcodec.a ${FFMPEG_DIR}/lib/libavutil.a m)
set_target_properties(eden-run-native PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# the machine's filesystem defines the C library's file calls: exported, so
# the shared libraries (libstdc++'s file streams) reach them too
target_link_options(eden-run-native PRIVATE "-Wl,--dynamic-list=${CMAKE_CURRENT_SOURCE_DIR}/memfs.list")
