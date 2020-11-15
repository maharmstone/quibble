/* Copyright (c) Mark Harmstone 2020
 *
 * This file is part of Quibble.
 *
 * Quibble is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * Quibble is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with Quibble.  If not, see <http://www.gnu.org/licenses/>. */

#include <string.h>
#include "config.h"
#include "quibble.h"
#include "misc.h"
#include "x86.h"
#include "print.h"

typedef struct {
    LIST_ENTRY list_entry;
    LIST_ENTRY children;
    char name[1];
} ini_section;

typedef struct {
    LIST_ENTRY list_entry;
    char* name;
    char* value;
} ini_value;

#define VERSION "Quibble " PROJECT_VER
#define VERSIONW L"Quibble " PROJECT_VERW

#define URL "https://github.com/maharmstone/quibble"
#define URLW L"https://github.com/maharmstone/quibble"

static boot_option* options = NULL;
static unsigned int num_options, selected_option;

extern void* framebuffer;
extern void* shadow_fb;
extern EFI_GRAPHICS_OUTPUT_MODE_INFORMATION gop_info;
extern unsigned int font_height;
extern text_pos console_pos;

static const char timeout_message[] = "Time until selected option is chosen: ";
static const WCHAR timeout_messagew[] = L"Time until selected option is chosen: ";

static EFI_STATUS parse_ini_file(char* data, LIST_ENTRY* ini_sections) {
    EFI_STATUS Status;
    char* s = data;
    char* sectname = NULL;
    unsigned int sectnamelen = 0;
    ini_section* sect = NULL;

    do {
        if (*s == '[') {
            sectname = s + 1;
            sectnamelen = 0;
            sect = NULL;

            s++;
            do {
                s++;
                sectnamelen++;
            } while (*s != 0 && *s != ']' && *s != '\n');

            while (*s != 0 && *s != '\n') {
                s++;
            }
        } else if (*s == '\n')
            s++;
        else if (*s == ';') { // comment
            while (*s != 0 && *s != '\n') {
                s++;
            }
        } else {
            char* line = s;
            unsigned int linelen = 0;
            char* name = line;
            unsigned int namelen;
            char* value = NULL;
            unsigned int valuelen = 0;
            ini_value* item;

            do {
                linelen++;
                s++;
            } while (*s != 0 && *s != '\n');

            namelen = linelen;

            for (unsigned int i = 0; i < linelen; i++) {
                if (line[i] == '=') {
                    namelen = i;
                    value = &line[i+1];
                    valuelen = linelen - i - 1;
                    break;
                }
            }

            // remove whitespace round name
            while (namelen > 0 && (*name == ' ' || *name == '\t' || *name == '\r')) {
                name++;
                namelen--;
            }

            while (namelen > 0 && (name[namelen - 1] == ' ' || name[namelen - 1] == '\t' || name[namelen - 1] == '\r')) {
                namelen--;
            }

            if (namelen == 0)
                continue;

            // remove whitespace round value
            while (valuelen > 0 && (*value == ' ' || *value == '\t' || *value == '\r')) {
                value++;
                valuelen--;
            }

            while (valuelen > 0 && (value[valuelen - 1] == ' ' || value[valuelen - 1] == '\t' || value[valuelen - 1] == '\r')) {
                valuelen--;
            }

            // remove quotes around value
            if (valuelen >= 2 && *value == '"' && value[valuelen - 1] == '"') {
                value++;
                valuelen -= 2;
            }

            if (!sect) { // allocate new section
                Status = systable->BootServices->AllocatePool(EfiLoaderData, offsetof(ini_section, name[0]) + sectnamelen + 1,
                                                              (void**)&sect);
                if (EFI_ERROR(Status)) {
                    print_error("AllocatePool", Status);
                    return Status;
                }

                InitializeListHead(&sect->children);
                memcpy(sect->name, sectname, sectnamelen);
                sect->name[sectnamelen] = 0;

                InsertTailList(ini_sections, &sect->list_entry);
            }

            Status = systable->BootServices->AllocatePool(EfiLoaderData, sizeof(ini_value) + namelen + 1 + valuelen + 1,
                                                          (void**)&item);
            if (EFI_ERROR(Status)) {
                print_error("AllocatePool", Status);
                return Status;
            }

            item->name = (char*)item + sizeof(ini_value);
            item->value = item->name + namelen + 1;

            memcpy(item->name, name, namelen);
            item->name[namelen] = 0;

            memcpy(item->value, value, valuelen);
            item->value[valuelen] = 0;

            InsertTailList(&sect->children, &item->list_entry);
        }
    } while (*s != 0);

    return EFI_SUCCESS;
}

