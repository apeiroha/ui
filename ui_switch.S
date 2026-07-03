.text
.globl ui_switch
.globl ui_switch_defer
.globl ui_first_switch
.globl ui_trampoline

# void ui_switch(void **from_rsp, void *to_rsp)
#   rdi = pointer to save current RSP into
#   rsi = VALUE of new RSP to load
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

# void ui_switch_defer(void **from_rsp, void *to_rsp,
#                       void (*defer_fn)(void*), void *defer_arg)
#   rdi = pointer to save current RSP into
#   rsi = VALUE of new RSP to load
#   rdx = callback function to call on the NEW stack
#   rcx = argument for callback
# Like ui_switch, but instead of returning to the caller,
# tail-calls defer_fn(defer_arg) on the new stack.
# When defer_fn returns (ret), it returns to whoever called
# ui_switch on the target stack.
ui_switch_defer:
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
    movq  %rcx, %rdi
    jmp   *%rdx

# void ui_first_switch(void *sched_rsp_ptr, void *to_rsp)
#   rdi = pointer to scheduler's RSP save location (v->sched_rsp_ptr)
#   rsi = new RSP to load (g->rsp)
ui_first_switch:
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

# ui_trampoline: calls entry(arg), then ui_goro_exit
# rbx = entry, r12 = arg (set by ui_first_switch / goro init)
ui_trampoline:
    movq  %r12, %rdi
    callq *%rbx
    callq ui_goro_exit
    ud2
