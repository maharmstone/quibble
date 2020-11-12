#include <string.h>
#include "quibble.h"
#include "quibbleproto.h"
#include "print.h"
#include "misc.h"
#include "font8x8_basic.h"
#include <ft2build.h>
#include <freetype/freetype.h>

static EFI_HANDLE info_handle = NULL;
static EFI_QUIBBLE_INFO_PROTOCOL info_proto;
text_pos console_pos;
static unsigned int console_width, console_height;
static FT_Library ft = NULL;
static FT_Face face = NULL;
static void* font_data = NULL;
static size_t font_size;

unsigned int font_height = 0;

extern bool have_csm;
extern void* framebuffer;
extern EFI_GRAPHICS_OUTPUT_MODE_INFORMATION gop_info;

EFI_STATUS info_register(EFI_BOOT_SERVICES* bs) {
    EFI_GUID info_guid = EFI_QUIBBLE_INFO_PROTOCOL_GUID;

    info_proto.Print = print_string;

    return bs->InstallProtocolInterface(&info_handle, &info_guid, EFI_NATIVE_INTERFACE, &info_proto);
}

static void move_up_console(unsigned int delta) {
    uint32_t* src;
    uint32_t* dest;

    src = (uint32_t*)framebuffer + (gop_info.PixelsPerScanLine * delta);
    dest = (uint32_t*)framebuffer;

    for (unsigned int y = 0; y < gop_info.VerticalResolution - delta; y++) {
        memcpy(dest, src, gop_info.HorizontalResolution * sizeof(uint32_t));
        src += gop_info.PixelsPerScanLine;
        dest += gop_info.PixelsPerScanLine;
    }

    for (unsigned int y = gop_info.VerticalResolution - delta; y < gop_info.VerticalResolution; y++) {
        memset(dest, 0, gop_info.HorizontalResolution * sizeof(uint32_t)); // black
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
                move_up_console(8);
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
                move_up_console(8);
                p->y = console_height - 1;
            }
        }
    }
}

void draw_text_ft(const char* s, text_pos* p, uint32_t bg_colour) {
    size_t len;
    FT_Error error;
    uint32_t* base;
    FT_Bitmap* bitmap;

    len = strlen(s);

    // FIXME - UTF-8

    for (size_t i = 0; i < len; i++) {
        uint8_t* buf;
        uint32_t skip_y, width;

        if (s[i] == '\n') {
            p->x = 0;
            p->y += font_height;

            if (p->y > gop_info.VerticalResolution - font_height) {
                move_up_console(font_height);
                p->y -= font_height;
            }

            continue;
        }

        error = FT_Load_Char(face, s[i], FT_LOAD_RENDER | FT_RENDER_MODE_MONO);
        if (error)
            continue;

        bitmap = &face->glyph->bitmap;

        // if overruns right of screen, do newline
        if (p->x + face->glyph->bitmap_left + bitmap->width >= gop_info.HorizontalResolution) {
            p->x = 0;
            p->y += font_height;

            if (p->y > gop_info.VerticalResolution - font_height) {
                move_up_console(font_height);
                p->y -= font_height;
            }
        }

        // FIXME - make sure won't overflow left of screen
        base = (uint32_t*)framebuffer;

        if ((int)p->y > face->glyph->bitmap_top)
            base += gop_info.PixelsPerScanLine * (p->y - face->glyph->bitmap_top);

        base += p->x + face->glyph->bitmap_left;

        buf = bitmap->buffer;

        width = bitmap->width;
        if (p->x + face->glyph->bitmap_left + width > gop_info.HorizontalResolution)
            width = gop_info.HorizontalResolution - p->x - face->glyph->bitmap_left;

        if ((int)p->y < face->glyph->bitmap_top) {
            skip_y = face->glyph->bitmap_top - p->y;
            buf += bitmap->width * skip_y;
        } else
            skip_y = 0;

        for (unsigned int y = skip_y; y < bitmap->rows; y++) {
            if (p->y - face->glyph->bitmap_top + y >= gop_info.VerticalResolution)
                break;

            for (unsigned int x = 0; x < width; x++) {
                if (*buf == 0xff || bg_colour == 0x000000)
                    base[x] = (*buf << 16) | (*buf << 8) | *buf;
                else if (*buf != 0) {
                    // FIXME - should we be doing this without using floats?

                    float f = *buf / 255.0f;
                    uint8_t r = ((1.0f - f) * (bg_colour >> 16)) + (f * 255.0f);
                    uint8_t g = ((1.0f - f) * ((bg_colour >> 8) & 0xff)) + (f * 255.0f);
                    uint8_t b = ((1.0f - f) * (bg_colour & 0xff)) + (f * 255.0f);

                    base[x] = (r << 16) | (g << 8) | b;
                }

                buf++;
            }

            buf += bitmap->width - width;

            base += gop_info.PixelsPerScanLine;
        }

        p->x += face->glyph->advance.x / 64;
        p->y += face->glyph->advance.y / 64;
    }
}

