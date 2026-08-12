[bits 64]
[extern panic_handler]
[extern keyboard_callback]
[extern timer_callback]
[extern schedule]
[extern current_task]
[extern tasks]

%macro SAVE_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_REGS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

section .text

%macro ISR_NOERRCODE 1
[global isr%1]
isr%1:
    push qword 0    ; fake error code
    push qword %1   ; interrupt number
    jmp exception_common
%endmacro

%macro ISR_ERRCODE 1
[global isr%1]
isr%1:
    push qword %1
    jmp exception_common
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8   ; Double Fault
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_ERRCODE   29
ISR_ERRCODE   30
ISR_NOERRCODE 31

exception_common:
    SAVE_REGS
    mov rdi, rsp
    sub rsp, 8          ; FIX: Align RSP to 16 bytes boundary for System V ABI
    call panic_handler
    add rsp, 8
.halt_loop:
    cli
    hlt
    jmp .halt_loop

[global keyboard_handler]
keyboard_handler:
    SAVE_REGS
    sub rsp, 8          ; FIX: Align RSP to 16-byte boundary
    call keyboard_callback
    add rsp, 8
    mov al, 0x20
    out 0x20, al
    RESTORE_REGS
    iretq 

[global irq0_handler_asm]
irq0_handler_asm:
    push qword 0      
    push qword 32     
    SAVE_REGS         

    sub rsp, 8
    call timer_callback  
    add rsp, 8

    mov rdi, rsp      
    call schedule     
    
    mov rsp, rax      

    mov al, 0x20
    out 0x20, al

    RESTORE_REGS      
    add rsp, 16       
    iretq

[extern syscall_handler]
[global syscall_interrupt_asm]
syscall_interrupt_asm:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push r8
    push r9
    push rax

    mov rdi, rsp
    sub rsp, 8          ; FIX: Align RSP for C function call
    call syscall_handler
    add rsp, 8

    pop rax
    pop r9
    pop r8
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    iretq

[extern linux_syscall_handler]
[global linux_syscall_interrupt_asm]
linux_syscall_interrupt_asm:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push r8
    push r9
    push rax

    mov rdi, rsp
    sub rsp, 8
    call linux_syscall_handler
    add rsp, 8

    pop rax
    pop r9
    pop r8
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    iretq

section .data
[global isr_stub_table]
isr_stub_table:
%assign i 0
%rep 32
    dq isr%+i
%assign i i+1
%endrep