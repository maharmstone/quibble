#include <string.h>
#include "quibble.h"
#include "quibbleproto.h"
#include "print.h"
#include "misc.h"
#include "font8x8_basic.h"

static EFI_HANDLE info_handle = NULL;
static EFI_QUIBBLE_INFO_PROTOCOL info_proto;
static text_pos console_pos;

extern bool have_csm;
extern void* framebuffer;
extern EFI_GRAPHICS_OUTPUT_MODE_INFORMATION gop_info;
unsigned int console_width, console_height;

EFI_STATUS info_register(EFI_BOOT_SERVICES* bs) {
    EFI_GUID info_guid = EFI_QUIBBLE_INFO_PROTOCOL_GUID;

    info_proto.Print = print_string;

    return bs->InstallProtocolInterface(&info_handle, &info_guid, EFI_NATIVE_INTERFACE, &info_proto);
}

static void move_up_console() {
    uint32_t* src;
    uint32_t* dest;
    unsigned int delta = 8;

    src = (uint32_t*)framebuffer + (gop_info.PixelsPerScanLine * 8);
    dest = (uint32_t*)framebuffer;

    for (unsigned int y = 0; y < gop_info.VerticalResolution - delta; y++) {
        for (unsigned int x = 0; x < gop_info.HorizontalResolution; x++) {
            dest[x] = src[x];
        }

        src += gop_info.PixelsPerScanLine;
        dest += gop_info.PixelsPerScanLine;
    }

    for (unsigned int y = gop_info.VerticalResolution - delta; y < gop_info.VerticalResolution; y++) {
        for (unsigned int x = 0; x < gop_info.HorizontalResolution; x++) {
            dest[x] = 0; // black
        }

        dest += gop_info.PixelsPerScanLine;
    }
}

void draw_text(const char* s, text_pos* p) {
    unsigned int len = strlen(s);

    for (unsigned int i = 0; i < len; i++) {
        char* v = font8x8_basic[(unsigned int)s[i]];

        if (s[i] == '\n') {
            p->y++;
            p->x = 0;

            if (p->y >= console_height) {
                move_up_console();
                p->y = console_height - 1;
            }

            continue;
        }

        uint32_t* base = (uint32_t*)framebuffer + (gop_info.PixelsPerScanLine * p->y * 8) + (p->x * 8);

        for (unsigned int y = 0; y < 8; y++) {
            uint8_t v2 = v[y];
            uint32_t* buf = base + (gop_info.PixelsPerScanLine * y);

            for (unsigned int x = 0; x < 8; x++) {
                if (v2 & 1)
                    *buf = 0xffffffff;
                else
                    *buf = 0;

                v2 >>= 1;
                buf++;
            }
        }

        p->x++;

        if (p->x > console_width) {
            p->y++;
            p->x = 0;

            if (p->y >= console_height) {
                move_up_console();
                p->y = console_height - 1;
            }
        }
    }
}

void init_gop_console() {
    console_width = gop_info.HorizontalResolution / 8;
    console_height = gop_info.VerticalResolution / 8;
}

void print_string(const char* s) {
    if (!have_csm)
        draw_text(s, &console_pos);
    else {
        WCHAR w[255], *t;

        // FIXME - make sure no overflow

        t = w;

        while (*s) {
            if (*s == '\n') {
                *t = '\r';
                t++;
            }

            *t = *s;
            s++;
            t++;
        }

        *t = 0;

        systable->ConOut->OutputString(systable->ConOut, (CHAR16*)w);
    }
}

void print_error(const char* func, EFI_STATUS Status) {
    char s[255], *p;

    p = stpcpy(s, func);
    p = stpcpy(p, " returned ");
    p = stpcpy(p, error_string(Status));
    p = stpcpy(p, "\n");

    print_string(s);
}
