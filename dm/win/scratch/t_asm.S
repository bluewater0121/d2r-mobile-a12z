// probe: minimal copy of dm_gate conventions to test COFF assembly
    .text
    .globl probe_stub
    .p2align 3
probe_stub:
    .set k, 0
    .rept 4
    movz x9, #k
    b probe_entry
    .set k, k + 1
    .endr

    .globl probe_entry
    .p2align 2
probe_entry:
    sub sp, sp, #800
    str x30, [sp, #240]
    adrp x9, probe_global
    add x9, x9, :lo12:probe_global
    ldr x9, [x9]
    mov x0, x30
    mov x1, sp
    bl probe_dispatch
    ldr x30, [sp, #240]
    add sp, sp, #800
    ret

    .globl probe_global
    .data
probe_global:
    .xword 0
