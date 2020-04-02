IFDEF RAX
ELSE
.686P
ENDIF

_TEXT  SEGMENT

IFDEF RAX

; void change_stack2(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle, void* stack_end, change_stack_cb cb);
PUBLIC change_stack2

; rcx = bs
; rdx = image_handle
; r8 = stack_end
; r9 = cb

; FIXME - probably should restore original rbx

change_stack2:
    mov rbx, rsp
    mov rsp, r8
    push rbp
    mov rbp, rsp
    push rbx
    sub rsp, 32
    call r9
    add rsp, 32
    pop rbx
    pop rbp
    mov rsp, rbx
    ret

; void set_gdt2(GDTIDT* desc, uint16_t selector);

PUBLIC set_gdt2

; rcx = desc
; rdx = selector

set_gdt2:
    ; set GDT
    lgdt fword ptr [rcx]

    ; set task register
    ltr dx

    ; change cs to 0x10
    mov rax, 10h
    push rax
    lea rax, [set_gdt2_label]
    push rax
    retf

set_gdt2_label:
    ; change ss to 0x18
    mov ax, 18h
    mov ss, ax

    ret

; void call_startup(void* stack, void* loader_block, void* KiSystemStartup);

PUBLIC call_startup

; rcx = stack
; rdx = loader_block
; r8 = KiSystemStarutp

call_startup:
    mov rsp, rcx
    mov rcx, rdx
    call r8
    ret

ELSE

; FIXME - change_stack2
; change_stack2:
;     __asm__ __volatile__ (
;         "mov eax, %0\n\t"
;         "mov ebx, %1\n\t"
;         "mov ecx, %3\n\t"
;         "mov edx, esp\n\t"
;         "mov esp, %2\n\t"
;         "push ebp\n\t"
;         "mov ebp, esp\n\t"
;         "push edx\n\t"
;         "push ebx\n\t"
;         "push eax\n\t"
;         "call ecx\n\t"
;         "pop edx\n\t"
;         "pop ebp\n\t"
;         "mov esp, edx\n\t"
;         :
;         : "m" (bs), "m" (image_handle), "m" (stack_end), "" (cb)
;         : "eax", "ebx", "ecx", "edx"
;     );

; FIXME - set_gdt2

ENDIF

_TEXT  ENDS

end
