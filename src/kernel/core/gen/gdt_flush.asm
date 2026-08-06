[bits 64]
global gdt_flush

gdt_flush:
    lgdt [rdi]          ; Load GDT pointer passed via RDI (System V AMD64 ABI)
    mov ax, 0x10        ; Kernel data segment selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Trick to reload CS register in 64-bit mode using far return
    push 0x08           ; Kernel code segment selector
    lea rax, [rel .next]
    push rax            ; Return address
    db 0x48, 0xCB       ; RETFQ instruction machine code (64-bit far return)
.next:
    ret