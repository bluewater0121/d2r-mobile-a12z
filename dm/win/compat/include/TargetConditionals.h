// Windows：苹果的平台判定头（宿主部分文件无条件包含它）。全部为 0，与安卓 compat 同一写法。
// Windows: Apple's platform-condition header (some host files include it unconditionally). All 0, same as the Android compat.
#pragma once
#define TARGET_OS_MAC 0
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_TV 0
#define TARGET_OS_WATCH 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_EMBEDDED 0
#define TARGET_OS_VISION 0
#define TARGET_CPU_ARM64 1
#define TARGET_CPU_X86_64 0
#define TARGET_CPU_ARM 0
#define TARGET_RT_64_BIT 1
#define TARGET_RT_LITTLE_ENDIAN 1
#define TARGET_RT_BIG_ENDIAN 0
#define TARGET_RT_MAC_CFCAREF 0