static EFI_STATUS populate_options_from_ini(LIST_ENTRY* ini_sections, unsigned int* timeout) {
    EFI_STATUS Status;
    LIST_ENTRY* le;
    ini_section* os_sect = NULL;
    ini_section* freeldr_sect = NULL;
    boot_option* opt;
    ini_value* default_val = NULL;
    unsigned int num = 0;

    le = ini_sections->Flink;
    while (le != ini_sections) {
        ini_section* sect = _CR(le, ini_section, list_entry);

        if (!stricmp(sect->name, "Operating Systems"))
            os_sect = sect;
        else if (!stricmp(sect->name, "FREELOADER"))
            freeldr_sect = sect;

        le = le->Flink;
    }

    if (!os_sect)
        return EFI_SUCCESS;

    num_options = 0;

    le = os_sect->children.Flink;
    while (le != &os_sect->children) {
        ini_value* v = _CR(le, ini_value, list_entry);

        if (v->value[0] != 0)
            num_options++;

        le = le->Flink;
    }

    if (num_options == 0)
        return EFI_SUCCESS;

    selected_option = 0;

    Status = systable->BootServices->AllocatePool(EfiLoaderData, sizeof(boot_option) * num_options, (void**)&options);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    memset(options, 0, sizeof(boot_option) * num_options);

    opt = options;

    if (freeldr_sect) {
        le = freeldr_sect->children.Flink;
        while (le != &freeldr_sect->children) {
            ini_value* v = _CR(le, ini_value, list_entry);

            if (!stricmp(v->name, "DefaultOS"))
                default_val = v;
            else if (!stricmp(v->name, "TimeOut")) {
                bool numeric = true;
                char* s = v->value;

                while (*s != 0) {
                    if (*s < '0' || *s > '9') {
                        numeric = false;
                        break;
                    }

                    s++;
                }

                if (numeric) {
                    s = v->value;

                    *timeout = 0;

                    while (*s != 0) {
                        *timeout *= 10;
                        *timeout += *s - '0';

                        s++;
                    }
                }
            }

            le = le->Flink;
        }
    }

    num = 0;

    le = os_sect->children.Flink;
    while (le != &os_sect->children) {
        ini_value* v = _CR(le, ini_value, list_entry);

        if (v->value[0] != 0) {
            unsigned int len = strlen(v->value);
            LIST_ENTRY* le2;
            ini_section* sect = NULL;
            unsigned int wlen;

            if (gop_console) {
                size_t len = strlen(v->value);

                Status = systable->BootServices->AllocatePool(EfiLoaderData, len + 1, (void**)&opt->name);
                if (EFI_ERROR(Status)) {
                    print_error("AllocatePool", Status);
                    return Status;
                }

                memcpy(opt->name, v->value, len + 1);
            } else {
                Status = utf8_to_utf16(NULL, 0, &wlen, v->value, len);
                if (EFI_ERROR(Status)) {
                    print_error("utf8_to_utf16", Status);
                    return Status;
                }

                Status = systable->BootServices->AllocatePool(EfiLoaderData, wlen + sizeof(WCHAR), (void**)&opt->namew);
                if (EFI_ERROR(Status)) {
                    print_error("AllocatePool", Status);
                    return Status;
                }

                Status = utf8_to_utf16(opt->namew, wlen, &wlen, v->value, len);
                if (EFI_ERROR(Status)) {
                    print_error("utf8_to_utf16", Status);
                    return Status;
                }

                opt->namew[wlen / sizeof(WCHAR)] = 0;
            }

            // find matching section
            le2 = ini_sections->Flink;
            while (le2 != ini_sections) {
                ini_section* sect2 = _CR(le2, ini_section, list_entry);

                if (!stricmp(sect2->name, v->name)) {
                    sect = sect2;
                    break;
                }

                le2 = le2->Flink;
            }

            if (sect) {
                le2 = sect->children.Flink;

                while (le2 != &sect->children) {
                    ini_value* v2 = _CR(le2, ini_value, list_entry);

                    if (v2->value[0] != 0) {
                        if (!stricmp(v2->name, "SystemPath")) {
                            unsigned int len = strlen(v2->value);

                            Status = systable->BootServices->AllocatePool(EfiLoaderData, len + 1, (void**)&opt->system_path);
                            if (EFI_ERROR(Status)) {
                                print_error("AllocatePool", Status);
                                return Status;
                            }

                            memcpy(opt->system_path, v2->value, len + 1);
                        } else if (!stricmp(v2->name, "Options")) {
                            unsigned int len = strlen(v2->value);

                            Status = systable->BootServices->AllocatePool(EfiLoaderData, len + 1, (void**)&opt->options);
                            if (EFI_ERROR(Status)) {
                                print_error("AllocatePool", Status);
                                return Status;
                            }

                            memcpy(opt->options, v2->value, len + 1);
                        }
                    }

                    le2 = le2->Flink;
                }
            }

            if (default_val && !stricmp(v->name, default_val->value))
                selected_option = num;

            opt++;
            num++;
        }

        le = le->Flink;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS load_ini_file(unsigned int* timeout) {
    EFI_STATUS Status;
    EFI_BOOT_SERVICES* bs = systable->BootServices;
    EFI_GUID guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID guid2 = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_LOADED_IMAGE_PROTOCOL* image;
    EFI_FILE_IO_INTERFACE* fs;
    EFI_FILE_HANDLE dir;
    char* data = NULL;
    size_t size;
    LIST_ENTRY ini_sections;

    InitializeListHead(&ini_sections);

    Status = bs->OpenProtocol(image_handle, &guid, (void**)&image, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        print_error("OpenProtocol", Status);
        return Status;
    }

    if (!image->DeviceHandle)
        goto end2;

    Status = bs->OpenProtocol(image->DeviceHandle, &guid2, (void**)&fs, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        print_error("OpenProtocol", Status);
        goto end2;
    }

    Status = open_parent_dir(fs, (FILEPATH_DEVICE_PATH*)image->FilePath, &dir);
    if (EFI_ERROR(Status)) {
        print_error("open_parent_dir", Status);
        goto end;
    }

    Status = read_file(bs, dir, L"freeldr.ini", (void**)&data, &size);

    dir->Close(dir);

    if (EFI_ERROR(Status)) {
        print_string("Error opening freeldr.ini.\n");
        print_error("read_file", Status);
        goto end;
    }

    Status = parse_ini_file(data, &ini_sections);
    if (EFI_ERROR(Status)) {
        print_error("parse_ini_file", Status);
        goto end;
    }

    Status = populate_options_from_ini(&ini_sections, timeout);
    if (EFI_ERROR(Status)) {
        print_error("populate_options_from_ini", Status);
        goto end;
    }

    Status = EFI_SUCCESS;

end:
    while (!IsListEmpty(&ini_sections)) {
        ini_section* sect = _CR(ini_sections.Flink, ini_section, list_entry);

        RemoveEntryList(&sect->list_entry);

        while (!IsListEmpty(&sect->children)) {
            ini_value* v = _CR(sect->children.Flink, ini_value, list_entry);

            RemoveEntryList(&v->list_entry);

            bs->FreePool(v);
        }

        bs->FreePool(sect);
    }

    if (data)
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)data, PAGE_COUNT(size));

    bs->CloseProtocol(image->DeviceHandle, &guid2, image_handle, NULL);

end2:
    bs->CloseProtocol(image_handle, &guid, image_handle, NULL);

    return Status;
}

