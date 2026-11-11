# Cross build of the Windows port on Linux: host clang/lld + an MSYS2 CLANG64 sysroot (the same
# packages as packaging/windows/README.md, unpacked by packaging/linux-cross/fetch_sysroot.py).
# Used by build-linux.sh; CLANG64_ROOT is the folder that holds clang64/ and res/.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if (NOT CLANG64_ROOT)
    set(CLANG64_ROOT "$ENV{CLANG64_ROOT}")
endif()
if (NOT CLANG64_ROOT OR NOT EXISTS "${CLANG64_ROOT}/clang64/include")
    message(FATAL_ERROR "Set CLANG64_ROOT to the unpacked MSYS2 CLANG64 sysroot (see build-linux.sh)")
endif()
# Try-compile projects re-read this file: pass the root on.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CLANG64_ROOT)

set(triple x86_64-w64-windows-gnu)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_RC_COMPILER llvm-windres)
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_SYSROOT "${CLANG64_ROOT}/clang64")

# The host clang's own headers with the sysroot's compiler-rt (res/ links both, same version).
set(cross_flags "-resource-dir=${CLANG64_ROOT}/res")
set(CMAKE_C_FLAGS_INIT "${cross_flags}")
set(CMAKE_CXX_FLAGS_INIT "${cross_flags} -stdlib=libc++")
set(link_flags "-fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${link_flags}")

# Libraries and headers from the sysroot; programs (glslang, python) from the host.
set(CMAKE_FIND_ROOT_PATH "${CLANG64_ROOT}/clang64")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config files say prefix=/clang64: map them into the sysroot.
set(ENV{PKG_CONFIG_LIBDIR} "${CLANG64_ROOT}/clang64/lib/pkgconfig:${CLANG64_ROOT}/clang64/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CLANG64_ROOT}")
set(ENV{PKG_CONFIG_PATH} "")
