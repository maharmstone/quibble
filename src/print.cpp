#include <string.h>
#include "quibble.h"
#include "quibbleproto.h"
#include "print.h"
#include "misc.h"
#include <ft2build.h>
#include <freetype/freetype.h>

static EFI_HANDLE info_handle = NULL;
static EFI_QUIBBLE_INFO_PROTOCOL info_proto;
text_pos console_pos;
static unsigned int console_width, console_height;
static FT_Library ft = NULL;
static FT_Face face = NULL;
bool gop_console = false;
unsigned int font_height = 0;
uint8_t* ft_pool = nullptr;
static FT_MemoryRec_ ftmem;

extern void* framebuffer;
extern EFI_GRAPHICS_OUTPUT_MODE_INFORMATION gop_info;
extern bool have_edid;
extern uint8_t edid[128];

// in font.s
extern void* font_data_start asm("_font_data_start");
extern size_t font_size asm ("_font_size");

void* font_data = &font_data_start;

struct alloc_header {
    uint32_t size : 31;
    uint32_t free : 1;
};

static_assert(sizeof(alloc_header) == 4);

EFI_STATUS info_register(EFI_BOOT_SERVICES* bs) {
    EFI_GUID info_guid = EFI_QUIBBLE_INFO_PROTOCOL_GUID;

    info_proto.Print = print_string;

    return bs->InstallProtocolInterface(&info_handle, &info_guid, EFI_NATIVE_INTERFACE, &info_proto);
}

static void move_up_console(unsigned int delta) {
    uint32_t* src;
    uint32_t* dest;

    src = (uint32_t*)shadow_fb + (gop_info.PixelsPerScanLine * delta);
    dest = (uint32_t*)shadow_fb;

    memcpy(dest, src, gop_info.PixelsPerScanLine * (gop_info.VerticalResolution - delta) * sizeof(uint32_t));
    dest += gop_info.PixelsPerScanLine * (gop_info.VerticalResolution - delta);

    memset(dest, 0, gop_info.PixelsPerScanLine * delta * sizeof(uint32_t)); // black

    memcpy(framebuffer, shadow_fb, framebuffer_size);
}

