# Windows ARM64 宿主构建（llvm-mingw，aarch64-w64-mingw32）。组织方式照抄 dm/android/CMakeLists.txt：源清单从 dm/host/Makefile 的 SRC / GFX / LIBDEFLATE 取，
# 不手抄；汇编仍是 Mach-O 写法，经 tools/asm_coff.py 转 COFF 后编；compat/include 只在 Windows 构建的头文件路径里。
#
# 与安卓的两处实质差异：
#   1) Vulkan 没有 ARM64 导入库，直接链 C:\Windows\System32\vulkan-1.dll（lld 接受 DLL 路径）；`log` 库在 Windows 上不存在，
#      这里用空的 INTERFACE 目标顶替，好让 gfx/vulkan/CMakeLists.txt 原样复用（不产生引用，链接时自然消失）。
#   2) 不做 SPIRV-Tools 内联优化（那是 Adreno 750 的驱动怪癖，Windows 上用不到），所以 dm_spirv_opt 不加入。
#
# 必需：-DDM_GEN_DIR=<内含 dm_vault_tab.h / dm_hud_exp.h / dm_bright_levels.inc 的目录>
# 可选：-DDM_VULKAN_INCLUDE=…（缺省 $ENV{VULKAN_SDK}/Include 或 scoop 路径）、-DDM_VULKAN_DLL=…（缺省 C:/Windows/System32/vulkan-1.dll）
# 产物：dmhost.exe（命令行宿主，开发验证用）
#
# Windows ARM64 host build (llvm-mingw, aarch64-w64-mingw32). Organised like dm/android/CMakeLists.txt: the source list comes from SRC / GFX / LIBDEFLATE
# in dm/host/Makefile rather than being copied by hand; assembly is still Mach-O syntax, converted to COFF by tools/asm_coff.py before assembling;
# compat/include is only on the Windows build's header path.
#
# Two substantive differences from Android:
#   1) Vulkan has no ARM64 import library, so we link C:\Windows\System32\vulkan-1.dll directly (lld accepts a DLL path); the `log` library does not
#      exist on Windows, so an empty INTERFACE target stands in for it, letting gfx/vulkan/CMakeLists.txt be reused unchanged (no references are
#      produced, so it vanishes at link time).
#   2) No SPIRV-Tools inlining (that is an Adreno 750 driver quirk, useless on Windows), so dm_spirv_opt is not added.
#
# Required: -DDM_GEN_DIR=<directory with dm_vault_tab.h / dm_hud_exp.h / dm_bright_levels.inc>
# Optional: -DDM_VULKAN_INCLUDE=… (default $ENV{VULKAN_SDK}/Include or the scoop path), -DDM_VULKAN_DLL=… (default C:/Windows/System32/vulkan-1.dll)
# Output: dmhost.exe (command-line host, for development checks)
cmake_minimum_required(VERSION 3.22)
project(dmhost_win LANGUAGES C CXX ASM)
set(DM ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(HOST ${DM}/host)
get_filename_component(DM_PATH_ROOT ${DM}/.. ABSOLUTE)
foreach(map "${CMAKE_BINARY_DIR}=build" "${DM_GEN_DIR}=gen" "${DM_PATH_ROOT}=src")
  add_compile_options("-ffile-prefix-map=${map}")
endforeach()

if(NOT DM_GEN_DIR OR NOT EXISTS ${DM_GEN_DIR}/dm_vault_tab.h)
  message(FATAL_ERROR "要 -DDM_GEN_DIR=<由游戏文件生成的宿主表头目录>（dm_vault_tab.h / dm_hud_exp.h / dm_bright_levels.inc，见 dm/host/Makefile）")
endif()
if(NOT DM_VULKAN_INCLUDE)
  if(DEFINED ENV{VULKAN_SDK} AND EXISTS $ENV{VULKAN_SDK}/Include/vulkan/vulkan.h)
    set(DM_VULKAN_INCLUDE $ENV{VULKAN_SDK}/Include)
  else()
    set(DM_VULKAN_INCLUDE C:/Users/zhcy/scoop/apps/vulkan/current/Include)
  endif()
endif()
if(NOT DM_VULKAN_DLL)
  set(DM_VULKAN_DLL C:/Windows/System32/vulkan-1.dll)
endif()
if(NOT EXISTS ${DM_VULKAN_INCLUDE}/vulkan/vulkan.h)
  message(FATAL_ERROR "找不到 vulkan/vulkan.h（-DDM_VULKAN_INCLUDE=…）")
endif()

# 顶替 gfx/vulkan 期望的 `vulkan` / `log` 目标（见文件头说明 1）
# Stand-ins for the `vulkan` / `log` targets gfx/vulkan expects (see note 1 in the header)
add_library(vulkan INTERFACE)
target_include_directories(vulkan INTERFACE ${DM_VULKAN_INCLUDE})
target_link_libraries(vulkan INTERFACE ${DM_VULKAN_DLL})
add_library(log INTERFACE)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_subdirectory(../gfx/vulkan ${CMAKE_CURRENT_BINARY_DIR}/vulkan)
# 引擎也吃 Apple 风格的 include（sys/mman.h、mach/*），头文件路径与强制包含和 dmcore 对齐
# The engine also eats Apple-style includes (sys/mman.h, mach/*); align its header path and forced include with dmcore
target_include_directories(dm_vulkan_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat/include)
target_compile_definitions(dm_vulkan_engine PRIVATE DM_WIN=1 _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(dm_vulkan_engine PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/include/dm_win_compat.h)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HOST}/Makefile ${DM}/android/tools/host_sources.py)
execute_process(COMMAND ${Python3_EXECUTABLE} ${DM}/android/tools/host_sources.py
  ${HOST}/Makefile ${CMAKE_CURRENT_BINARY_DIR}/host_sources.cmake
  RESULT_VARIABLE parse_result)
