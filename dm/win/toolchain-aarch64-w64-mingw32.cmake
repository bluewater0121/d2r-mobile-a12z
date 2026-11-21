# llvm-mingw 交叉工具链：在 x86_64 或 ARM64 的 Windows 上都能产出 aarch64 目标。
# 用 scoop 的 mingw-mstorsjo-llvm-ucrt（自带 winpthreads 的 libpthread.a，宿主大量用 pthread）。
# llvm-mingw cross toolchain: works on an x86_64 or ARM64 Windows host and always targets aarch64.
# Uses scoop's mingw-mstorsjo-llvm-ucrt (bundles winpthreads' libpthread.a, which the host relies on heavily).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(DM_MINGW_PREFIX C:/Users/zhcy/scoop/apps/mingw-mstorsjo-llvm-ucrt/current)
set(CMAKE_C_COMPILER   ${DM_MINGW_PREFIX}/bin/aarch64-w64-mingw32-clang.exe)
set(CMAKE_CXX_COMPILER ${DM_MINGW_PREFIX}/bin/aarch64-w64-mingw32-clang++.exe)
set(CMAKE_RC_COMPILER  ${DM_MINGW_PREFIX}/bin/llvm-rc.exe)
set(CMAKE_AR           ${DM_MINGW_PREFIX}/bin/llvm-ar.exe)
set(CMAKE_RANLIB       ${DM_MINGW_PREFIX}/bin/llvm-ranlib.exe)
set(CMAKE_FIND_ROOT_PATH ${DM_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# 静态链 winpthreads，避免目标机上缺 libwinpthread-1.dll（Windows 不自带，玩家机器上不会有）
# Link winpthreads statically so the target machine does not need libwinpthread-1.dll (not shipped with Windows, absent on players' machines)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc")
set(CMAKE_CXX_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_C_FLAGS_INIT "-static-libgcc")
