[bits 64]
[extern panic_handler]
[extern keyboard_callback]
[extern mouse_callback]
[extern timer_callback]
[extern schedule]
[extern current_task]
[extern tasks]
[extern schedule]

; Макросы должны быть симметричны!
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

; --- СЕКЦИЯ КОДА ---
section .text

%macro ISR_NOERRCODE 1
[global isr%1]
isr%1:
    push qword 0    ; fake error code
    push qword %1   ; номер прерывания
    jmp exception_common
%endmacro

%macro ISR_ERRCODE 1
[global isr%1]
isr%1:
    ; Код ошибки уже в стеке
    push qword %1
    jmp exception_common
%endmacro

; Генерируем 32 исключения (0-31)
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
    call panic_handler
    ; panic_handler не возвращается

[global keyboard_handler]
keyboard_handler:
    SAVE_REGS
    call keyboard_callback
    mov al, 0x20
    out 0x20, al
    RESTORE_REGS
    iretq 

[global timer_handler]
timer_handler:
    SAVE_REGS
    call timer_callback  ; Просто вызываем функцию
    mov al, 0x20
    out 0x20, al
    RESTORE_REGS
    iretq
    
[global mouse_handler]
mouse_handler:
    SAVE_REGS
    call mouse_callback
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    RESTORE_REGS
    iretq

[global irq0_handler_asm]
irq0_handler_asm:
    ; 1. Процессор при входе уже запушил: SS, RSP, RFLAGS, CS, RIP
    ; 2. Пушим фейковый код ошибки и номер прерывания для симметрии с исключениями
    push qword 0      
    push qword 32     
    
    ; 3. Сохраняем все регистры (15 штук)
    SAVE_REGS         

    ; Стек сейчас: 5 (CPU) + 2 (Fake) + 15 (SAVE_REGS) = 22 квода.
    ; 22 * 8 = 176 байт. 176 делятся на 16. Стек выровнен!

    ; 4. Инкремент системного тика
    call timer_callback  

    ; 5. Подготовка к переключению контекста
    mov rdi, rsp      ; Текущий стек передаем как первый аргумент в schedule()
    call schedule     ; schedule вернет RSP следующей задачи в RAX
    
    ; 6. ПЕРЕКЛЮЧЕНИЕ СТЕКА
    mov rsp, rax      ; Теперь RSP указывает на стек новой задачи

    ; 7. Сигнал контроллеру прерываний (EOI)
    mov al, 0x20
    out 0x20, al

    ; 8. Восстанавливаем регистры НОВОЙ задачи
    RESTORE_REGS      
    
    ; 9. Убираем фейковый код ошибки и номер прерывания
    add rsp, 16       
    
    ; 10. Возвращаемся в код новой задачи
    iretq

[extern syscall_handler]
[global syscall_interrupt_asm]
syscall_interrupt_asm:
    ; ВАЖНО: сохраняем ВЕСЬ набор GP-регистров пользователя.
    ; r10/r11 — caller-saved в SysV C-ABI, поэтому syscall_handler (и всё, что
    ; он вызывает: memcpy, планировщик, отрисовка) может их затереть. Раньше
    ; стаб их НЕ сохранял, и любой код с inline `int 0x80`, который держит
    ; живые значения в r10/r11 через сисколл (например порт SDL2 —
    ; third_party/sdl2 equos_syscall), получал мусор в регистре и падал в
    ; page fault. r12–r15 формально сохраняются вызываемой C-функцией, но
    ; пушим их тоже — так syscall ABI становится «прозрачным» для userspace
    ; и не зависит от того, что именно делает обработчик.
    ; Порядок: r15..r10 пушим ПЕРВЫМИ, чтобы смещения уже существующих
    ; полей в syscall_regs_t (rax..rbp) не изменились.
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
    push rax           ; RAX пушим последним, он будет первым в структуре C

    mov rdi, rsp
    call syscall_handler

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

; --- Этап 6a: Linux-ABI шлюз (int 0x81) ---
; Полное зеркало syscall_interrupt_asm, но вызывает linux_syscall_handler.
; Строит тот же кадр syscall_regs_t (см. uregs.h) — порядок push ОБЯЗАН
; совпадать. musl эмитит `int $0x81` вместо `syscall` (4-й арг в r10, его мы
; тоже сохраняем). `int` не клобберит rcx/r11, поэтому ABI прозрачен.
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
    call linux_syscall_handler
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

; --- СЕКЦИЯ ДАННЫХ ---
section .data
[global isr_stub_table]
isr_stub_table:
%assign i 0
%rep 32
    dq isr%+i
%assign i i+1
%endrep