if(NOT parse_result EQUAL 0)
  message(FATAL_ERROR "Cannot parse host sources for Windows")
endif()
include(${CMAKE_CURRENT_BINARY_DIR}/host_sources.cmake)
file(GLOB HLE_SRCS CONFIGURE_DEPENDS ${HOST}/hle_*.c)
file(GLOB GFX_SRCS CONFIGURE_DEPENDS ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)

# ---- 汇编：Mach-O → COFF | Assembly: Mach-O → COFF ----
set(ASM_SRCS)
foreach(s ${HOST}/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
  get_filename_component(n ${s} NAME)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/asm/${n})
  add_custom_command(OUTPUT ${out} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_coff.py ${s} ${out}
                     DEPENDS ${s} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_coff.py VERBATIM)
  list(APPEND ASM_SRCS ${out})
endforeach()

set(WIN_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/front_win.c
  ${CMAKE_CURRENT_SOURCE_DIR}/pad_win.c
  ${CMAKE_CURRENT_SOURCE_DIR}/stall_win.c
  ${CMAKE_CURRENT_SOURCE_DIR}/sched_win.c
  ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_present_win.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/tls_win.c
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/misc_win.c
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/mman_win.c
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/crash_win.c
  ${DM}/android/compat/digest.c
  ${DM}/gfx/vulkan/nvn_vk.c)

add_library(dmcore STATIC ${HOST_SRCS} ${HLE_SRCS} ${GFX_SRCS} ${LIBDEFLATE_SRCS} ${ASM_SRCS} ${WIN_SRCS})
list(REMOVE_DUPLICATES HOST_SRCS)
target_include_directories(dmcore PUBLIC
  ${HOST} ${DM}/gfx ${DM}/gfx/nvn/include ${DM}/gfx/vulkan ${DM}/gfx/vulkan/engine
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/include ${DM_GEN_DIR} ${DM_VULKAN_INCLUDE})
# 与安卓同档（-O1 -g），dm_win_compat.h 强制包含；客体代码要大量读写宿主内存，关掉严格别名
# Same tier as Android (-O1 -g), dm_win_compat.h force-included; guest code reads and writes host memory heavily, so strict aliasing is off
set(DM_CFLAGS -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-sign-compare -Wno-missing-braces -fno-strict-aliasing
  -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/include/dm_win_compat.h)
