## Project

This is a modern C++ reimplementation of the original autossh, which is a tool to automatically restart SSH sessions and tunnels.

Languange: C++
Build Tool: CMake 4.2
Language Standard: C++23 (Because we use the latest MSVC, we can and are encouraged to use some of the C++26 features to make the code cleaner and more efficient).
Libraries: Boost, argparse, spdlog are added in CMakeLists.txt. Feel free to add more if you want.

## Build

In PowerShell 7, I have a command to load the MSVC environment variables (only need to do it once per session):

```powershell
MSVC
```

Then goes the usual CMake build:

```powershell
cmake -B build -S . -GNinja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=1
cmake --build build -- -v
```

## Proxy

If ran into network issues, you can set the proxy environment variables:

```powershell
$env:HTTP_PROXY="http://127.0.0.1:10808"
$env:HTTPS_PROXY="http://127.0.0.1:10808"
$env:ALL_PROXY="socks5://127.0.0.1:10808"
```

## Reference Autossh implementation

See the directory `autossh-1.4f`. This is the original autossh implementation, which is a C program. You can refer to it for the logic and behavior of the program, but feel free to redesign the code structure. Our objective is to reimplement a more modern, more efficient, portable, and more advanced (e.g. using new network technologies) version of autossh, not to do a line-by-line translation. You can also refer to the original autossh documentation for the command line options and behavior.

## Version Control

Use Git for version control.