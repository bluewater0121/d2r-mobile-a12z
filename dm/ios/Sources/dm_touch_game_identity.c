#include "dm_touch_game_identity.h"
#include <string.h>
#if defined(__APPLE__) || defined(__ANDROID__) || defined(__OHOS__)
#include <mach/mach.h>
#ifdef __APPLE__
#include <mach/vm_map.h>
#endif
#endif
#if defined(DM_WIN) || defined(_WIN32)
#include <windows.h>
#endif
int dm_tg_vm_read(uint64_t address,void *out,size_t bytes) {
    if(!out || !bytes || !address || address>UINT64_MAX-bytes)return 0;
#if defined(__APPLE__) || defined(__ANDROID__) || defined(__OHOS__)
    vm_size_t copied=0;
    kern_return_t rc=vm_read_overwrite(mach_task_self(),(vm_address_t)address,(vm_size_t)bytes,(vm_address_t)out,&copied);
    return rc==KERN_SUCCESS && copied==bytes;
#elif defined(DM_WIN) || defined(_WIN32)
    /* Windows：客体代码和宿主在同一个进程地址里，直接读；先按 VirtualQuery 查这一段是不是可读的已提交内存（跨区域就逐段走）。
       读坏指针 / 未提交页 / PAGE_GUARD 一律当读失败返回 0，与 Mach 版的返回语义一致。
       Windows: the guest runs in the same process address space, so read directly; check with VirtualQuery that this span is committed and
       readable first, walking segment by segment when it crosses regions. A bad pointer, an uncommitted page or PAGE_GUARD all count as a
       failed read (returns 0), the same semantics as the Mach version. */
    unsigned char *dst=(unsigned char *)out;
    uint64_t at=address,left=bytes;
    while(left) {
        MEMORY_BASIC_INFORMATION mbi;
        if(!VirtualQuery((LPCVOID)(uintptr_t)at,&mbi,sizeof mbi))return 0;
        if(mbi.State!=MEM_COMMIT)return 0;
        DWORD prot=mbi.Protect & 0xff;
        if(!prot || prot==PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD))return 0;
        uint64_t base=(uint64_t)(uintptr_t)mbi.BaseAddress, end=base+(uint64_t)mbi.RegionSize;
        if(at>=end)return 0;
        uint64_t n=end-at; if(n>left)n=left;
        memcpy(dst,(const void *)(uintptr_t)at,(size_t)n);
        dst+=n; at+=n; left-=n;
    }
    return 1;
#else
    (void)address;return 0;
#endif
}
/* 诊断（DM_BINDING_DIAG）：最近一次的返回原因码与读到的 LR / 槽号，供 bridge 计数（v4 后快照全空，查卡在哪个返回点） | Diagnostic (DM_BINDING_DIAG): the most recent return reason code and the LR / slot number read, for bridge to count (after v4 the snapshot is entirely empty; find which return point it is stuck at) */
int dm_tg_identity_reason; uint64_t dm_tg_identity_lr; uint32_t dm_tg_identity_slot;
void dm_tg_hud_reset(dm_tg_hud_cohort *c,uint64_t world) {memset(c,0,sizeof(*c));c->world_epoch=world;}
static uint64_t add(uint64_t h,uint64_t v) {
    for(int i=0;i<8;i++){h^=(unsigned char)(v>>(i*8));h*=1099511628211ull;}return h;
}
int dm_tg_hud_identity(dm_tg_hud_cohort *c,const uint64_t *regs,uint64_t base,int game,uint64_t world,
                       dm_tg_read_fn read_fn,uint64_t *caller,uint64_t *hud) {
    dm_tg_identity_reason=1;
    if(!caller || !hud)return DM_TG_IDENTITY_UNCONFIRMED;
    *caller=*hud=0;
    if(!c || !regs || !read_fn)return DM_TG_IDENTITY_UNCONFIRMED;
    if(c->world_epoch!=world)dm_tg_hud_reset(c,world);
    uint32_t arg=(uint32_t)regs[19];
    dm_tg_identity_reason=3;
    if(!game || !world || arg>=12 || base>UINT64_MAX-0x3e2f74)return DM_TG_IDENTITY_UNCONFIRMED;
    dm_tg_identity_reason=4;
    /* 原Get栈帧一次读完：fp-8=调用者x19（widget）、fp=上一帧fp、fp+8=savedLR；三者同在原Get序言刚写下的0x30字节帧里。 | Read the original Get's stack frame in one go: fp-8 = caller's x19 (widget), fp = previous frame's fp, fp+8 = savedLR; all three are in the 0x30-byte frame the original Get's prologue has just written. */
    uint64_t fp=regs[29],frame[3]={0},lr=0,widget=0;
    if(fp<8 || fp>UINT64_MAX-16 || !read_fn(fp-8,frame,sizeof frame))return DM_TG_IDENTITY_UNCONFIRMED;
    widget=frame[0];lr=frame[2];dm_tg_identity_lr=lr-base;
    if(lr!=base+0x3e2f74 || !widget || widget>UINT64_MAX-0xc90)return DM_TG_IDENTITY_UNCONFIRMED;
    /* widget的enum（+b18）与alwaysGetAlt（+c90）同一对象内，一次读[+b18,+c91)。 | The widget's enum (+b18) and alwaysGetAlt (+c90) are inside the same object; read [+b18,+c91) in one go. */
    uint32_t slot=UINT32_MAX;unsigned char always_alt=255;static unsigned char span[0xc91-0xb18]; /* 调用方（bridge 的门回调）持锁；不占客体线程的栈 | The caller (bridge's gate callback) holds the lock; does not use the guest thread's stack */ dm_tg_identity_reason=5;
    if(read_fn(widget+0xb18,span,sizeof span)){memcpy(&slot,span,4);always_alt=span[0xc90-0xb18];}
    if(slot>=6 || always_alt ||
       (arg!=slot && arg!=slot+6)) { dm_tg_identity_slot=slot; return DM_TG_IDENTITY_UNCONFIRMED; }
    dm_tg_identity_reason=6; dm_tg_identity_slot=slot;
    if(c->widget[slot] && c->widget[slot]!=widget)return DM_TG_IDENTITY_UNCONFIRMED;
    for(unsigned i=0;i<6;i++)if(i!=slot && c->widget[i]==widget)return DM_TG_IDENTITY_UNCONFIRMED;
    if(c->confirmed){*caller=lr;*hud=c->hud_identity;dm_tg_identity_reason=0;return DM_TG_IDENTITY_NATIVE;}
    dm_tg_identity_reason=7;   /* 学习中：六槽未齐 | Learning: the six slots are not all filled yet */
    c->widget[slot]=widget;c->seen|=1u<<slot;
    if(c->seen==63) {
        uint64_t h=add(1469598103934665603ull,world);for(int i=0;i<6;i++)h=add(h,c->widget[i]);
        c->hud_identity=h?h:1;c->confirmed=1;
    }
    /* 首组六体只确认cohort，不把第六体借给反馈组；下一个原frame完整六组才ready。 | The first group of six only confirms the cohort and does not lend the sixth one to the feedback group; ready only once the next original frame has the complete six groups. */
    return DM_TG_IDENTITY_UNCONFIRMED;
}
