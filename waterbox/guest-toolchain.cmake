# The miniBox waterbox guest as a CMake toolchain: musl + static libstdc++,
# large code model, no PIC, no host libraries (as the other C++ cores have it, plus the
# libraries build-deps.sh builds for the guest on the find path).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{MINIBOX_SYSROOT})
  set(SR "$ENV{MINIBOX_SYSROOT}")
elseif(DEFINED ENV{MINIBOX_DIR})
  set(SR "$ENV{MINIBOX_DIR}/build/meson-cpp/guest-sysroot")
else()
  set(SR "$ENV{HOME}/chimera/extern/chimera-common-minibox/build/meson-cpp/guest-sysroot")
endif()
if(NOT EXISTS "${SR}/lib/musl-gcc.specs")
  message(FATAL_ERROR "miniBox guest sysroot not found at ${SR}: build it (meson setup <miniBox>/build/meson-cpp <miniBox> -Dguest_cpp=true && ninja -C <miniBox>/build/meson-cpp) or set MINIBOX_DIR")
endif()
get_filename_component(DEPS "${CMAKE_CURRENT_LIST_DIR}/../build/deps" ABSOLUTE)

execute_process(COMMAND gcc -dumpfullversion OUTPUT_VARIABLE GCCVER OUTPUT_STRIP_TRAILING_WHITESPACE)

# guest/include: the kernel header Boost asks for, saying "no kernel", and
# chimera-guest.h, before every source: what musl 1.2.0 does not declare.
# ZDICT_QSORT=0: zstd's dictionary builder with plain qsort (musl 1.2.0 has
# no qsort_r).
set(WB "-specs=${SR}/lib/musl-gcc.specs -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -DCHIMERA_GUEST -DZDICT_QSORT=0 -I${CMAKE_CURRENT_LIST_DIR}/guest/include -include ${CMAKE_CURRENT_LIST_DIR}/guest/include/chimera-guest.h")
set(CMAKE_C_FLAGS_INIT "${WB}")
set(CMAKE_CXX_FLAGS_INIT "${WB} -nostdinc++ -I${SR}/include/c++/${GCCVER} -I${SR}/include/c++/${GCCVER}/x86_64-linux-musl")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -nostdlib++")

# the host's pkg-config would hand the guest the host's libraries (SDL found
# dbus and ibus that way)
set(PKG_CONFIG_EXECUTABLE "/bin/false" CACHE FILEPATH "no host pkg-config for the guest" FORCE)

# feature probes must not try to run guest binaries
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${SR}" "${DEPS}/openssl-guest" "${DEPS}/ffmpeg-guest")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
