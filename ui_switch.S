.text
.globl ui_switch
.globl ui_first_switch
.globl ui_trampoline

# void ui_switch(void **from_rsp, void *to_rsp)
#   rdi = pointer to save current RSP into
#   rsi = VALUE of new RSP to load
ui_switch:
    pushq %rbp
    movq  %rsp, (%rdi)
    movq  %rsi, %rsp
    popq  %rbp
    retq

# void ui_first_switch(void *sched_rsp_ptr, void *to_rsp)
#   rdi = pointer to scheduler's RSP save location (v->sched_rsp_ptr)
#   rsi = new RSP to load (g->rsp)
# Like ui_switch, but for the initial goroutine entry.
# The goroutine's stack has 6 callee-saved regs + ret addr.
ui_first_switch:
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

# ui_trampoline: calls goro->entry(goro->arg)
# Then jumps to ui_goro_exit.
# rbx = entry, r12 = arg (set up by ui_first_switch / goro init)
ui_trampoline:
    movq  %r12, %rdi
    callq *%rbx
    jmp   ui_goro_exit
