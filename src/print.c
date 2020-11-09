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

void draw_text(const char* s, text_pos* p) {
    unsigned int len = strlen(s);

    for (unsigned int i = 0; i < len; i++) {
        char* v = font8x8_basic[(unsigned int)s[i]];

        if (s[i] == '\n') {
            p->y++;
            p->x = 0;

            if (p->y > console_height)
                p->y = 0; // FIXME

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

            if (p->y > console_height)
                p->y = 0; // FIXME
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

static WCHAR* error_string_utf16(EFI_STATUS Status) {
    switch (Status) {
        case EFI_SUCCESS:
            return L"EFI_SUCCESS";

        case EFI_LOAD_ERROR:
            return L"EFI_LOAD_ERROR";

        case EFI_INVALID_PARAMETER:
            return L"EFI_INVALID_PARAMETER";

        case EFI_UNSUPPORTED:
            return L"EFI_UNSUPPORTED";

        case EFI_BAD_BUFFER_SIZE:
            return L"EFI_BAD_BUFFER_SIZE";

        case EFI_BUFFER_TOO_SMALL:
            return L"EFI_BUFFER_TOO_SMALL";

        case EFI_NOT_READY:
            return L"EFI_NOT_READY";

        case EFI_DEVICE_ERROR:
            return L"EFI_DEVICE_ERROR";

        case EFI_WRITE_PROTECTED:
            return L"EFI_WRITE_PROTECTED";

        case EFI_OUT_OF_RESOURCES:
            return L"EFI_OUT_OF_RESOURCES";

        case EFI_VOLUME_CORRUPTED:
            return L"EFI_VOLUME_CORRUPTED";

        case EFI_VOLUME_FULL:
            return L"EFI_VOLUME_FULL";

        case EFI_NO_MEDIA:
            return L"EFI_NO_MEDIA";

        case EFI_MEDIA_CHANGED:
            return L"EFI_MEDIA_CHANGED";

        case EFI_NOT_FOUND:
            return L"EFI_NOT_FOUND";

        case EFI_ACCESS_DENIED:
            return L"EFI_ACCESS_DENIED";

        case EFI_NO_RESPONSE:
            return L"EFI_NO_RESPONSE";

        case EFI_NO_MAPPING:
            return L"EFI_NO_MAPPING";

        case EFI_TIMEOUT:
            return L"EFI_TIMEOUT";

        case EFI_NOT_STARTED:
            return L"EFI_NOT_STARTED";

        case EFI_ALREADY_STARTED:
            return L"EFI_ALREADY_STARTED";

        case EFI_ABORTED:
            return L"EFI_ABORTED";

        case EFI_ICMP_ERROR:
            return L"EFI_ICMP_ERROR";

        case EFI_TFTP_ERROR:
            return L"EFI_TFTP_ERROR";

        case EFI_PROTOCOL_ERROR:
            return L"EFI_PROTOCOL_ERROR";

        case EFI_INCOMPATIBLE_VERSION:
            return L"EFI_INCOMPATIBLE_VERSION";

        case EFI_SECURITY_VIOLATION:
            return L"EFI_SECURITY_VIOLATION";

        case EFI_CRC_ERROR:
            return L"EFI_CRC_ERROR";

        case EFI_END_OF_MEDIA:
            return L"EFI_END_OF_MEDIA";

        case EFI_END_OF_FILE:
            return L"EFI_END_OF_FILE";

        case EFI_INVALID_LANGUAGE:
            return L"EFI_INVALID_LANGUAGE";

        case EFI_COMPROMISED_DATA:
            return L"EFI_COMPROMISED_DATA";

        default:
            return L"(unknown error)";
    }
}

void print_error(const WCHAR* func, EFI_STATUS Status) {
    WCHAR s[255];

    wcsncpy(s, func, sizeof(s) / sizeof(WCHAR));
    wcsncat(s, L" returned ", sizeof(s) / sizeof(WCHAR));
    wcsncat(s, error_string_utf16(Status), sizeof(s) / sizeof(WCHAR));
    wcsncat(s, L"\r\n", sizeof(s) / sizeof(WCHAR));

    systable->ConOut->OutputString(systable->ConOut, s);
}

