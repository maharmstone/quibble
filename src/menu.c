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

#define VERSION L"Quibble " PROJECT_VERW
#define URL L"https://github.com/maharmstone/quibble"

boot_option* options = NULL;
unsigned int num_options, selected_option;

static const WCHAR timeout_message[] = L"Time until selected option is chosen: ";

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
                    print_error(L"AllocatePool", Status);
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
                print_error(L"AllocatePool", Status);
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
        print_error(L"AllocatePool", Status);
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

            Status = utf8_to_utf16(NULL, 0, &wlen, v->value, len);
            if (EFI_ERROR(Status)) {
                print_error(L"utf8_to_utf16", Status);
                return Status;
            }

            Status = systable->BootServices->AllocatePool(EfiLoaderData, wlen + sizeof(WCHAR), (void**)&opt->name);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePool", Status);
                return Status;
            }

            Status = utf8_to_utf16(opt->name, wlen, &wlen, v->value, len);
            if (EFI_ERROR(Status)) {
                print_error(L"utf8_to_utf16", Status);
                return Status;
            }

            opt->name[wlen / sizeof(WCHAR)] = 0;

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
                                print_error(L"AllocatePool", Status);
                                return Status;
                            }

                            memcpy(opt->system_path, v2->value, len + 1);
                        } else if (!stricmp(v2->name, "Options")) {
                            unsigned int len = strlen(v2->value);

                            Status = systable->BootServices->AllocatePool(EfiLoaderData, len + 1, (void**)&opt->options);
                            if (EFI_ERROR(Status)) {
                                print_error(L"AllocatePool", Status);
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
        print_error(L"OpenProtocol", Status);
        return Status;
    }

    if (!image->DeviceHandle)
        goto end2;

    Status = bs->OpenProtocol(image->DeviceHandle, &guid2, (void**)&fs, image_handle, NULL,
                              EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(Status)) {
        print_error(L"OpenProtocol", Status);
        goto end2;
    }

    Status = open_parent_dir(fs, (FILEPATH_DEVICE_PATH*)image->FilePath, &dir);
    if (EFI_ERROR(Status)) {
        print_error(L"open_parent_dir", Status);
        goto end;
    }

    Status = read_file(bs, dir, L"freeldr.ini", (void**)&data, &size);

    dir->Close(dir);

    if (EFI_ERROR(Status)) {
        print(L"Error opening freeldr.ini.\r\n");
        print_error(L"read_file", Status);
        goto end;
    }

    Status = parse_ini_file(data, &ini_sections);
    if (EFI_ERROR(Status)) {
        print_error(L"parse_ini_file", Status);
        goto end;
    }

    Status = populate_options_from_ini(&ini_sections, timeout);
    if (EFI_ERROR(Status)) {
        print_error(L"populate_options_from_ini", Status);
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
        print_error(L"AllocatePool", Status);
        return Status;
    }

    col = con->Mode->CursorColumn;
    row = con->Mode->CursorRow;

    for (unsigned int i = y; i < y + h; i++) {
        Status = con->SetCursorPosition(con, x, i);
        if (EFI_ERROR(Status)) {
            print_error(L"SetCursorPosition", Status);
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
            print_error(L"OutputString", Status);
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
        print_error(L"SetCursorPosition", Status);
        return Status;
    }

    if (selected) {
        Status = con->SetAttribute(con, EFI_BLACK | EFI_BACKGROUND_LIGHTGRAY);
        if (EFI_ERROR(Status)) {
            print_error(L"SetAttribute", Status);
            return Status;
        }
    }

    Status = bs->AllocatePool(EfiLoaderData, (width + 1) * sizeof(WCHAR), (void**)&s);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
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
        print_error(L"OutputString", Status);
        bs->FreePool(s);
        return Status;
    }

    if (selected) { // change back
        Status = con->SetAttribute(con, EFI_LIGHTGRAY | EFI_BACKGROUND_BLACK);
        if (EFI_ERROR(Status)) {
            print_error(L"SetAttribute", Status);
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
        Status = draw_option(con, i, cols - 3, options[i].name, i == selected_option);
        if (EFI_ERROR(Status)) {
            print_error(L"draw_option", Status);
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
            print_error(L"OutputString", Status);
            return Status;
        }
    }

    return EFI_SUCCESS;
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
        print_error(L"SetWatchdogTimer", Status);
        return Status;
    }

    Status = con->ClearScreen(con);
    if (EFI_ERROR(Status)) {
        print_error(L"ClearScreen", Status);
        return Status;
    }

    Status = con->SetCursorPosition(con, 0, 0);
    if (EFI_ERROR(Status)) {
        print_error(L"SetCursorPosition", Status);
        return Status;
    }

    Status = con->OutputString(con, VERSION L"\r\n");
    if (EFI_ERROR(Status)) {
        print_error(L"OutputString", Status);
        return Status;
    }

    Status = con->OutputString(con, URL L"\r\n");
    if (EFI_ERROR(Status)) {
        print_error(L"OutputString", Status);
        return Status;
    }

    Status = con->QueryMode(con, con->Mode->Mode, &cols, &rows);
    if (EFI_ERROR(Status)) {
        print_error(L"QueryMode", Status);
        return Status;
    }

    // FIXME - BCD support

    Status = load_ini_file(&timer);
    if (EFI_ERROR(Status)) {
        print_error(L"load_ini_file", Status);
        return Status;
    }

    if (num_options == 0) {
        print(L"No options found in INI file.\r\n");
        return EFI_ABORTED;
    }

    if (timer > 0) {
        if (cursor_visible)
            con->EnableCursor(con, false);

        Status = draw_box(con, 0, 2, cols - 1, rows - 3);
        if (EFI_ERROR(Status)) {
            print_error(L"draw_box", Status);
            goto end;
        }

        Status = draw_options(con, cols);
        if (EFI_ERROR(Status)) {
            print_error(L"draw_options", Status);
            goto end;
        }

        Status = con->SetCursorPosition(con, 0, rows - 1);
        if (EFI_ERROR(Status)) {
            print_error(L"SetCursorPosition", Status);
            return Status;
        }

        print((WCHAR*)timeout_message);
        print_dec(timer);

        /* The second parameter to CreateEvent was originally TPL_APPLICATION, but some old
         * EFIs ignore the specs and return EFI_INVALID_PARAMETER if you do this. */
        Status = systable->BootServices->CreateEvent(EVT_TIMER, TPL_CALLBACK, NULL, NULL, &evt);
        if (EFI_ERROR(Status)) {
            print_error(L"CreateEvent", Status);
            goto end;
        }

        Status = systable->BootServices->SetTimer(evt, TimerPeriodic, one_second);
        if (EFI_ERROR(Status)) {
            print_error(L"SetTimer", Status);
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
                print_error(L"WaitForEvent", Status);
                goto end;
            }

            if (index == 0) { // timer
                timer--;

                Status = con->SetCursorPosition(con, (sizeof(timeout_message) - sizeof(WCHAR)) / sizeof(WCHAR), rows - 1);
                if (EFI_ERROR(Status)) {
                    print_error(L"SetCursorPosition", Status);
                    return Status;
                }

                Status = print_spaces(con, cols - (sizeof(timeout_message) - sizeof(WCHAR)) / sizeof(WCHAR) - 1);
                if (EFI_ERROR(Status)) {
                    print_error(L"print_spaces", Status);
                    return Status;
                }

                Status = con->SetCursorPosition(con, (sizeof(timeout_message) - sizeof(WCHAR)) / sizeof(WCHAR), rows - 1);
                if (EFI_ERROR(Status)) {
                    print_error(L"SetCursorPosition", Status);
                    return Status;
                }

                print_dec(timer);

                if (timer == 0) {
                    Status = systable->BootServices->SetTimer(evt, TimerCancel, 0);
                    if (EFI_ERROR(Status)) {
                        print_error(L"SetTimer", Status);
                        goto end;
                    }

                    break;
                }
            } else { // key press
                if (!timer_cancelled) {
                    Status = systable->BootServices->SetTimer(evt, TimerCancel, 0);
                    if (EFI_ERROR(Status)) {
                        print_error(L"SetTimer", Status);
                        goto end;
                    }

                    timer_cancelled = true;

                    Status = con->SetCursorPosition(con, 0, rows - 1);
                    if (EFI_ERROR(Status)) {
                        print_error(L"SetCursorPosition", Status);
                        return Status;
                    }

                    Status = print_spaces(con, cols - 1);
                    if (EFI_ERROR(Status)) {
                        print_error(L"print_spaces", Status);
                        return Status;
                    }
                }

                Status = systable->ConIn->ReadKeyStroke(systable->ConIn, &key);
                if (EFI_ERROR(Status)) {
                    print_error(L"ReadKeyStroke", Status);
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

                    Status = draw_options(con, cols);
                    if (EFI_ERROR(Status)) {
                        print_error(L"draw_options", Status);
                        goto end;
                    }
                } else if (key.ScanCode == 1) { // up
                    if (selected_option == 0)
                        selected_option = num_options - 1;
                    else
                        selected_option--;

                    Status = draw_options(con, cols);
                    if (EFI_ERROR(Status)) {
                        print_error(L"draw_options", Status);
                        goto end;
                    }
                } else if (key.ScanCode == 0x17) // escape
                    return EFI_ABORTED;
            }
        } while (true);
    }

    *ret = &options[selected_option];

    Status = con->ClearScreen(con);
    if (EFI_ERROR(Status)) {
        print_error(L"ClearScreen", Status);
        goto end;
    }

    Status = con->SetCursorPosition(con, 0, 0);
    if (EFI_ERROR(Status)) {
        print_error(L"SetCursorPosition", Status);
        goto end;
    }

    Status = EFI_SUCCESS;

end:
    if (cursor_visible)
        con->EnableCursor(con, true);

    return Status;
}