void draw_text_ft(const char* s, text_pos* p, uint32_t bg_colour, uint32_t fg_colour) {
    size_t len;
    FT_Error error;
    uint32_t* base;
    uint32_t* shadow_base;
    FT_Bitmap* bitmap;
    uint8_t bg_r, bg_g, bg_b, fg_r, fg_g, fg_b;

    bg_r = bg_colour >> 16;
    bg_g = (bg_colour >> 8) & 0xff;
    bg_b = bg_colour & 0xff;
    fg_r = fg_colour >> 16;
    fg_g = (fg_colour >> 8) & 0xff;
    fg_b = fg_colour & 0xff;

    len = strlen(s);

    for (size_t i = 0; i < len; i++) {
        uint8_t* buf;
        uint32_t skip_y, width;
        uint32_t cp;

        if (s[i] == '\n') {
            p->x = 0;
            p->y += font_height;

            if (p->y > gop_info.VerticalResolution - font_height) {
                move_up_console(font_height);
                p->y -= font_height;
            }

            continue;
        }

        // get UTF-8 code point

        if (!((uint8_t)s[i] & 0x80))
            cp = s[i];
        else if (((uint8_t)s[i] & 0xe0) == 0xc0) { // 2-byte UTF-8
            if (((uint8_t)s[i+1] & 0xc0) != 0x80)
                cp = 0xfffd;
            else {
                cp = (((uint8_t)s[i] & 0x1f) << 6) | ((uint8_t)s[i+1] & 0x3f);
                i++;
            }
        } else if (((uint8_t)s[i] & 0xf0) == 0xe0) { // 3-byte UTF-8
            if (((uint8_t)s[i+1] & 0xc0) != 0x80 || ((uint8_t)s[i+2] & 0xc0) != 0x80)
                cp = 0xfffd;
            else {
                cp = (((uint8_t)s[i] & 0xf) << 12) | (((uint8_t)s[i+1] & 0x3f) << 6) | ((uint8_t)s[i+2] & 0x3f);
                i += 2;
            }
        } else if (((uint8_t)s[i] & 0xf8) == 0xf0) { // 4-byte UTF-8
            if (((uint8_t)s[i+1] & 0xc0) != 0x80 || ((uint8_t)s[i+2] & 0xc0) != 0x80 || ((uint8_t)s[i+3] & 0xc0) != 0x80)
                cp = 0xfffd;
            else {
                cp = (((uint8_t)s[i] & 0x7) << 18) | (((uint8_t)s[i+1] & 0x3f) << 12) | (((uint8_t)s[i+2] & 0x3f) << 6) | ((uint8_t)s[i+3] & 0x3f);
                i += 3;
            }
        } else
            cp = 0xfffd;

        error = FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_RENDER_MODE_MONO);
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
        shadow_base = (uint32_t*)(((uint8_t*)base - (uint8_t*)framebuffer) + (uint8_t*)shadow_fb);

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
                if ((*buf == 0xff || bg_colour == 0x000000) && fg_colour == 0xffffff)
                    base[x] = shadow_base[x] = (*buf << 16) | (*buf << 8) | *buf;
                else if (*buf != 0) {
                    float f = *buf / 255.0f;
                    uint8_t r = ((1.0f - f) * bg_r) + (f * fg_r);
                    uint8_t g = ((1.0f - f) * bg_g) + (f * fg_g);
                    uint8_t b = ((1.0f - f) * bg_b) + (f * fg_b);

                    base[x] = shadow_base[x] = (r << 16) | (g << 8) | b;
                }

                buf++;
            }

            buf += bitmap->width - width;

            base += gop_info.PixelsPerScanLine;
            shadow_base += gop_info.PixelsPerScanLine;
        }

        p->x += face->glyph->advance.x / 64;
        p->y += face->glyph->advance.y / 64;
    }
}

EFI_STATUS load_font() {
    FT_Error error;

    error = FT_Init_FreeType(&ft);
    if (error) {
        char s[255], *p;

        p = stpcpy(s, "FT_Init_FreeType failed (");
        p = dec_to_str(p, error);
        p = stpcpy(p, ").\n");

        print_string(s);

        return EFI_INVALID_PARAMETER;
    }

    error = FT_New_Memory_Face(ft, (const FT_Byte*)font_data, font_size, 0, &face);
    if (error) {
        char s[255], *p;

        p = stpcpy(s, "FT_New_Memory_Face failed (");
        p = dec_to_str(p, error);
        p = stpcpy(p, ").\n");

        print_string(s);

        return EFI_INVALID_PARAMETER;
    }

    return EFI_SUCCESS;
}

void init_gop_console() {
    FT_Error error;
    unsigned int dpi = 96;

    console_width = gop_info.HorizontalResolution / 8;
    console_height = gop_info.VerticalResolution / 8;

    // FIXME - allow font size to be specified

    if (have_edid) {
        uint8_t screen_x_cm = edid[21];
        uint8_t screen_y_cm = edid[22];

        if (screen_x_cm != 0 && screen_y_cm != 0) {
            float screen_x_in = (float)screen_x_cm / 2.54f;

            dpi = (unsigned int)((float)gop_info.HorizontalResolution / screen_x_in);
        }
    }

    error = FT_Set_Char_Size(face, 12 * 64, 0, dpi, 0);
    if (error) {
        char s[255], *p;

        p = stpcpy(s, "FT_Set_Char_Size failed (");
        p = dec_to_str(p, error);
        p = stpcpy(p, ").\n");

        print_string(s);

        return;
    }

    font_height = face->size->metrics.height / 64;

    console_pos.x = 0;
    console_pos.y = font_height;

    gop_console = true;
}

