.text
.globl ui_switch
.globl ui_first_switch
.globl ui_trampoline

# void ui_switch(void **from_rsp, void *to_rsp)
ui_switch:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
    pushq %rbp
    movq  %rsp, (%rdi)
    movq  %rsi, %rsp
    popq  %rbp
    popq  %rbx
    popq  %r12
    popq  %r13
    popq  %r14
    popq  %r15
    retq

# void ui_first_switch(void *to_rsp)
#   rdi = new RSP (goroutine stack, set up by ui_goro_init)
ui_first_switch:
    movq  %rdi, %rsp
    popq  %rbp
    popq  %rbx
    popq  %r12
    popq  %r13
    popq  %r14
    popq  %r15
    retq

# Trampoline for first goroutine entry.
# On entry: rbx = entry function, r12 = arg
# Call entry(arg), then exit.
ui_trampoline:
    movq  %r12, %rdi
    callq *%rbx
    jmp   ui_goro_exit