static EFI_STATUS draw_box(EFI_SIMPLE_TEXT_OUT_PROTOCOL* con, unsigned int x, unsigned int y,
                           unsigned int w, unsigned int h) {
    EFI_STATUS Status;
    EFI_BOOT_SERVICES* bs = systable->BootServices;
    INT32 col, row;
    WCHAR* s;

    Status = bs->AllocatePool(EfiLoaderData, (w + 1) * sizeof(WCHAR), (void**)&s);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    col = con->Mode->CursorColumn;
    row = con->Mode->CursorRow;

    for (unsigned int i = y; i < y + h; i++) {
        Status = con->SetCursorPosition(con, x, i);
        if (EFI_ERROR(Status)) {
            print_error("SetCursorPosition", Status);
            bs->FreePool(s);
            return Status;
        }

        if (i == y) {
            s[0] = BOXDRAW_DOWN_RIGHT;

            for (unsigned int j = 1; j <= w - 2; j++) {
                s[j] = BOXDRAW_HORIZONTAL;
            }

            s[w - 1] = BOXDRAW_DOWN_LEFT;
        } else if (i == y + h - 1) {
            s[0] = BOXDRAW_UP_RIGHT;

            for (unsigned int j = 1; j <= w - 2; j++) {
                s[j] = BOXDRAW_HORIZONTAL;
            }

            s[w - 1] = BOXDRAW_UP_LEFT;
        } else {
            s[0] = BOXDRAW_VERTICAL;

            for (unsigned int j = 1; j <= w - 2; j++) {
                s[j] = ' ';
            }

            s[w - 1] = BOXDRAW_VERTICAL;
        }

        s[w] = 0;

        Status = con->OutputString(con, s);
        if (EFI_ERROR(Status)) {
            con->SetCursorPosition(con, col, row);
            print_error("OutputString", Status);
            bs->FreePool(s);
            return Status;
        }
    }

    con->SetCursorPosition(con, col, row);

    bs->FreePool(s);

    return EFI_SUCCESS;
}

