# 生成 Windows 用的界面覆盖层（与 Mac App 同一套：原版手柄版布局 + 我们的 HUD / 面板改动 + 键盘键帽）。
# 产物是 build\win\overlay，运行时用 DM_ROMFS_OVERLAY 指过去（start-win-arm64.bat 会自己接上）。
#
# 生成过程要正编一个「块线性 ↔ 线性」转换库（dm/ios/tools/romfs_art.py 读贴图用）：有系统 clang 就直接编，
# 没有就由本脚本用 llvm-mingw 的 clang 编成 deswizzle.dll 并通过 DM_DESWIZZLE_LIB 交给它。
# 关键点：这个库要**与跑脚本的 python 同架构**（本机 python 若是 x64，就编 x86_64 版；ARM64 就编 aarch64 版），否则 ctypes 加载报 WinError 193。
#
# Generate the UI overlay for Windows (the same set the Mac App uses: the original controller layouts plus our HUD /
# panel changes and keyboard key caps). The output is build\win\overlay, and the runtime points at it with
# DM_ROMFS_OVERLAY (start-win-arm64.bat wires it up by itself).
#
# The generation compiles a block-linear <-> linear conversion library (dm/ios/tools/romfs_art.py reads textures with it):
# with a system clang it just builds; without one this script builds deswizzle.dll with llvm-mingw's clang and hands it
# over through DM_DESWIZZLE_LIB.
# Important: that library must match the architecture of the python running the tool (x64 python needs an x86_64 build,
# ARM64 python an aarch64 one), otherwise ctypes fails to load it with WinError 193.
param(
  [string]$RomFS = "C:\work\prj\Diablo\RomFS",
  [string]$Out = "",
  [string]$Python = "",
  [string]$ToolchainBin = ""
)
$Error = $null
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if (-not $Out) { $Out = Join-Path $root "build\win\overlay" }
if (-not $Python) {
  $Python = (Get-Command python -ErrorAction SilentlyContinue).Source
  if (-not $Python) { throw "找不到 python：装一个（3.9+，要 PIL / numpy / scipy），或用 -Python 指定" }
}
if (-not (Test-Path $RomFS)) { throw "RomFS 不存在：$RomFS（用 -RomFS 指定游戏文件里的 RomFS 目录）" }

# python 的位数与架构 → 编哪个目标的转换库 | python bitness and architecture -> which target the conversion library is built for
$arch = & $Python -c "import ctypes,platform; print(ctypes.sizeof(ctypes.c_void_p)*8, platform.machine())"
if ($LASTEXITCODE -ne 0) { throw "python 起不来：$Python" }
"python：$Python（$arch）"

foreach ($m in 'PIL', 'numpy', 'scipy') {
  & $Python -c "import $m" 2>$null
  if ($LASTEXITCODE -ne 0) { throw "python 缺 $m：$Python -m pip install pillow numpy scipy" }
}

$target = if ($arch -match 'ARM64') { 'aarch64-w64-mingw32' } else { 'x86_64-w64-mingw32' }
if (-not $ToolchainBin) {
  $cl = Get-Command "$target-clang" -ErrorAction SilentlyContinue
  if (-not $cl) { $cl = Get-Command clang -ErrorAction SilentlyContinue }
  if (-not $cl) { throw "找不到 clang（llvm-mingw 的 bin 目录），或用 -ToolchainBin 指定；也可以自己编好转换库后用 DM_DESWIZZLE_LIB 指过去" }
  $ToolchainBin = Split-Path $cl.Source
}
$clang = Join-Path $ToolchainBin "clang.exe"
if (-not (Test-Path $clang)) { $clang = Join-Path $ToolchainBin "clang" }
$dll = Join-Path $root "build\win\deswizzle-$target.dll"
New-Item -ItemType Directory -Force (Split-Path $dll) | Out-Null
"编转换库：$target → $dll"
& $clang --target=$target -O2 -shared '-Wl,/EXPORT:nvn_tex_layout' '-Wl,/EXPORT:nvn_deswizzle' '-Wl,/EXPORT:nvn_swizzle' `
  "-I$root/dm/gfx/nvn" "-I$root/dm/gfx/tex" -o $dll `
  "$root/dm/gfx/tex/nvn_deswizzle.c" "$root/dm/gfx/nvn/nvn_tex.c"
if ($LASTEXITCODE -ne 0) { throw "转换库编译失败" }
& $Python -c "import ctypes; ctypes.CDLL(r'$dll'); print('转换库加载正常')" 2>$null
if ($LASTEXITCODE -ne 0) { throw "转换库与 python 架构不匹配（$dll）" }

# 生成：DM_OVERLAY_MAC=1 出键盘键帽（桌面形态用），产物落 $Out | Generate: DM_OVERLAY_MAC=1 adds keyboard key caps (desktop form), output goes to $Out
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$env:DM_ROMFS_SRC = (Resolve-Path $RomFS).Path
$env:DM_OVERLAY_MAC = '1'
$env:DM_DESWIZZLE_LIB = $dll
$log = Join-Path $root "build\win\gen_overlay.log"
Push-Location (Join-Path $root "dm\ios")
try { & $Python "tools\gen_romfs_overlay.py" $env:DM_ROMFS_SRC $Out > $log 2>&1 } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) {
  "生成失败，最后几行："
  Get-Content $log -Tail 15
  throw "gen_romfs_overlay 失败"
}
"完成：$Out"
Get-ChildItem $Out | Select-Object Name, Length
"运行时：set DM_ROMFS_OVERLAY=$Out（start-win-arm64.bat 已自己接上）"
