# The guest half: Eden's libraries plus the core (driver/: the boot code a
# frontend has, the machine's filesystem, the GPU bridge's guest half and the
# exports), as archives build-core.sh links into core.wbx. Built by Eden's
# own CMake with CHIMERA_HEADLESS (patch 0001), in place of the frontends;
# CMAKE_SOURCE_DIR is extern/eden.
set(W ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GEN ${W}/../build/generated-gl)
if(DEFINED ENV{MINIBOX_DIR})
	set(MB "$ENV{MINIBOX_DIR}" CACHE PATH "miniBox checkout")
else()
	set(MB "$ENV{HOME}/chimera/extern/chimera-common-minibox" CACHE PATH "miniBox checkout")
endif()

add_library(eden-guest-core STATIC
	${W}/driver/eden_driver.cpp
	${W}/driver/kept_images.cpp
	${W}/driver/nsz_tool.cpp
	${W}/driver/vma.cpp
	${W}/driver/wbx-entry.cpp
	${W}/driver/memfs.cpp
	${W}/driver/guest-libc.cpp
	${GEN}/gl-bridge-guest.cpp
)
target_include_directories(eden-guest-core PRIVATE ${CMAKE_SOURCE_DIR}/src ${W}/driver
	${MB}/source/gl ${GEN} ${MB}/extern/emulibc ${MB}/source/guest/include)
# the generated wrappers want glad 2's types: built apart from Eden's glad
set_source_files_properties(${GEN}/gl-bridge-guest.cpp PROPERTIES INCLUDE_DIRECTORIES "${W}/glad/include;${GEN};${MB}/source/gl")
target_link_libraries(eden-guest-core PUBLIC common core hid_core audio_core video_core frontend_common glad fmt::fmt
	GPUOpen::VulkanMemoryAllocator Vulkan::Headers ZLIB::ZLIB)