static EFI_STATUS draw_option(EFI_SIMPLE_TEXT_OUT_PROTOCOL* con, unsigned int pos, unsigned int width,
                              WCHAR* text, bool selected) {
    EFI_BOOT_SERVICES* bs = systable->BootServices;
    EFI_STATUS Status;
    WCHAR* s;
    unsigned int len;

    Status = con->SetCursorPosition(con, 1, pos + 3);
    if (EFI_ERROR(Status)) {
        print_error("SetCursorPosition", Status);
        return Status;
    }

    if (selected) {
        Status = con->SetAttribute(con, EFI_BLACK | EFI_BACKGROUND_LIGHTGRAY);
        if (EFI_ERROR(Status)) {
            print_error("SetAttribute", Status);
            return Status;
        }
    }

    Status = bs->AllocatePool(EfiLoaderData, (width + 1) * sizeof(WCHAR), (void**)&s);
    if (EFI_ERROR(Status)) {
        print_error("AllocatePool", Status);
        return Status;
    }

    len = wcslen(text);

    for (unsigned int i = 0; i < width; i++) {
        s[i] = ' ';
    }

    s[width] = 0;

    memcpy(s, text, (len < width ? len : width) * sizeof(WCHAR));

    // FIXME - add ellipsis if truncated?

    Status = con->OutputString(con, s);
    if (EFI_ERROR(Status)) {
        print_error("OutputString", Status);
        bs->FreePool(s);
        return Status;
    }

    if (selected) { // change back
        Status = con->SetAttribute(con, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        if (EFI_ERROR(Status)) {
            print_error("SetAttribute", Status);
            bs->FreePool(s);
            return Status;
        }
    }

    bs->FreePool(s);

    return EFI_SUCCESS;
}

static EFI_STATUS draw_options(EFI_SIMPLE_TEXT_OUT_PROTOCOL* con, unsigned int cols) {
    EFI_STATUS Status;

    // FIXME - paging

    for (unsigned int i = 0; i < num_options; i++) {
        Status = draw_option(con, i, cols - 3, options[i].namew, i == selected_option);
        if (EFI_ERROR(Status)) {
            print_error("draw_option", Status);
            return Status;
        }
    }

    return EFI_SUCCESS;
}

static EFI_STATUS print_spaces(EFI_SIMPLE_TEXT_OUT_PROTOCOL* con, unsigned int num) {
    EFI_STATUS Status;

    for (unsigned int i = 0; i < num; i++) {
        Status = con->OutputString(con, L" ");
        if (EFI_ERROR(Status)) {
            print_error("OutputString", Status);
            return Status;
        }
    }

    return EFI_SUCCESS;
}

static void print(const WCHAR* s) {
    systable->ConOut->OutputString(systable->ConOut, (CHAR16*)s);
}

static void print_dec(uint32_t v) {
    WCHAR s[12], *p;

    if (v == 0) {
        print(L"0");
        return;
    }

    s[11] = 0;
    p = &s[11];

    while (v != 0) {
        p = &p[-1];

        *p = (v % 10) + '0';

        v /= 10;
    }

    print(p);
}

static void draw_box_gop(unsigned int x, unsigned int y, unsigned int w, unsigned int h) {
    uint32_t* base;

    memset((uint32_t*)framebuffer + (y * gop_info.PixelsPerScanLine) + x, 0xff, w * sizeof(uint32_t));
    memset((uint32_t*)framebuffer + ((y + h) * gop_info.PixelsPerScanLine) + x, 0xff, w * sizeof(uint32_t));

    base = (uint32_t*)framebuffer + ((y + 1) * gop_info.PixelsPerScanLine);
    for (unsigned int i = 0; i < h - 1; i++) {
        base[x] = base[x + w - 1] = 0xffffffff;
        base += gop_info.PixelsPerScanLine;
    }
}

static void draw_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, uint32_t colour) {
    uint32_t* base;

    base = (uint32_t*)framebuffer + (y * gop_info.PixelsPerScanLine) + x;

    for (unsigned int i = 0; i < h; i++) {
        for (unsigned int j = 0; j < w; j++) {
            base[j] = colour;
        }

        base += gop_info.PixelsPerScanLine;
    }
}