void init_gop_console() {
    EFI_STATUS Status;
    EFI_BOOT_SERVICES* bs = systable->BootServices;
    EFI_GUID guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID guid2 = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_LOADED_IMAGE_PROTOCOL* image;
    EFI_FILE_IO_INTERFACE* fs;
    EFI_FILE_HANDLE dir;
    FT_Error error;

    console_width = gop_info.HorizontalResolution / 8;
    console_height = gop_info.VerticalResolution / 8;

    Status = bs->OpenProtocol(image_handle, &guid, (void**)&image, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        print_error("OpenProtocol", Status);
        return;
    }

    if (!image->DeviceHandle) {
        bs->CloseProtocol(image_handle, &guid, image_handle, NULL);
        return;
    }

    Status = bs->OpenProtocol(image->DeviceHandle, &guid2, (void**)&fs, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        print_error("OpenProtocol", Status);
        bs->CloseProtocol(image_handle, &guid, image_handle, NULL);
        return;
    }

    Status = bs->OpenProtocol(image->DeviceHandle, &guid2, (void**)&fs, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        print_error("OpenProtocol", Status);
        bs->CloseProtocol(image_handle, &guid, image_handle, NULL);
        return;
    }

    Status = open_parent_dir(fs, (FILEPATH_DEVICE_PATH*)image->FilePath, &dir);
    if (EFI_ERROR(Status)) {
        print_error("open_parent_dir", Status);
        bs->CloseProtocol(image->DeviceHandle, &guid2, image_handle, NULL);
        bs->CloseProtocol(image_handle, &guid, image_handle, NULL);
        return;
    }

    // FIXME - allow font filename to be specified in freeldr.ini
    Status = read_file(bs, dir, L"font.ttf", (void**)&font_data, &font_size);

    dir->Close(dir);

    bs->CloseProtocol(image->DeviceHandle, &guid2, image_handle, NULL);
    bs->CloseProtocol(image_handle, &guid, image_handle, NULL);

    if (EFI_ERROR(Status)) {
        print_string("Could not load font file.\n");
        print_error("read_file", Status);
        return;
    }

    error = FT_Init_FreeType(&ft);
    if (error) {
        char s[255], *p;

        p = stpcpy(s, "FT_Init_FreeType failed (");
        p = dec_to_str(p, error);
        p = stpcpy(p, ").\n");

        print_string(s);

        return;
    }

    error = FT_New_Memory_Face(ft, font_data, font_size, 0, &face);
    if (error) {
        char s[255], *p;

        p = stpcpy(s, "FT_New_Memory_Face failed (");
        p = dec_to_str(p, error);
        p = stpcpy(p, ").\n");

        print_string(s);

        return;
    }

    // FIXME - allow font size to be specified
    // FIXME - get DPI from EDID?
    error = FT_Set_Char_Size(face, 12 * 64, 0, 100, 0);
    if (error) {
        char s[255], *p;

        p = stpcpy(s, "FT_Set_Char_Size failed (");
        p = dec_to_str(p, error);
        p = stpcpy(p, ").\n");

        print_string(s);

        return;
    }

    font_height = face->size->metrics.height / 64;
}

void print_string(const char* s) {
    if (!have_csm) {
        if (face)
            draw_text_ft(s, &console_pos, 0x000000);
        else
            draw_text(s, &console_pos);
    } else {
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

static void* ft_alloc(FT_Memory memory, long size) {
    EFI_STATUS Status;
    void* ret;

    UNUSED(memory);

    Status = systable->BootServices->AllocatePool(EfiLoaderData, size, &ret);
    if (EFI_ERROR(Status))
        return NULL;

    return ret;
}

static void* ft_realloc(FT_Memory memory, long cur_size, long new_size, void* block) {
    EFI_STATUS Status;
    void* ret;

    UNUSED(memory);

    Status = systable->BootServices->AllocatePool(EfiLoaderData, new_size, &ret);
    if (EFI_ERROR(Status))
        return NULL;

    memcpy(ret, block, cur_size < new_size ? new_size : cur_size);

    systable->BootServices->FreePool(block);

    return ret;
}

void ft_free(FT_Memory memory, void* block) {
    UNUSED(memory);

    systable->BootServices->FreePool(block);
}

FT_Memory FT_New_Memory() {
    EFI_STATUS Status;
    FT_Memory memory;

    Status = systable->BootServices->AllocatePool(EfiLoaderData, sizeof(*memory), (void**)&memory);
    if (EFI_ERROR(Status))
        return NULL;

    memory->user = NULL;
    memory->alloc = ft_alloc;
    memory->realloc = ft_realloc;
    memory->free = ft_free;

    return memory;
}

void FT_Done_Memory(FT_Memory memory) {
    UNUSED(memory);
}
