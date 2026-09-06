[bits 64]
[extern panic_handler]
[extern keyboard_callback]
[extern timer_callback]
[extern schedule]
[extern tasks]
[extern linux_syscall_handler]
[extern vmm_page_fault_handler]
[extern syscall_handler]
[extern current_task]
[extern syscall_user_rsp]

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
    sub rsp, 8          ; Align RSP for System V ABI C function call
    call keyboard_callback
    add rsp, 8          ; Restore RSP alignment
    mov al, 0x20
    out 0x20, al        ; Send EOI to Master PIC
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

[global sched_yield_asm]
sched_yield_asm:
    push qword 0          ; Fake error code for System V ABI alignment
    push qword 0x82       ; Vector 0x82 (Yield)
    SAVE_REGS

    mov rdi, rsp          ; Pass current task RSP to schedule()
    call schedule
    mov rsp, rax          ; Switch to target task RSP!

    RESTORE_REGS
    add rsp, 16           ; Pop vector and fake error code
    iretq

[global syscall_interrupt_asm]
syscall_interrupt_asm:
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

    mov rdi, rsp
    sub rsp, 8          ; Выравнивание RSP по 16 байт
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
    iretq

[global linux_syscall_interrupt_asm]
linux_syscall_interrupt_asm:
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

    mov rdi, rsp
    sub rsp, 8          ; Выравнивание RSP по 16 байт
    call linux_syscall_handler
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
    iretq

[global page_fault_asm]
page_fault_asm:
    push qword 14       ; Vector 14
    SAVE_REGS           
    mov rdi, rsp        
    sub rsp, 8          
    call vmm_page_fault_handler
    add rsp, 8
    RESTORE_REGS        
    add rsp, 16         
    iretq

[global syscall_entry_asm]
syscall_entry_asm:
    ; 1. При входе CPU сохраняет User RIP в RCX, User RFLAGS в R11.
    ; Сохраняем User RSP в переменную в секции .data ядра
    mov [rel syscall_user_rsp], rsp

    ; 2. Переключаемся на верхушку ядерного стека текущей задачи
    mov rsp, [rel current_task]
    mov rsp, [rsp + 8]                   ; current_task->kstack_at_bottom

    ; 3. Формируем контекст вызова на стеке ядра
    push qword 0x1B                      ; User SS
    push qword [rel syscall_user_rsp]    ; User RSP
    push r11                             ; User RFLAGS
    push qword 0x23                      ; User CS
    push rcx                             ; User RIP

    ; 4. Сохраняем все регистры общего назначения (GPR)
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

    ; 5. Вызываем диспетчер сисколлов
    mov rdi, rsp                         ; Указатель на структуру регистров syscall_regs_t
    sub rsp, 8                           ; Выравнивание RSP по 16 байт для System V AMD64 ABI
    call syscall_handler
    add rsp, 8

    ; 6. Восстанавливаем регистры
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
    pop rax                              ; RAX содержит результат сисколла

    ; 7. Восстанавливаем контекст пользователя
    pop rcx                              ; User RIP
    add rsp, 8                           ; Пропускаем CS
    pop r11                              ; User RFLAGS (включая IF)
    pop rsp                              ; Восстанавливаем User RSP

    db 0x48, 0x0F, 0x07                  ; sysretq (64-bit SYSRET)

section .data
[global isr_stub_table]
isr_stub_table:
%assign i 0
%rep 32
    dq isr%+i
%assign i i+1
%endrep