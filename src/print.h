#pragma once

typedef struct {
    unsigned int x;
    unsigned int y;
} text_pos;

EFI_STATUS info_register(EFI_BOOT_SERVICES* bs);

void print_error(const WCHAR* func, EFI_STATUS Status);
void print_string(const char* s);
void draw_text(const char* s, text_pos* p);