static void draw_option_gop(unsigned int num, const char* name, bool selected) {
    text_pos p;

    // FIXME - non-TTF

    draw_rect(font_height + 1, (font_height * (num + 3)) + 1 + (font_height / 4),
              gop_info.HorizontalResolution - (2 * font_height) - 2, font_height,
              selected ? 0xcccccc : 0x000000);

    p.x = font_height * 3 / 2;
    p.y = font_height * (num + 4);

    if (selected)
        draw_text_ft(name, &p, 0xcccccc, 0x000000);
    else
        draw_text_ft(name, &p, 0x000000, 0xffffff);
}

static void draw_options_gop() {
    // FIXME - paging

    for (unsigned int i = 0; i < num_options; i++) {
        draw_option_gop(i, options[i].name, i == selected_option);
    }
}

EFI_STATUS show_menu(EFI_SYSTEM_TABLE* systable, boot_option** ret) {
    EFI_STATUS Status;
    UINTN cols, rows;
    EFI_EVENT evt;
    EFI_SIMPLE_TEXT_OUT_PROTOCOL* con = systable->ConOut;
    bool cursor_visible = con->Mode->CursorVisible;
    unsigned int timer = 10;
    bool timer_cancelled = false;

    static const uint64_t one_second = 10000000;

    // prevent the firmware from thinking we're hanging
    Status = systable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
    if (EFI_ERROR(Status)) {
        print_error("SetWatchdogTimer", Status);
        return Status;
    }

    if (gop_console) {
        text_pos p;

        memset(framebuffer, 0, gop_info.PixelsPerScanLine * gop_info.VerticalResolution * 4); // clear screen

        p.x = 0;
        p.y = font_height;

        draw_text_ft(VERSION "\n", &p, 0x000000, 0xffffff);
        draw_text_ft(URL "\n", &p, 0x000000, 0xffffff);
    } else {
        Status = con->ClearScreen(con);
        if (EFI_ERROR(Status)) {
            print_error("ClearScreen", Status);
            return Status;
        }

        Status = con->SetCursorPosition(con, 0, 0);
        if (EFI_ERROR(Status)) {
            print_error("SetCursorPosition", Status);
            return Status;
        }

        Status = con->OutputString(con, VERSIONW L"\r\n");
        if (EFI_ERROR(Status)) {
            print_error("OutputString", Status);
            return Status;
        }

        Status = con->OutputString(con, URLW L"\r\n");
        if (EFI_ERROR(Status)) {
            print_error("OutputString", Status);
            return Status;
        }

        Status = con->QueryMode(con, con->Mode->Mode, &cols, &rows);
        if (EFI_ERROR(Status)) {
            print_error("QueryMode", Status);
            return Status;
        }
    }

    // FIXME - BCD support

    Status = load_ini_file(&timer);
    if (EFI_ERROR(Status)) {
        print_error("load_ini_file", Status);
        return Status;
    }

    if (num_options == 0) {
        print_string("No options found in INI file.\n");
        return EFI_ABORTED;
    }

    if (timer > 0) {
        unsigned int timer_pos;

        if (gop_console) {
            text_pos p;
            char s[10];

            draw_box_gop(font_height, font_height * 3, gop_info.HorizontalResolution - (font_height * 2), gop_info.VerticalResolution - (font_height * 5));

            draw_options_gop();

            p.x = font_height;
            p.y = gop_info.VerticalResolution - (font_height * 3 / 4);

            draw_text_ft(timeout_message, &p, 0x000000, 0xffffff);

            timer_pos = p.x;

            dec_to_str(s, timer);
            draw_text_ft(s, &p, 0x000000, 0xffffff);
        } else {
            if (cursor_visible)
                con->EnableCursor(con, false);

            Status = draw_box(con, 0, 2, cols - 1, rows - 3);
            if (EFI_ERROR(Status)) {
                print_error("draw_box", Status);
                goto end;
            }

            Status = draw_options(con, cols);
            if (EFI_ERROR(Status)) {
                print_error("draw_options", Status);
                goto end;
            }

            Status = con->SetCursorPosition(con, 0, rows - 1);
            if (EFI_ERROR(Status)) {
                print_error("SetCursorPosition", Status);
                return Status;
            }

            print(timeout_messagew);
            print_dec(timer);
        }

        /* The second parameter to CreateEvent was originally TPL_APPLICATION, but some old
         * EFIs ignore the specs and return EFI_INVALID_PARAMETER if you do this. */
        Status = systable->BootServices->CreateEvent(EVT_TIMER, TPL_CALLBACK, NULL, NULL, &evt);
        if (EFI_ERROR(Status)) {
            print_error("CreateEvent", Status);
            goto end;
        }

        Status = systable->BootServices->SetTimer(evt, TimerPeriodic, one_second);
        if (EFI_ERROR(Status)) {
            print_error("SetTimer", Status);
            goto end;
        }

        do {
            UINTN index;
            EFI_EVENT events[2];
            EFI_INPUT_KEY key;

            events[0] = evt;
            events[1] = systable->ConIn->WaitForKey;

            Status = systable->BootServices->WaitForEvent(2, events, &index);
            if (EFI_ERROR(Status)) {
                print_error("WaitForEvent", Status);
                goto end;
            }

            if (index == 0) { // timer
                timer--;

                if (gop_console) {
                    text_pos p;
                    char s[10];

                    p.x = timer_pos;
                    p.y = gop_info.VerticalResolution - (font_height * 3 / 4);

                    draw_rect(p.x, p.y - font_height, font_height * 5, font_height * 2, 0x000000);

                    dec_to_str(s, timer);
                    draw_text_ft(s, &p, 0x000000, 0xffffff);
                } else {
                    Status = con->SetCursorPosition(con, (sizeof(timeout_message) - sizeof(WCHAR)) / sizeof(WCHAR), rows - 1);
                    if (EFI_ERROR(Status)) {
                        print_error("SetCursorPosition", Status);
                        return Status;
                    }

                    Status = print_spaces(con, cols - (sizeof(timeout_message) - sizeof(WCHAR)) / sizeof(WCHAR) - 1);
                    if (EFI_ERROR(Status)) {
                        print_error("print_spaces", Status);
                        return Status;
                    }

                    Status = con->SetCursorPosition(con, (sizeof(timeout_message) - sizeof(WCHAR)) / sizeof(WCHAR), rows - 1);
                    if (EFI_ERROR(Status)) {
                        print_error("SetCursorPosition", Status);
                        return Status;
                    }

                    print_dec(timer);
                }

                if (timer == 0) {
                    Status = systable->BootServices->SetTimer(evt, TimerCancel, 0);
                    if (EFI_ERROR(Status)) {
                        print_error("SetTimer", Status);
                        goto end;
                    }

                    break;
                }
            } else { // key press
                unsigned int old_option = selected_option;

                if (!timer_cancelled) {
                    Status = systable->BootServices->SetTimer(evt, TimerCancel, 0);
                    if (EFI_ERROR(Status)) {
                        print_error("SetTimer", Status);
                        goto end;
                    }

                    timer_cancelled = true;

                    if (gop_console) {
                        draw_rect(font_height, gop_info.VerticalResolution - (font_height * 7 / 4),
                                  timer_pos + (font_height * 5), font_height * 2, 0x000000);
                    } else {
                        Status = con->SetCursorPosition(con, 0, rows - 1);
                        if (EFI_ERROR(Status)) {
                            print_error("SetCursorPosition", Status);
                            return Status;
                        }

                        Status = print_spaces(con, cols - 1);
                        if (EFI_ERROR(Status)) {
                            print_error("print_spaces", Status);
                            return Status;
                        }
                    }
                }

                Status = systable->ConIn->ReadKeyStroke(systable->ConIn, &key);
                if (EFI_ERROR(Status)) {
                    print_error("ReadKeyStroke", Status);
                    goto end;
                }

                if (key.UnicodeChar == 0xd || // enter
                    key.ScanCode == 3) { // right
                    break;
                }

                if (key.ScanCode == 2) { // down
                    selected_option++;

                    if (selected_option == num_options)
                        selected_option = 0;
                } else if (key.ScanCode == 1) { // up
                    if (selected_option == 0)
                        selected_option = num_options - 1;
                    else
                        selected_option--;
                } else if (key.ScanCode == 0x17) // escape
                    return EFI_ABORTED;

                if (key.ScanCode == 1 || key.ScanCode == 2) {
                    if (gop_console) {
                        draw_option_gop(old_option, options[old_option].name, false);
                        draw_option_gop(selected_option, options[selected_option].name, true);
                    } else {
                        Status = draw_options(con, cols);
                        if (EFI_ERROR(Status)) {
                            print_error("draw_options", Status);
                            goto end;
                        }
                    }
                }
            }
        } while (true);
    }

    *ret = &options[selected_option];

    if (gop_console) {
        memset(framebuffer, 0, gop_info.PixelsPerScanLine * gop_info.VerticalResolution * 4); // clear screen
        memset(shadow_fb, 0, gop_info.PixelsPerScanLine * gop_info.VerticalResolution * 4);

        console_pos.x = 0;
        console_pos.y = font_height;
    } else {
        Status = con->ClearScreen(con);
        if (EFI_ERROR(Status)) {
            print_error("ClearScreen", Status);
            goto end;
        }

        Status = con->SetCursorPosition(con, 0, 0);
        if (EFI_ERROR(Status)) {
            print_error("SetCursorPosition", Status);
            goto end;
        }
    }

    Status = EFI_SUCCESS;

end:
    if (cursor_visible && !gop_console)
        con->EnableCursor(con, true);

    return Status;
}