target_compile_definitions(dmcore PUBLIC DM_WIN=1 _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(dmcore PUBLIC $<$<COMPILE_LANGUAGE:C>:${DM_CFLAGS} -std=gnu17>)
target_compile_options(dmcore PUBLIC $<$<COMPILE_LANGUAGE:CXX>:${DM_CFLAGS} -std=gnu++17>)
# 引擎原样复用：POSITION_INDEPENDENT_CODE 在 Windows 上无意义但无害；它自带的 -O2 覆盖在上面
target_link_libraries(dmcore PUBLIC dm_vulkan_engine pthread bcrypt)

add_executable(dmhost WIN32 ${HOST}/main.c)
target_link_libraries(dmhost PRIVATE -Wl,--whole-archive dmcore -Wl,--no-whole-archive)
# 导出符号：崩溃回溯里 dladdr 能认出宿主函数（mingw 的 dladdr 走 PE 导出表）
# Export symbols so dladdr in crash backtraces can name host functions (mingw's dladdr uses the PE export table)
# -static：把 libc++ / libunwind / libwinpthread 都静态链进去，dmhost.exe 单文件就能跑（双击启动的 bat 里没有工具链的 bin 目录，缺 dll 会以 0xC0000135 秒退）
# -static: link libc++ / libunwind / libwinpthread statically so dmhost.exe runs on its own (the double-clickable bat has no toolchain bin directory on PATH; a missing dll made it exit at once with 0xC0000135)
target_link_options(dmhost PRIVATE -static -Wl,--export-all-symbols -Wl,--enable-auto-import)
target_link_libraries(dmhost PRIVATE ${DM_VULKAN_DLL})
set_target_properties(dmhost PROPERTIES OUTPUT_NAME dmhost)
# 运行期 DLL 摆在 exe 旁边：llvm-mingw 的 -static 吃掉了 libc++ / libunwind，但 libwinpthread-1.dll 仍是导入项，
# 而双击启动的 bat 里没有工具链的 bin 目录（缺 dll 时进程以 0xC0000135 秒退、日志里一个字都没有）
# Runtime dlls sit next to the exe: llvm-mingw's -static takes care of libc++ / libunwind, but libwinpthread-1.dll is still an import
# and the double-clickable bat has no toolchain bin directory on PATH (a missing dll exits at once with 0xC0000135 and nothing in the log)
get_filename_component(DM_MINGW_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
foreach(dll libwinpthread-1.dll libc++.dll libunwind.dll)
  if(EXISTS ${DM_MINGW_BIN}/${dll})
    add_custom_command(TARGET dmhost POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DM_MINGW_BIN}/${dll} $<TARGET_FILE_DIR:dmhost>)
  endif()
endforeach()

# 资源：图标 + 版本信息。windres 编出 COFF 目标文件，直接并进链接（EXTERNAL_OBJECT：CMake 别当源码去编）。
# 图标 dmhost.ico 由 tools/make_ico.py 从 iOS 的 AppIcon 生成，与安卓 / iOS / 鸿蒙同一套 App 图标。
# Resources: icon + version info. windres produces a COFF object that goes straight into the link
# (EXTERNAL_OBJECT keeps CMake from trying to compile it as source). dmhost.ico comes from tools/make_ico.py,
# generated from the iOS AppIcon, the same app icon Android / iOS / HarmonyOS use.
get_filename_component(DM_MINGW_ROOT ${DM_MINGW_BIN} DIRECTORY)
set(DM_RC_OBJ ${CMAKE_CURRENT_BINARY_DIR}/dmhost_res.o)
add_custom_command(OUTPUT ${DM_RC_OBJ}
  COMMAND ${DM_MINGW_BIN}/aarch64-w64-mingw32-windres.exe -I ${DM_MINGW_ROOT}/include
          -i ${CMAKE_CURRENT_SOURCE_DIR}/dmhost.rc -o ${DM_RC_OBJ} --output-format=coff
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dmhost.rc ${CMAKE_CURRENT_SOURCE_DIR}/dmhost.ico VERBATIM)
set_source_files_properties(${DM_RC_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_sources(dmhost PRIVATE ${DM_RC_OBJ})
