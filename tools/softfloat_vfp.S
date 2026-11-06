@ VFP versions of libgcc's soft-float helpers (see src/runtime/softfloat_accel.h).
@ s0-s5 / d0-d2 are caller-saved under AAPCS, so using them is safe. Regenerate: python tools/gen_softfloat_vfp.py
        .syntax unified
        .arm
        .fpu vfpv3-d16
        .text

        .macro FBIN name, op, a=s0, b=s1
        .global vfp_\name
vfp_\name:
        vmov    s0, r0
        vmov    s1, r1
        \op     s0, \a, \b
        vmov    r0, s0
        bx      lr
        .endm

        .macro DBIN name, op, a=d0, b=d1
        .global vfp_\name
vfp_\name:
        vmov    d0, r0, r1
        vmov    d1, r2, r3
        \op     d0, \a, \b
        vmov    r0, r1, d0
        bx      lr
        .endm

        FBIN fadd, vadd.f32
        FBIN fsub, vsub.f32
        FBIN frsub, vsub.f32, s1, s0
        FBIN fmul, vmul.f32
        FBIN fdiv, vdiv.f32
        DBIN dadd, vadd.f64
        DBIN dsub, vsub.f64
        DBIN drsub, vsub.f64, d1, d0
        DBIN dmul, vmul.f64
        DBIN ddiv, vdiv.f64

@ ---- conversions (float -> int truncates toward zero and saturates, NaN -> 0, like libgcc) ----
        .global vfp_f2d
vfp_f2d:
        vmov    s0, r0
        vcvt.f64.f32 d1, s0
        vmov    r0, r1, d1
        bx      lr
        .global vfp_d2f
vfp_d2f:
        vmov    d0, r0, r1
        vcvt.f32.f64 s4, d0
        vmov    r0, s4
        bx      lr
        .global vfp_i2f
vfp_i2f:
        vmov    s0, r0
        vcvt.f32.s32 s0, s0
        vmov    r0, s0
        bx      lr
        .global vfp_ui2f
vfp_ui2f:
        vmov    s0, r0
        vcvt.f32.u32 s0, s0
        vmov    r0, s0
        bx      lr
        .global vfp_i2d
vfp_i2d:
        vmov    s0, r0
        vcvt.f64.s32 d1, s0
        vmov    r0, r1, d1
        bx      lr
        .global vfp_ui2d
vfp_ui2d:
        vmov    s0, r0
        vcvt.f64.u32 d1, s0
        vmov    r0, r1, d1
        bx      lr
        .global vfp_f2iz
vfp_f2iz:
        vmov    s0, r0
        vcvt.s32.f32 s0, s0
        vmov    r0, s0
        bx      lr
        .global vfp_f2uiz
vfp_f2uiz:
        vmov    s0, r0
        vcvt.u32.f32 s0, s0
        vmov    r0, s0
        bx      lr
        .global vfp_d2iz
vfp_d2iz:
        vmov    d0, r0, r1
        vcvt.s32.f64 s4, d0
        vmov    r0, s4
        bx      lr
        .global vfp_d2uiz
vfp_d2uiz:
        vmov    d0, r0, r1
        vcvt.u32.f64 s4, d0
        vmov    r0, s4
        bx      lr

@ ---- __aeabi_[fd]cmp{eq,lt,le,ge,gt,un}: 1 if the relation holds, else 0 (unordered: 0, except cmpun) ----
        .macro FCMP name, cond
        .global vfp_fcmp\name
vfp_fcmp\name:
        vmov    s0, r0
        vmov    s1, r1
        vcmp.f32 s0, s1
        vmrs    APSR_nzcv, fpscr
        mov     r0, #0
        mov\cond r0, #1
        bx      lr
        .endm
        .macro DCMP name, cond
        .global vfp_dcmp\name
vfp_dcmp\name:
        vmov    d0, r0, r1
        vmov    d1, r2, r3
        vcmp.f64 d0, d1
        vmrs    APSR_nzcv, fpscr
        mov     r0, #0
        mov\cond r0, #1
        bx      lr
        .endm
        @ after vcmp: less N=1; equal Z=1,C=1; greater C=1; unordered C=1,V=1. mi/ls/ge/gt are false when unordered.
        FCMP eq, eq
        FCMP lt, mi
        FCMP le, ls
        FCMP ge, ge
        FCMP gt, gt
        FCMP un, vs
        DCMP eq, eq
        DCMP lt, mi
        DCMP le, ls
        DCMP ge, ge
        DCMP gt, gt
        DCMP un, vs

@ ---- __aeabi_c[fd]cmple / cmpeq / rcmple: result in the flags (Z: equal, C clear: less), core registers kept ----
        .global vfp_cfcmple
vfp_cfcmple:
        vmov    s0, r0
        vmov    s1, r1
        vcmp.f32 s0, s1
        vmrs    APSR_nzcv, fpscr
        bx      lr
        .global vfp_cfrcmple
vfp_cfrcmple:
        vmov    s0, r1
        vmov    s1, r0
        vcmp.f32 s0, s1
        vmrs    APSR_nzcv, fpscr
        bx      lr
        .global vfp_cdcmple
vfp_cdcmple:
        vmov    d0, r0, r1
        vmov    d1, r2, r3
        vcmp.f64 d0, d1
        vmrs    APSR_nzcv, fpscr
        bx      lr
        .global vfp_cdrcmple
vfp_cdrcmple:
        vmov    d0, r2, r3
        vmov    d1, r0, r1
        vcmp.f64 d0, d1
        vmrs    APSR_nzcv, fpscr
        bx      lr

@ ---- gcc's __{ge,gt}[sd]f2: -1 less or unordered, 0 equal, 1 greater
@      gcc's __{le,lt,eq,ne}[sd]f2: -1 less, 0 equal, 1 greater or unordered ----
        .global vfp_gesf2
vfp_gesf2:
        vmov    s0, r0
        vmov    s1, r1
        vcmp.f32 s0, s1
        vmrs    APSR_nzcv, fpscr
        mov     r0, #0
        movgt   r0, #1
        mvnmi   r0, #0
        mvnvs   r0, #0
        bx      lr
        .global vfp_lesf2
vfp_lesf2:
        vmov    s0, r0
        vmov    s1, r1
        vcmp.f32 s0, s1
        vmrs    APSR_nzcv, fpscr
        mov     r0, #0
        movgt   r0, #1
        mvnmi   r0, #0
        movvs   r0, #1
        bx      lr
        .global vfp_gedf2
vfp_gedf2:
        vmov    d0, r0, r1
        vmov    d1, r2, r3
        vcmp.f64 d0, d1
        vmrs    APSR_nzcv, fpscr
        mov     r0, #0
        movgt   r0, #1
        mvnmi   r0, #0
        mvnvs   r0, #0
        bx      lr
        .global vfp_ledf2
vfp_ledf2:
        vmov    d0, r0, r1
        vmov    d1, r2, r3
        vcmp.f64 d0, d1
        vmrs    APSR_nzcv, fpscr
        mov     r0, #0
        movgt   r0, #1
        mvnmi   r0, #0
        movvs   r0, #1
        bx      lr
