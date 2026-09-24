# Toolchain for cross-compiling a Windows x64 build on Linux with MinGW-w64.
#
#   cmake -S . -B build-win \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#       -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/mingw_64 \
#       -DQT_HOST_PATH=/path/to/Qt/6.8.3/gcc_64
#
# Needs the MinGW-w64 cross compiler with POSIX threads
# (Debian/Ubuntu: g++-mingw-w64-x86-64-posix).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLET x86_64-w64-mingw32)

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER ${MINGW_TRIPLET}-gcc-posix)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER ${MINGW_TRIPLET}-g++-posix)
endif()
if(NOT DEFINED CMAKE_RC_COMPILER)
    set(CMAKE_RC_COMPILER ${MINGW_TRIPLET}-windres)
endif()

# Look for headers and libraries only in the target environment, but run
# programs (moc, rcc, ...) from the host.
set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TRIPLET})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
