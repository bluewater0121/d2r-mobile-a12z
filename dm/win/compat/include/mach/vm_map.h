// vm_map.h（苹果）：只在 iOS 的内嵌装载路径用到（vm_remap）。Windows 走 loader.c 的「文件装载」分支（mmap / mprotect），
// vm_remap 只为头文件完整；万一被调到会明确失败。
// vm_map.h (Apple): only used by the iOS embedded loading path (vm_remap). Windows takes loader.c's file-loading branch (mmap / mprotect);
// vm_remap only exists for include completeness and fails explicitly if it is ever called.
#pragma once
#include "mach/mach.h"
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
typedef int vm_inherit_t;
static inline kern_return_t vm_remap(mach_port_t target_task, vm_address_t *address, vm_size_t size, vm_address_t mask, int flags,
                                     mach_port_t src_task, vm_address_t src_address, int copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh) {
    (void)target_task; (void)address; (void)size; (void)mask; (void)flags; (void)src_task; (void)src_address; (void)copy; (void)cur; (void)max; (void)inh;
    return KERN_FAILURE;
}
