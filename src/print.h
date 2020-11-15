#pragma once

typedef struct {
    unsigned int x;
    unsigned int y;
} text_pos;

EFI_STATUS info_register(EFI_BOOT_SERVICES* bs);

void print_error(const char* func, EFI_STATUS Status);
void print_string(const char* s);
void draw_text_ft(const char* s, text_pos* p, uint32_t bg_colour, uint32_t fg_colour);
void init_gop_console();
EFI_STATUS load_font();

extern bool gop_console;
