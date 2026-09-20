# Encoder conformance sample for the in-OS assembler.
# Copy into fs/, rebuild, assemble it with `as` inside the OS, then compare
# against the host:  as --32 -o ref.o tools/as_conformance.s
# Free of labels and symbols so encodings are position independent.
.text
    mov  %eax, %ebx
    mov  %esp, %ebp
    mov  $5, %eax
    mov  $0x12345678, %edi
    mov  (%eax), %ecx
    mov  4(%ebp), %edx
    mov  -8(%ebp), %esi
    mov  %eax, 12(%esp)
    mov  (%esp), %eax
    mov  (%ebp), %eax
    mov  0x1234, %eax
    mov  %eax, 0x1234
    mov  (%eax,%ebx,4), %ecx
    mov  8(%eax,%ebx,2), %ecx
    mov  (,%ebx,8), %ecx
    movl $7, (%eax)
    movl $7, 16(%esp)
    movb $9, (%edx)
    mov  %al, (%ebx)
    mov  (%ebx), %cl
    movzbl (%eax), %edx
    movsbl (%eax), %edx
    movzwl (%eax), %edx
    lea  8(%eax,%ebx,4), %esi
    lea  (%ecx), %eax

    add  %eax, %ebx
    add  (%eax), %ebx
    add  %ebx, (%eax)
    add  $1, %eax
    add  $0x1000, %eax
    addl $1, (%eax)
    or   %eax, %ecx
    adc  %eax, %ecx
    sbb  %eax, %ecx
    and  $0xFF, %edx
    sub  %esi, %edi
    xor  %eax, %eax
    cmp  $0, %esi
    cmp  %eax, %ebx
    cmpb $3, (%eax)

    test %eax, %eax
    test $1, %eax
    xchg %eax, %ebx

    push %ebp
    push $4
    push $0x1000
    pushl (%eax)
    pop  %ebp
    popl (%eax)

    inc  %eax
    dec  %ecx
    incl (%eax)
    not  %eax
    neg  %ebx
    mul  %ecx
    imul %ecx
    imul %ecx, %eax
    div  %ecx
    idiv %ecx
    cltd

    shl  $3, %eax
    shr  $1, %ebx
    sar  $2, %ecx
    shl  %cl, %eax
    sar  %cl, %edx
    rol  $4, %eax

    sete %al
    setne %bl
    setg %cl
    setle (%eax)
    cmove %eax, %ebx
    cmovne (%eax), %ebx

    int  $0x80
    nop
    hlt
    leave
    ret
