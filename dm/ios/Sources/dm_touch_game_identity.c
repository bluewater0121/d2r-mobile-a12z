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
    /* 页缓存：观测门每秒进三千次，每次都要读客体的栈帧（3 个字）；若每次都 VirtualQuery，光这一步就能把 60 帧啃成 40 帧。
       所以按页记住「这段已提交可读」和区域末尾，命中就只剩一次 memcpy；**读不出来的页也照记**（实体里大半 fp 都指向没提交/带守卫的内存，
       那批要是每次重查，等于什么都没省）。条目 200 ms 过期——提交状态或保护属性变了不会照旧读。
       Page cache: the gate is entered 3k times a second and each entry reads the guest's stack frame (3 words); a VirtualQuery every time is enough to
       eat 60 fps down to 40. So remember per page that "this span is committed and readable" plus the region end, and a hit costs only the memcpy.
       Unreadable pages are remembered too (in the guest most fp values point at uncommitted / guarded memory, and re-checking that batch every call would
       save nothing at all). Entries expire after 200 ms so a changed commit state or protection is never read on stale information. */
    static struct {uint64_t page,end,seen;int ok;} vc[4096];
    unsigned char *dst=(unsigned char *)out;
    uint64_t at=address,left=bytes,now=(uint64_t)GetTickCount64();
    while(left) {
        uint64_t pg=at>>12; unsigned h=(unsigned)((pg^(pg>>12))&4095);
        if(vc[h].page==pg && now-vc[h].seen<200) {
            if(!vc[h].ok)return 0;
            uint64_t n=vc[h].end-at; if(n>left)n=left;
            memcpy(dst,(const void *)(uintptr_t)at,(size_t)n); dst+=n; at+=n; left-=n; continue; }
        MEMORY_BASIC_INFORMATION mbi;
        if(!VirtualQuery((LPCVOID)(uintptr_t)at,&mbi,sizeof mbi))return 0;
        if(mbi.State!=MEM_COMMIT) {vc[h].page=pg;vc[h].ok=0;vc[h].seen=now;return 0;}
        DWORD prot=mbi.Protect & 0xff;
        if(!prot || prot==PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) {vc[h].page=pg;vc[h].ok=0;vc[h].seen=now;return 0;}
        uint64_t base=(uint64_t)(uintptr_t)mbi.BaseAddress, end=base+(uint64_t)mbi.RegionSize;
        if(at>=end)return 0;
        uint64_t n=end-at; if(n>left)n=left;
        memcpy(dst,(const void *)(uintptr_t)at,(size_t)n);
        dst+=n; at+=n; left-=n;
        vc[h].page=pg; vc[h].end=end; vc[h].ok=1; vc[h].seen=now;
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
/* 负缓存（按 fp）：fp 就是「这次是从哪个栈帧 bl 的」，同一帧里存着的 savedLR 也是同一个，所以「这一帧不是 HUD updater」是结构性结论，
   记下来就不用再读栈。Windows 上这类调用占九成（DM_BINDING_DIAG 原因 4：18 万次对 1.4 万次真调用），这些栈读足够把 56 帧啃掉 4 帧。
   只在 Windows 开：其它平台本来就是 vm_read，语义不变；200 ms 过期，换角色/换面板改了栈帧布局就自然重学。
   Negative cache keyed on fp: fp says "which stack frame made this bl", the savedLR stored there is the same one every time, so "this frame is not the
   HUD updater" is a structural verdict: remember it and skip the stack read. On Windows these are ~90% of the calls (DM_BINDING_DIAG reason 4: 180k
   against 14k real ones) and the stack reads alone cost 4 of 56 fps. Enabled on Windows only (elsewhere it is a vm_read anyway, so semantics are
   unchanged); expires after 200 ms so a changed frame layout (new character / panel) is simply relearned. */
#if defined(DM_WIN) || defined(_WIN32)
#define DM_TG_NEG 1
static struct {uint64_t fp,seen;} g_neg[1024];
static uint64_t g_neg_now(void) { return (uint64_t)GetTickCount64(); }
int dm_tg_neg_hits, dm_tg_neg_marks;
#else
#define DM_TG_NEG 0
#endif
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
    if(fp<8 || fp>UINT64_MAX-16)return DM_TG_IDENTITY_UNCONFIRMED;
    /* 负缓存（按 fp）：fp 就是「这次是从哪个栈帧 bl 的」，同一个 fp 里存着的 savedLR 也是同一个，所以「这一帧不是 HUD updater」是结构性结论，
       记下来就不用再读栈。Windows 上这类调用占九成（DM_BINDING_DIAG 的原因 4：18 万次对 1.4 万次真调用），一次栈读足够把 56 帧啃掉 4 帧。
       200 ms 过期，栈帧布局变了（换角色/换面板）自然重学。
       Negative cache keyed on fp: fp says "which stack frame made this bl", and the savedLR stored there is the same one every time, so "this frame is not
       the HUD updater" is a structural verdict: remember it and skip the stack read. On Windows these are ~90% of the calls (DM_BINDING_DIAG reason 4:
       180k against 14k real ones) and the stack reads alone cost 4 of 56 fps. Expires after 200 ms, so a changed frame layout (new character / panel)
       is simply relearned. */
#if DM_TG_NEG
    { unsigned h=(unsigned)((fp>>3)&1023);
      if(g_neg[h].fp==fp && g_neg_now()-g_neg[h].seen<200) {dm_tg_neg_hits++;dm_tg_identity_reason=4;return DM_TG_IDENTITY_UNCONFIRMED;} }
#endif
    if(!read_fn(fp-8,frame,sizeof frame))return DM_TG_IDENTITY_UNCONFIRMED;
    widget=frame[0];lr=frame[2];dm_tg_identity_lr=lr-base;
    if(lr!=base+0x3e2f74 || !widget || widget>UINT64_MAX-0xc90) {
        /* 只把「LR 不符」记进负缓存；widget 读成 0 可能只是这一拍的时机问题，下次还该再读。 | Only an LR mismatch goes into the negative cache; widget read as 0 may just be this tick's timing, so read again next time. */
#if DM_TG_NEG
        if(lr!=base+0x3e2f74) { unsigned h=(unsigned)((fp>>3)&1023); g_neg[h].fp=fp; g_neg[h].seen=g_neg_now(); dm_tg_neg_marks++; }
#endif
        return DM_TG_IDENTITY_UNCONFIRMED;
    }
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
