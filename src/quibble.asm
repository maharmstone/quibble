IFDEF RAX
ELSE
.686P
ENDIF

_TEXT  SEGMENT

IFDEF RAX

; void __stdcall change_stack2(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle, void* stack_end, change_stack_cb cb);
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

; void __stdcall set_gdt2(GDTIDT* desc);
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
    DB 48h ; rex.W - masm doesn't seem to allow you setting this normally
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

; void __stdcall change_stack2(EFI_BOOT_SERVICES* bs, EFI_HANDLE image_handle, void* stack_end, change_stack_cb cb);
PUBLIC _change_stack2@16

; eax = bs
; ebx = image_handle
; ecx = cb
; esi = stack_end (FIXME - ought to preserve)

; FIXME - this probably doesn't fail gracefully

_change_stack2@16:
    mov ecx, [esp+10h]
    mov esi, [esp+0ch]
    mov ebx, [esp+8h]
    mov eax, [esp+4h]
    mov edx, esp
    mov esp, esi
    push ebp
    mov ebp, esp
    push edx
    push ebx
    push eax
    call ecx
    pop edx
    pop ebp
    mov esp, edx
    ret

; void __stdcall set_gdt2(GDTIDT* desc, uint16_t selector);
PUBLIC _set_gdt2@8

; ecx = desc
; edx = selector

_set_gdt2@8:
    mov edx, [esp+8h]
    mov ecx, [esp+4h]

    ; set GDT
    lgdt fword ptr [ecx]

    ; set task register
    ltr dx

    ; change cs to 0x8
    ;jump far 8:set_gdt2_label
    DB 0EAh
    DD [set_gdt2_label]
    DW 8h

set_gdt2_label:
    ret 8

ENDIF

_TEXT  ENDS

end
