[bits 64]
global gdt_flush

gdt_flush:
    lgdt [rdi]          ; Загружаем GDT
    mov ax, 0x10        ; Селектор данных ядра
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Трюк для загрузки CS в 64-битном режиме
    push 0x08           ; Сегмент кода
    lea rax, [rel .next]
    push rax            ; Адрес возврата
    db 0x48, 0xCB       ; Это инструкция RETFQ (64-битный far return)
.next:
    ret