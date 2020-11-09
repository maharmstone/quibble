#pragma once

EFI_STATUS info_register(EFI_BOOT_SERVICES* bs);

void print(const WCHAR* s);
void print_error(const WCHAR* func, EFI_STATUS Status);
void print_hex(uint64_t v);
void print_dec(uint32_t v);
void print_string(const char* s);
