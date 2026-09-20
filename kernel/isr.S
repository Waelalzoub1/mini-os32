.code32
.globl isr_stub_table
.globl irq_stub_table
.globl irq48
.globl isr_common
.globl irq_common
.globl fault_handler
.globl irq_handler

.macro ISR_NOERR n
.globl isr\n
isr\n:
    pushl $0
    pushl $\n
    jmp isr_common
.endm

.macro ISR_ERR n
.globl isr\n
isr\n:
    pushl $\n
    jmp isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

isr_common:
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
    call fault_handler
    add $4, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp
    iret

.macro IRQ n
.globl irq\n
irq\n:
    pushl $0
    pushl $\n
    jmp irq_common
.endm

IRQ 32
IRQ 33
IRQ 34
IRQ 35
IRQ 36
IRQ 37
IRQ 38
IRQ 39
IRQ 40
IRQ 41
IRQ 42
IRQ 43
IRQ 44
IRQ 45
IRQ 46
IRQ 47

/* Local APIC timer.  Its own vector rather than sharing 0x20 with the PIT,
 * because it must be acknowledged at the LAPIC and never at the 8259 -- an
 * EOI to the PIC here could prematurely end an in-service keyboard IRQ. */
IRQ 48

irq_common:
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
    call irq_handler
    add $4, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp
    iret

isr_stub_table:
    .long isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .long isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .long isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .long isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

irq_stub_table:
    .long irq32, irq33, irq34, irq35, irq36, irq37, irq38, irq39
    .long irq40, irq41, irq42, irq43, irq44, irq45, irq46, irq47
