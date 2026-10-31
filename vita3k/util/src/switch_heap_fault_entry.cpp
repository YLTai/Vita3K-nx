// A64 exception interposer and register-preservation test.
// SPDX-License-Identifier: GPL-2.0-or-later
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB) && defined(__aarch64__)
asm(R"A64(
.pushsection .text.v3k_heap_fault, "ax", %progbits
.balign 16
.global __wrap___libnx_exception_entry
.type __wrap___libnx_exception_entry, %function
__wrap___libnx_exception_entry:
// Kernel PLR entry stack is small: switch BEFORE saving the SIMD context.
// x0..x8 are already in the kernel-owned frame; only x2/x8 are temporary.
cbnz x1, .Lframe_ready
b __real___libnx_exception_entry
.Lframe_ready:
mov x8, sp
adrp x2, v3k_heap_exception_stack
add x2, x2, :lo12:v3k_heap_exception_stack
add x2, x2, #4, lsl #12
mov sp, x2
sub sp, sp, #800
str x8, [sp, #792]
stp x0, x1, [sp, #0]
ldp x2, x3, [x1, #16]
ldp x4, x5, [x1, #32]
ldp x6, x7, [x1, #48]
ldr x8, [x1, #64]
stp x2, x3, [sp, #16]
stp x4, x5, [sp, #32]
stp x6, x7, [sp, #48]
stp x8, x9, [sp, #64]
stp x10, x11, [sp, #80]
stp x12, x13, [sp, #96]
stp x14, x15, [sp, #112]
stp x16, x17, [sp, #128]
stp x18, x19, [sp, #144]
stp x20, x21, [sp, #160]
stp x22, x23, [sp, #176]
stp x24, x25, [sp, #192]
stp x26, x27, [sp, #208]
stp x28, x29, [sp, #224]
str x30, [sp, #240]
stp q0, q1, [sp, #256]
stp q2, q3, [sp, #288]
stp q4, q5, [sp, #320]
stp q6, q7, [sp, #352]
stp q8, q9, [sp, #384]
stp q10, q11, [sp, #416]
stp q12, q13, [sp, #448]
stp q14, q15, [sp, #480]
stp q16, q17, [sp, #512]
stp q18, q19, [sp, #544]
stp q20, q21, [sp, #576]
stp q22, q23, [sp, #608]
stp q24, q25, [sp, #640]
stp q26, q27, [sp, #672]
stp q28, q29, [sp, #704]
stp q30, q31, [sp, #736]
mrs x2, fpcr
mrs x3, fpsr
str x2, [sp, #768]
str x3, [sp, #776]
mrs x2, nzcv
str x2, [sp, #784]
bl v3k_try_heap_fault
cbz w0, .Lfallback
ldr x2, [sp, #768]
ldr x3, [sp, #776]
msr fpcr, x2
msr fpsr, x3
ldr x2, [sp, #784]
msr nzcv, x2
ldp q0, q1, [sp, #256]
ldp q2, q3, [sp, #288]
ldp q4, q5, [sp, #320]
ldp q6, q7, [sp, #352]
ldp q8, q9, [sp, #384]
ldp q10, q11, [sp, #416]
ldp q12, q13, [sp, #448]
ldp q14, q15, [sp, #480]
ldp q16, q17, [sp, #512]
ldp q18, q19, [sp, #544]
ldp q20, q21, [sp, #576]
ldp q22, q23, [sp, #608]
ldp q24, q25, [sp, #640]
ldp q26, q27, [sp, #672]
ldp q28, q29, [sp, #704]
ldp q30, q31, [sp, #736]
ldp x0, x1, [sp, #0]
ldp x2, x3, [sp, #16]
ldp x4, x5, [sp, #32]
ldp x6, x7, [sp, #48]
ldp x8, x9, [sp, #64]
ldp x10, x11, [sp, #80]
ldp x12, x13, [sp, #96]
ldp x14, x15, [sp, #112]
ldp x16, x17, [sp, #128]
ldp x18, x19, [sp, #144]
ldp x20, x21, [sp, #160]
ldp x22, x23, [sp, #176]
ldp x24, x25, [sp, #192]
ldp x26, x27, [sp, #208]
ldp x28, x29, [sp, #224]
ldr x30, [sp, #240]
ldr x2, [sp, #792]
mov sp, x2
ldr x2, [x1, #16]
mov w0, wzr
svc #0x28
b .
.Lfallback:
ldr x2, [sp, #768]
ldr x3, [sp, #776]
msr fpcr, x2
msr fpsr, x3
ldr x2, [sp, #784]
msr nzcv, x2
ldp q0, q1, [sp, #256]
ldp q2, q3, [sp, #288]
ldp q4, q5, [sp, #320]
ldp q6, q7, [sp, #352]
ldp q8, q9, [sp, #384]
ldp q10, q11, [sp, #416]
ldp q12, q13, [sp, #448]
ldp q14, q15, [sp, #480]
ldp q16, q17, [sp, #512]
ldp q18, q19, [sp, #544]
ldp q20, q21, [sp, #576]
ldp q22, q23, [sp, #608]
ldp q24, q25, [sp, #640]
ldp q26, q27, [sp, #672]
ldp q28, q29, [sp, #704]
ldp q30, q31, [sp, #736]
ldp x0, x1, [sp, #0]
ldp x2, x3, [sp, #16]
ldp x4, x5, [sp, #32]
ldp x6, x7, [sp, #48]
ldp x8, x9, [sp, #64]
ldp x10, x11, [sp, #80]
ldp x12, x13, [sp, #96]
ldp x14, x15, [sp, #112]
ldp x16, x17, [sp, #128]
ldp x18, x19, [sp, #144]
ldp x20, x21, [sp, #160]
ldp x22, x23, [sp, #176]
ldp x24, x25, [sp, #192]
ldp x26, x27, [sp, #208]
ldp x28, x29, [sp, #224]
ldr x30, [sp, #240]
ldr x2, [sp, #792]
mov sp, x2
ldr x2, [x1, #16]
b __real___libnx_exception_entry
.size __wrap___libnx_exception_entry, .-__wrap___libnx_exception_entry
.balign 16
.global v3k_heap_store8
.type v3k_heap_store8, %function
v3k_heap_store8:
.global v3k_heap_store8_pc
v3k_heap_store8_pc:
strb w1, [x0]
ret
.size v3k_heap_store8, .-v3k_heap_store8
.balign 16
.global v3k_heap_store16
.type v3k_heap_store16, %function
v3k_heap_store16:
.global v3k_heap_store16_pc
v3k_heap_store16_pc:
strh w1, [x0]
ret
.size v3k_heap_store16, .-v3k_heap_store16
.balign 16
.global v3k_heap_store32
.type v3k_heap_store32, %function
v3k_heap_store32:
.global v3k_heap_store32_pc
v3k_heap_store32_pc:
str w1, [x0]
ret
.size v3k_heap_store32, .-v3k_heap_store32
.balign 16
.global v3k_heap_store64
.type v3k_heap_store64, %function
v3k_heap_store64:
.global v3k_heap_store64_pc
v3k_heap_store64_pc:
str x1, [x0]
ret
.size v3k_heap_store64, .-v3k_heap_store64
.balign 16
.global v3k_heap_store128
.type v3k_heap_store128, %function
v3k_heap_store128:
dup v0.2d, x1
.global v3k_heap_store128_pc
v3k_heap_store128_pc:
str q0, [x0]
ret
.size v3k_heap_store128, .-v3k_heap_store128
.balign 16
.global v3k_heap_register_test
.type v3k_heap_register_test, %function
v3k_heap_register_test:
sub sp, sp, #800
stp x0, x1, [sp, #0]
stp x2, x3, [sp, #16]
stp x4, x5, [sp, #32]
stp x6, x7, [sp, #48]
stp x8, x9, [sp, #64]
stp x10, x11, [sp, #80]
stp x12, x13, [sp, #96]
stp x14, x15, [sp, #112]
stp x16, x17, [sp, #128]
stp x18, x19, [sp, #144]
stp x20, x21, [sp, #160]
stp x22, x23, [sp, #176]
stp x24, x25, [sp, #192]
stp x26, x27, [sp, #208]
stp x28, x29, [sp, #224]
str x30, [sp, #240]
stp q0, q1, [sp, #256]
stp q2, q3, [sp, #288]
stp q4, q5, [sp, #320]
stp q6, q7, [sp, #352]
stp q8, q9, [sp, #384]
stp q10, q11, [sp, #416]
stp q12, q13, [sp, #448]
stp q14, q15, [sp, #480]
stp q16, q17, [sp, #512]
stp q18, q19, [sp, #544]
stp q20, q21, [sp, #576]
stp q22, q23, [sp, #608]
stp q24, q25, [sp, #640]
stp q26, q27, [sp, #672]
stp q28, q29, [sp, #704]
stp q30, q31, [sp, #736]
mrs x2, fpcr
mrs x3, fpsr
str x2, [sp, #768]
str x3, [sp, #776]
mrs x2, nzcv
str x2, [sp, #784]
mov x2, #20738
mov x3, #20739
mov x4, #20740
mov x5, #20741
mov x6, #20742
mov x7, #20743
mov x8, #20744
mov x9, #20745
mov x10, #20746
mov x11, #20747
mov x12, #20748
mov x13, #20749
mov x14, #20750
mov x15, #20751
mov x16, #20752
mov x17, #20753
mov x18, #20754
mov x19, #20755
mov x20, #20756
mov x21, #20757
mov x22, #20758
mov x23, #20759
mov x24, #20760
mov x25, #20761
mov x26, #20762
mov x27, #20763
mov x28, #20764
mov x29, #20765
mov x30, #20766
movi v0.16b, #96
movi v1.16b, #97
movi v2.16b, #98
movi v3.16b, #99
movi v4.16b, #100
movi v5.16b, #101
movi v6.16b, #102
movi v7.16b, #103
movi v8.16b, #104
movi v9.16b, #105
movi v10.16b, #106
movi v11.16b, #107
movi v12.16b, #108
movi v13.16b, #109
movi v14.16b, #110
movi v15.16b, #111
movi v16.16b, #112
movi v17.16b, #113
movi v18.16b, #114
movi v19.16b, #115
movi v20.16b, #116
movi v21.16b, #117
movi v22.16b, #118
movi v23.16b, #119
movi v24.16b, #120
movi v25.16b, #121
movi v26.16b, #122
movi v27.16b, #123
movi v28.16b, #124
movi v29.16b, #125
movi v30.16b, #126
movi v31.16b, #127
mov x2, #0x400000
msr fpcr, x2
mov x2, #0x10
msr fpsr, x2
mov x2, #0xa0000000
msr nzcv, x2
mov x2, #0x5102
str x0, [x1, #0]
str x1, [x1, #8]
str x2, [x1, #16]
str x3, [x1, #24]
str x4, [x1, #32]
str x5, [x1, #40]
str x6, [x1, #48]
str x7, [x1, #56]
str x8, [x1, #64]
str x9, [x1, #72]
str x10, [x1, #80]
str x11, [x1, #88]
str x12, [x1, #96]
str x13, [x1, #104]
str x14, [x1, #112]
str x15, [x1, #120]
str x16, [x1, #128]
str x17, [x1, #136]
str x18, [x1, #144]
str x19, [x1, #152]
str x20, [x1, #160]
str x21, [x1, #168]
str x22, [x1, #176]
str x23, [x1, #184]
str x24, [x1, #192]
str x25, [x1, #200]
str x26, [x1, #208]
str x27, [x1, #216]
str x28, [x1, #224]
str x29, [x1, #232]
str x30, [x1, #240]
mov x2, sp
str x2, [x1, #248]
mrs x2, nzcv
str x2, [x1, #256]
mrs x2, fpcr
str x2, [x1, #264]
mrs x2, fpsr
str x2, [x1, #272]
mov x2, #0x5102
str q0, [x1, #288]
str q1, [x1, #304]
str q2, [x1, #320]
str q3, [x1, #336]
str q4, [x1, #352]
str q5, [x1, #368]
str q6, [x1, #384]
str q7, [x1, #400]
str q8, [x1, #416]
str q9, [x1, #432]
str q10, [x1, #448]
str q11, [x1, #464]
str q12, [x1, #480]
str q13, [x1, #496]
str q14, [x1, #512]
str q15, [x1, #528]
str q16, [x1, #544]
str q17, [x1, #560]
str q18, [x1, #576]
str q19, [x1, #592]
str q20, [x1, #608]
str q21, [x1, #624]
str q22, [x1, #640]
str q23, [x1, #656]
str q24, [x1, #672]
str q25, [x1, #688]
str q26, [x1, #704]
str q27, [x1, #720]
str q28, [x1, #736]
str q29, [x1, #752]
str q30, [x1, #768]
str q31, [x1, #784]
.global v3k_heap_register_test_pc
v3k_heap_register_test_pc:
str x2, [x0]
str x0, [x1, #800]
str x1, [x1, #808]
str x2, [x1, #816]
str x3, [x1, #824]
str x4, [x1, #832]
str x5, [x1, #840]
str x6, [x1, #848]
str x7, [x1, #856]
str x8, [x1, #864]
str x9, [x1, #872]
str x10, [x1, #880]
str x11, [x1, #888]
str x12, [x1, #896]
str x13, [x1, #904]
str x14, [x1, #912]
str x15, [x1, #920]
str x16, [x1, #928]
str x17, [x1, #936]
str x18, [x1, #944]
str x19, [x1, #952]
str x20, [x1, #960]
str x21, [x1, #968]
str x22, [x1, #976]
str x23, [x1, #984]
str x24, [x1, #992]
str x25, [x1, #1000]
str x26, [x1, #1008]
str x27, [x1, #1016]
str x28, [x1, #1024]
str x29, [x1, #1032]
str x30, [x1, #1040]
mov x2, sp
str x2, [x1, #1048]
mrs x2, nzcv
str x2, [x1, #1056]
mrs x2, fpcr
str x2, [x1, #1064]
mrs x2, fpsr
str x2, [x1, #1072]
mov x2, #0x5102
str q0, [x1, #1088]
str q1, [x1, #1104]
str q2, [x1, #1120]
str q3, [x1, #1136]
str q4, [x1, #1152]
str q5, [x1, #1168]
str q6, [x1, #1184]
str q7, [x1, #1200]
str q8, [x1, #1216]
str q9, [x1, #1232]
str q10, [x1, #1248]
str q11, [x1, #1264]
str q12, [x1, #1280]
str q13, [x1, #1296]
str q14, [x1, #1312]
str q15, [x1, #1328]
str q16, [x1, #1344]
str q17, [x1, #1360]
str q18, [x1, #1376]
str q19, [x1, #1392]
str q20, [x1, #1408]
str q21, [x1, #1424]
str q22, [x1, #1440]
str q23, [x1, #1456]
str q24, [x1, #1472]
str q25, [x1, #1488]
str q26, [x1, #1504]
str q27, [x1, #1520]
str q28, [x1, #1536]
str q29, [x1, #1552]
str q30, [x1, #1568]
str q31, [x1, #1584]
ldr x2, [sp, #768]
ldr x3, [sp, #776]
msr fpcr, x2
msr fpsr, x3
ldr x2, [sp, #784]
msr nzcv, x2
ldp q0, q1, [sp, #256]
ldp q2, q3, [sp, #288]
ldp q4, q5, [sp, #320]
ldp q6, q7, [sp, #352]
ldp q8, q9, [sp, #384]
ldp q10, q11, [sp, #416]
ldp q12, q13, [sp, #448]
ldp q14, q15, [sp, #480]
ldp q16, q17, [sp, #512]
ldp q18, q19, [sp, #544]
ldp q20, q21, [sp, #576]
ldp q22, q23, [sp, #608]
ldp q24, q25, [sp, #640]
ldp q26, q27, [sp, #672]
ldp q28, q29, [sp, #704]
ldp q30, q31, [sp, #736]
ldp x0, x1, [sp, #0]
ldp x2, x3, [sp, #16]
ldp x4, x5, [sp, #32]
ldp x6, x7, [sp, #48]
ldp x8, x9, [sp, #64]
ldp x10, x11, [sp, #80]
ldp x12, x13, [sp, #96]
ldp x14, x15, [sp, #112]
ldp x16, x17, [sp, #128]
ldp x18, x19, [sp, #144]
ldp x20, x21, [sp, #160]
ldp x22, x23, [sp, #176]
ldp x24, x25, [sp, #192]
ldp x26, x27, [sp, #208]
ldp x28, x29, [sp, #224]
ldr x30, [sp, #240]
add sp, sp, #800
ret
.size v3k_heap_register_test, .-v3k_heap_register_test
.popsection
.pushsection .bss.v3k_heap_exception_stack, "aw", %nobits
.balign 16
v3k_heap_exception_stack:
.skip 16384
.popsection
)A64");
#endif