void print_string(const char* s) {
    if (face)
        draw_text_ft(s, &console_pos, 0x000000, 0xffffff);
    else {
        wchar_t w[255], *t;

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

static void* ft_alloc(FT_Memory, long size) {
    if (size < 0)
        return nullptr;

    // round to multiple of 4
    if (size & 3)
        size = ((size >> 2) + 1) << 2;

    if (size == 0)
        return nullptr;

    auto h = (alloc_header*)ft_pool;
    alloc_header* last = nullptr;

    do {
        if (h->free) {
            if (last && last->free) { // merge adjacent free entries
                last->size += h->size + sizeof(alloc_header);
                h = last;
            }

            auto ptr = (uint8_t*)h + sizeof(alloc_header);

            if (h->size == size) {
                h->free = 0;
                return ptr;
            }

            if (h->size > size) {
                auto& nh = *(alloc_header*)((uint8_t*)h + sizeof(alloc_header) + size);

                if (&nh < (void*)((uintptr_t)ft_pool + (FT_POOL_PAGES << EFI_PAGE_SHIFT))) {
                    nh.free = 1;
                    nh.size = h->size - size - sizeof(alloc_header);
                }

                h->free = 0;
                h->size = size;

                return ptr;
            }
        }

        last = h;
        h = (alloc_header*)((uint8_t*)h + sizeof(alloc_header) + h->size);
    } while (h < (void*)((uintptr_t)ft_pool + (FT_POOL_PAGES << EFI_PAGE_SHIFT)));

    return nullptr;
}

static void ft_free(FT_Memory, void* block) {
    auto& ah = *(alloc_header*)((uint8_t*)block - sizeof(alloc_header));

    ah.free = 1;
}

static void* ft_realloc(FT_Memory memory, long cur_size, long new_size, void* block) {
    void* ret;

    // FIXME - do actual realloc if possible

    if (new_size < cur_size)
        return block;

    ret = ft_alloc(memory, new_size);
    if (!ret)
        return nullptr;

    memcpy(ret, block, new_size < cur_size ? new_size : cur_size);

    ft_free(memory, block);

    return ret;
}

extern "C"
FT_Memory FT_New_Memory() {
    EFI_STATUS Status;
    EFI_PHYSICAL_ADDRESS addr;

    /* We use our own allocation functions here, rather than relying on
     * AllocatePool, so that we can carry on using FreeType after exiting
     * boot services. */

    Status = systable->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, FT_POOL_PAGES, &addr);
    if (EFI_ERROR(Status))
        return nullptr;

    ft_pool = (uint8_t*)addr;

    memset(ft_pool, 0, FT_POOL_PAGES << EFI_PAGE_SHIFT);

    auto& h = *(alloc_header*)ft_pool;

    h.size = (FT_POOL_PAGES << EFI_PAGE_SHIFT) - sizeof(alloc_header);
    h.free = 1;

    ftmem.user = nullptr;
    ftmem.alloc = ft_alloc;
    ftmem.realloc = ft_realloc;
    ftmem.free = ft_free;

    return &ftmem;
}

extern "C"
void FT_Done_Memory(FT_Memory memory) {
    UNUSED(memory);
}

/* Dummy versions follow of unused functions linked to by FreeType. This is so
 * we can bring in the upstream version directly, rather than having to patch
 * the offending bits out. */

#ifdef __x86_64__

extern "C"
void __imp_longjmp(jmp_buf, int) {
    abort();
}

#elif defined(_X86_)

extern "C"
void _imp__longjmp(jmp_buf, int) {
    abort();
}

#endif

extern "C"
void qsort(void*, size_t, size_t, int (*)(const void*, const void*)) {
    abort();
}

extern "C"
int _setjmp(jmp_buf, void*) {
    return 0;
}

extern "C"
int _setjmp3(jmp_buf, void*) {
    return 0;
}

extern "C"
char* getenv(const char*) {
    return nullptr;
}

extern "C"
char* strrchr(const char*, int) {
    abort();
}

extern "C"
char* strncpy(char*, const char*, size_t) {
    abort();
}

extern "C"
int __mingw_vsprintf(char*, const char*, va_list) {
    abort();
}
