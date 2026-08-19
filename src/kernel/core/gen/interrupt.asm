[bits 64]
[extern panic_handler]
[extern keyboard_callback]
[extern timer_callback]
[extern schedule]
[extern current_task]
[extern tasks]
[extern syscall_handler]
[extern linux_syscall_handler]
[extern vmm_page_fault_handler]

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
    sub rsp, 8          ; Align RSP to 16 bytes boundary for System V ABI
    call panic_handler
    add rsp, 8
.halt_loop:
    cli
    hlt
    jmp .halt_loop

; Обработчик спонтанных IRQ
[global irq_ignore_handler]
irq_ignore_handler:
    push rax
    mov al, 0x20
    out 0x20, al        ; Send EOI to Master PIC
    out 0xA0, al        ; Send EOI to Slave PIC
    pop rax
    iretq

[global keyboard_handler]
keyboard_handler:
    SAVE_REGS
    sub rsp, 8          ; Align RSP to 16-byte boundary
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
    sub rsp, 8          ; Align RSP for C function call
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

[global syscall_entry_asm]
syscall_entry_asm:
    ; Swap GS to CPU-Local Storage (Preparing for Per-CPU architecture)
    ; RCX contains User RIP, R11 contains User RFLAGS saved by CPU during 'syscall'
    
    ; Temporarily save User RSP onto User Stack or Scratch Area safely
    ; We push user RSP to kernel stack AFTER swapping to Kernel RSP
    mov r12, rsp                ; R12 is saved-by-callee register in System V ABI
    
    ; Load Kernel Stack from current_task structure safely
    mov rsp, [rel current_task]
    mov rsp, [rsp + 8]          ; Fetch kstack_at_bottom offset (8)

    ; Push standard interrupt frame on Kernel Stack
    push qword 0x23             ; User SS Selector
    push r12                    ; User RSP
    push r11                    ; User RFLAGS
    push qword 0x1B             ; User CS Selector
    push rcx                    ; User RIP

    ; Save general purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp                ; Pass stack frame pointer as arg1 to syscall_handler
    sub rsp, 8                  ; Align RSP to 16-byte boundary for System V ABI
    call syscall_handler
    add rsp, 8

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Restore stack & register frame for sysretq
    pop rcx                     ; Restore User RIP for sysret
    add rsp, 8                  ; Skip CS selector
    pop r11                     ; Restore User RFLAGS
    pop rsp                     ; Directly restore User RSP from Kernel Stack!

    db 0x48                     ; REX.W prefix for 64-bit sysretq
    sysret

[global page_fault_asm]
page_fault_asm:
    push qword 14       ; Номер прерывания (Vector 14)
    SAVE_REGS           ; Сохраняем все регистры RAX..R15 на стек
    mov rdi, rsp        ; Передаем ЧЕСТНЫЙ указатель на cpu_state_t в RDI для C
    sub rsp, 8          ; Выравниваем стек по 16 байт
    call vmm_page_fault_handler
    add rsp, 8
    RESTORE_REGS        ; Восстанавливаем регистры
    add rsp, 16         ; Сбрасываем номер прерывания и error_code от CPU
    iretq

section .data
[global isr_stub_table]
isr_stub_table:
%assign i 0
%rep 32
    dq isr%+i
%assign i i+1
%endrep