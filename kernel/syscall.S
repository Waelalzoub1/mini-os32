.code32
.globl syscall_entry
.globl enter_user
.extern syscall_dispatch
.extern exit_requested
.extern thread_exit_pending
.extern thread_exit_switch
.extern kernel_stack_top
.extern after_user

syscall_entry:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push %esp
    call syscall_dispatch
    add $4, %esp
    mov %eax, 44(%esp)
    cmpl $0, thread_exit_pending
    je .check_exit
    mov %esp, %edx
    lea 48(%edx), %eax
    push %eax
    push %edx
    call thread_exit_switch
    add $8, %esp
    test %eax, %eax
    jne .exit
.check_exit:
    cmpl $0, exit_requested
    jne .exit
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    iret
.exit:
    mov $kernel_stack_top, %esp
    jmp after_user

enter_user:
    mov 4(%esp), %eax
    mov 8(%esp), %ebx
    mov $0x23, %cx
    mov %cx, %ds
    mov %cx, %es
    mov %cx, %fs
    mov %cx, %gs
    pushl $0x23
    pushl %ebx
    pushl $0x202
    pushl $0x1B
    pushl %eax
    iret
