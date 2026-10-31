// Wine probe: the pieces that need exact control of the stack and the code segment.

    .text

// long probe_call(long (*fn)(void*), void* arg, uint64_t* saved_rsp)
// Calls fn(arg). A fault inside it is resumed by the signal handler at probe_recover with rsp set
// back to *saved_rsp, which returns from here with whatever rax the handler left.
    .globl probe_call
    .type probe_call, @function
probe_call:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    sub $8, %rsp                // 16-byte aligned for the call
    mov %rsp, (%rdx)
    mov %rdi, %rax
    mov %rsi, %rdi
    call *%rax
    .globl probe_recover
probe_recover:
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

// uint64_t enter32(uint64_t code32, uint64_t stack32_top, uint64_t cs32)
// Jumps to code32 in 32-bit mode with the low stack. That code ends with a far jump to a 64-bit stub
// below 4 GiB, which jumps to probe32_back: rax = (ebx << 32) | eax.
    .globl enter32
    .type enter32, @function
enter32:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, g_enter32_rsp(%rip)
    mov %rsi, %rsp
    push %rdx
    push %rdi
    lretq
    .globl probe32_back
probe32_back:
    mov g_enter32_rsp(%rip), %rsp
    mov %eax, %eax
    mov %ebx, %ebx
    shl $32, %rbx
    or %rbx, %rax
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

    .bss
    .align 8
    .globl g_enter32_rsp
g_enter32_rsp:
    .quad 0

    .section .note.GNU-stack,"",@progbits
