cmake_minimum_required(VERSION 3.31 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)

# Set experimental flag to enable `import std` support from CMake. This must be
# enabled before C++ language support.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    # This specific value changes as experimental support evolves. See
    # `Help/dev/experimental.rst` in the CMake source corresponding to
    # your CMake build for the exact value to use.
    "d0edc3af-4c50-42ea-a356-e2862fe7a444")

project(autosshpp LANGUAGES CXX)

add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8;/Zc:__cplusplus;/Zc:preprocessor>")

if(WIN32)
    # macro see @ https://stackoverflow.com/a/40217291/1746503
    macro(get_win32_winnt version)
        if(CMAKE_SYSTEM_VERSION)
            set(ver ${CMAKE_SYSTEM_VERSION})
            string(REGEX MATCH "^([0-9]+).([0-9])" ver ${ver})
            string(REGEX MATCH "^([0-9]+)" verMajor ${ver})
            # Check for Windows 10, b/c we'll need to convert to hex 'A'.
            if("${verMajor}" MATCHES "10")
                set(verMajor "A")
                string(REGEX REPLACE "^([0-9]+)" ${verMajor} ver ${ver})
            endif("${verMajor}" MATCHES "10")
            # Remove all remaining '.' characters.
            string(REPLACE "." "" ver ${ver})
            # Prepend each digit with a zero.
            string(REGEX REPLACE "([0-9A-Z])" "0\\1" ver ${ver})
            set(${version} "0x${ver}")
        endif()
    endmacro()

    if(NOT DEFINED _WIN32_WINNT)
        get_win32_winnt(ver)
        set(_WIN32_WINNT ${ver})
    endif()

    message(STATUS "Set _WIN32_WINNET=${_WIN32_WINNT}")

    add_compile_definitions(_WIN32_WINNT=${_WIN32_WINNT})
endif()

include(FetchContent)

FetchContent_Declare(
    argparse
    # GIT_REPOSITORY https://ghfast.top/https://github.com/p-ranav/argparse.git
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
    GIT_TAG v3.2
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_Declare(
    spdlog
    # GIT_REPOSITORY https://ghfast.top/https://github.com/gabime/spdlog.git
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_Declare(
    Boost
    # URL https://ghfast.top/https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.7z
    URL https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.7z
    URL_HASH SHA256=218e74c4aa362a994b7b7a23b2920f455a00205c656405fcf262cf60b8871921
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
)
set(BOOST_INCLUDE_LIBRARIES asio process)
add_compile_definitions(BOOST_PROCESS_USE_STD_FS)
FetchContent_MakeAvailable(
    argparse
    spdlog
    Boost
)

add_executable(autossh)

target_compile_definitions(autossh PRIVATE $<$<BOOL:${WIN32}>:_WIN32_WINNT=0x0A01> BOOST_ALL_NO_LIB)

target_sources(autossh
    PRIVATE
        src/config.cpp
        src/main.cpp
        src/autossh.cpp
        src/platform_control.cpp
    PRIVATE
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            src/config.ixx
            src/platform_control.ixx
            src/autossh.ixx
)

target_link_libraries(autossh PRIVATE
    argparse
    spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
    Boost::asio Boost::process
)

# cmake -DCMAKE_BUILD_TYPE:STRING=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE -DCMAKE_C_COMPILER:FILEPATH=/usr/bin/clang-20 -DCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++-20 --no-warn-unused-cli -S /root/projects/Autosshpp -B /root/projects/Autosshpp/build -G Ninja -DBOOST_PROCESS_USE_STD_FS=ON -DCMAKE_CXX_FLAGS=-stdlib=libc++
