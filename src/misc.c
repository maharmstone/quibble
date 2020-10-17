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

#include "misc.h"
#include <stdbool.h>

void wcsncpy(WCHAR* dest, const WCHAR* src, size_t n) {
    size_t i = 0;

    while (src[i] != 0) {
        if (i >= n) {
            dest[n] = 0;
            return;
        }

        dest[i] = src[i];
        i++;
    }

    dest[i] = 0;
}

void wcsncat(WCHAR* dest, const WCHAR* src, size_t n) {
    size_t i = 0;

    while (dest[i] != 0) {
        if (n == 0)
            return;

        dest++;
        n--;
    }

    wcsncpy(dest, src, n);
}

size_t wcslen(const WCHAR* s) {
    size_t i = 0;

    while (s[i] != 0) {
        i++;
    }

    return i;
}

#ifdef DEBUG_TO_VAR
#define EFI_QUIBBLE_DEBUG_GUID { 0x94C55CBE, 0xD4B9, 0x43B1, { 0xB0, 0xBE, 0xA0, 0x25, 0x4B, 0xAF, 0x7B, 0x09 } }

void print(const WCHAR* s) {
    EFI_GUID guid = EFI_QUIBBLE_DEBUG_GUID;

    systable->ConOut->OutputString(systable->ConOut, (CHAR16*)s);

    systable->RuntimeServices->SetVariable(L"debug", &guid,
                                           EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_APPEND_WRITE,
                                           wcslen(s) * sizeof(CHAR16), (WCHAR*)s);
}
#endif

size_t strlen(const char* s) {
    size_t i = 0;

    while (s[i] != 0) {
        i++;
    }

    return i;
}

int wcsicmp(const WCHAR* s1, const WCHAR* s2) {
    size_t i = 0;

    while (true) {
        WCHAR c1 = s1[i];
        WCHAR c2 = s2[i];

        if (c1 == 0 && c2 == 0)
            return 0;
        else if (c1 == 0)
            return -1;
        else if (c2 == 0)
            return 1;

        if (c1 >= 'A' && c1 <= 'Z')
            c1 = c1 - 'A' + 'a';

        if (c2 >= 'A' && c2 <= 'Z')
            c2 = c2 - 'A' + 'a';

        if (c1 != c2)
            return c1 > c2 ? 1 : -1;

        i++;
    }
}

int stricmp(const char* s1, const char* s2) {
    size_t i = 0;

    while (true) {
        char c1 = s1[i];
        char c2 = s2[i];

        if (c1 == 0 && c2 == 0)
            return 0;
        else if (c1 == 0)
            return -1;
        else if (c2 == 0)
            return 1;

        if (c1 >= 'A' && c1 <= 'Z')
            c1 = c1 - 'A' + 'a';

        if (c2 >= 'A' && c2 <= 'Z')
            c2 = c2 - 'A' + 'a';

        if (c1 != c2)
            return c1 > c2 ? 1 : -1;

        i++;
    }
}

int strnicmp(const char* s1, const char* s2, int n) {
    for (int i = 0; i < n; i++) {
        char c1 = s1[i];
        char c2 = s2[i];

        if (c1 == 0 && c2 == 0)
            return 0;
        else if (c1 == 0)
            return -1;
        else if (c2 == 0)
            return 1;

        if (c1 >= 'A' && c1 <= 'Z')
            c1 = c1 - 'A' + 'a';

        if (c2 >= 'A' && c2 <= 'Z')
            c2 = c2 - 'A' + 'a';

        if (c1 != c2)
            return c1 > c2 ? 1 : -1;

        i++;
    }

    return 0;
}

int strcmp(const char* s1, const char* s2) {
    size_t i = 0;

    while (true) {
        char c1 = s1[i];
        char c2 = s2[i];

        if (c1 == 0 && c2 == 0)
            return 0;
        else if (c1 == 0)
            return -1;
        else if (c2 == 0)
            return 1;

        if (c1 != c2)
            return c1 > c2 ? 1 : -1;

        i++;
    }
}

int memcmp(const void* s1, const void* s2, size_t n) {
#if __INTPTR_WIDTH__ == 64
    while (n > sizeof(uint64_t)) {
        uint64_t c1 = *(uint64_t*)s1;
        uint64_t c2 = *(uint64_t*)s2;

        if (c1 != c2)
            return c1 > c2 ? 1 : -1;

        s1 = (uint64_t*)s1 + 1;
        s2 = (uint64_t*)s2 + 1;
        n -= sizeof(uint64_t);
    }
#endif

    while (n > sizeof(uint32_t)) {
        uint32_t c1 = *(uint32_t*)s1;
        uint32_t c2 = *(uint32_t*)s2;

        if (c1 != c2)
            return c1 > c2 ? 1 : -1;

        s1 = (uint32_t*)s1 + 1;
        s2 = (uint32_t*)s2 + 1;
        n -= sizeof(uint32_t);
    }

    while (n > 0) {
        uint8_t c1 = *(uint8_t*)s1;
        uint8_t c2 = *(uint8_t*)s2;

        if (c1 != c2)
            return c1 > c2 ? 1 : -1;

        s1 = (uint8_t*)s1 + 1;
        s2 = (uint8_t*)s2 + 1;
        n--;
    }

    return 0;
}

void memcpy(void* dest, const void* src, size_t n) {
#if __INTPTR_WIDTH__ == 64
    while (n >= sizeof(uint64_t)) {
        *(uint64_t*)dest = *(uint64_t*)src;

        dest = (uint8_t*)dest + sizeof(uint64_t);
        src = (uint8_t*)src + sizeof(uint64_t);

        n -= sizeof(uint64_t);
    }
#endif

    while (n >= sizeof(uint32_t)) {
        *(uint32_t*)dest = *(uint32_t*)src;

        dest = (uint8_t*)dest + sizeof(uint32_t);
        src = (uint8_t*)src + sizeof(uint32_t);

        n -= sizeof(uint32_t);
    }

    while (n >= sizeof(uint16_t)) {
        *(uint16_t*)dest = *(uint16_t*)src;

        dest = (uint8_t*)dest + sizeof(uint16_t);
        src = (uint8_t*)src + sizeof(uint16_t);

        n -= sizeof(uint16_t);
    }

    while (n >= sizeof(uint8_t)) {
        *(uint8_t*)dest = *(uint8_t*)src;

        dest = (uint8_t*)dest + sizeof(uint8_t);
        src = (uint8_t*)src + sizeof(uint8_t);

        n -= sizeof(uint8_t);
    }
}

void* memset(void* s, int c, size_t n) {
    void* orig_s = s;
    uint32_t v;

    // FIXME - use uint64_t instead if 64-bit CPU
    // FIXME - faster if we make sure we're aligned (also in memcpy)?

    v = 0;

    for (unsigned int i = 0; i < sizeof(uint32_t); i++) {
        v |= c & 0xff;
        v <<= 8;
    }

    while (n >= sizeof(uint32_t)) {
        *(uint32_t*)s = v;

        s = (uint8_t*)s + sizeof(uint32_t);
        n -= sizeof(uint32_t);
    }

    while (n > 0) {
        *(uint8_t*)s = c;

        s = (uint8_t*)s + 1;
        n--;
    }

    return orig_s;
}

void strcpy(char* dest, const char* src) {
    while (*src != 0) {
        *dest = *src;
        src++;
        dest++;
    }

    *dest = 0;
}

static WCHAR* error_string(EFI_STATUS Status) {
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
    wcsncat(s, error_string(Status), sizeof(s) / sizeof(WCHAR));
    wcsncat(s, L"\r\n", sizeof(s) / sizeof(WCHAR));

    systable->ConOut->OutputString(systable->ConOut, s);
}

void print_hex(uint64_t v) {
    WCHAR s[17], *p;

    if (v == 0) {
        print(L"0");
        return;
    }

    s[16] = 0;
    p = &s[16];

    while (v != 0) {
        p = &p[-1];

        if ((v & 0xf) >= 10)
            *p = (v & 0xf) - 10 + 'a';
        else
            *p = (v & 0xf) + '0';

        v >>= 4;
    }

    print(p);
}

void print_dec(uint32_t v) {
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

void itow(int v, WCHAR* w) {
    WCHAR s[12], *p;
    bool neg = false;

    if (v == 0) {
        s[0] = '0';
        s[1] = 0;
        return;
    }

    if (v < 0) {
        neg = true;
        v = -v;
    }

    s[11] = 0;
    p = &s[11];

    while (v != 0) {
        p = &p[-1];

        *p = (v % 10) + '0';

        v /= 10;
    }

    if (neg) {
        p = &p[-1];
        *p = '-';
    }

    do {
        *w = *p;
        w++;
        p++;
    } while (*p);

    *w = 0;
}

void print_string(const char* s) {
    WCHAR w[255], *t;

    // FIXME - make sure no overflow

    t = w;

    while (*s) {
        *t = *s;
        s++;
        t++;
    }

    *t = 0;

    print(w);
}

EFI_STATUS utf8_to_utf16(WCHAR* dest, unsigned int dest_max, unsigned int* dest_len, const char* src, unsigned int src_len) {
    EFI_STATUS Status = EFI_SUCCESS;
    uint8_t* in = (uint8_t*)src;
    uint16_t* out = (uint16_t*)dest;
    unsigned int needed = 0, left = dest_max / sizeof(uint16_t);

    for (unsigned int i = 0; i < src_len; i++) {
        uint32_t cp;

        if (!(in[i] & 0x80))
            cp = in[i];
        else if ((in[i] & 0xe0) == 0xc0) {
            if (i == src_len - 1 || (in[i+1] & 0xc0) != 0x80) {
                cp = 0xfffd;
                Status = EFI_INVALID_PARAMETER;
            } else {
                cp = ((in[i] & 0x1f) << 6) | (in[i+1] & 0x3f);
                i++;
            }
        } else if ((in[i] & 0xf0) == 0xe0) {
            if (i >= src_len - 2 || (in[i+1] & 0xc0) != 0x80 || (in[i+2] & 0xc0) != 0x80) {
                cp = 0xfffd;
                Status = EFI_INVALID_PARAMETER;
            } else {
                cp = ((in[i] & 0xf) << 12) | ((in[i+1] & 0x3f) << 6) | (in[i+2] & 0x3f);
                i += 2;
            }
        } else if ((in[i] & 0xf8) == 0xf0) {
            if (i >= src_len - 3 || (in[i+1] & 0xc0) != 0x80 || (in[i+2] & 0xc0) != 0x80 || (in[i+3] & 0xc0) != 0x80) {
                cp = 0xfffd;
                Status = EFI_INVALID_PARAMETER;
            } else {
                cp = ((in[i] & 0x7) << 18) | ((in[i+1] & 0x3f) << 12) | ((in[i+2] & 0x3f) << 6) | (in[i+3] & 0x3f);
                i += 3;
            }
        } else {
            cp = 0xfffd;
            Status = EFI_INVALID_PARAMETER;
        }

        if (cp > 0x10ffff) {
            cp = 0xfffd;
            Status = EFI_INVALID_PARAMETER;
        }

        if (dest) {
            if (cp <= 0xffff) {
                if (left < 1)
                    return EFI_BUFFER_TOO_SMALL;

                *out = (uint16_t)cp;
                out++;

                left--;
            } else {
                if (left < 2)
                    return EFI_BUFFER_TOO_SMALL;

                cp -= 0x10000;

                *out = 0xd800 | ((cp & 0xffc00) >> 10);
                out++;

                *out = 0xdc00 | (cp & 0x3ff);
                out++;

                left -= 2;
            }
        }

        if (cp <= 0xffff)
            needed += sizeof(uint16_t);
        else
            needed += 2 * sizeof(uint16_t);
    }

    if (dest_len)
        *dest_len = needed;

    return Status;
}

EFI_STATUS utf16_to_utf8(char* dest, unsigned int dest_max, unsigned int* dest_len, const WCHAR* src, unsigned int src_len) {
    EFI_STATUS Status = EFI_SUCCESS;
    uint16_t* in = (uint16_t*)src;
    uint8_t* out = (uint8_t*)dest;
    unsigned int in_len = src_len / sizeof(uint16_t);
    unsigned int needed = 0, left = dest_max;

    for (unsigned int i = 0; i < in_len; i++) {
        uint32_t cp = *in;
        in++;

        if ((cp & 0xfc00) == 0xd800) {
            if (i == in_len - 1 || (*in & 0xfc00) != 0xdc00) {
                cp = 0xfffd;
                Status = EFI_INVALID_PARAMETER;
            } else {
                cp = (cp & 0x3ff) << 10;
                cp |= *in & 0x3ff;
                cp += 0x10000;

                in++;
                i++;
            }
        } else if ((cp & 0xfc00) == 0xdc00) {
            cp = 0xfffd;
            Status = EFI_INVALID_PARAMETER;
        }

        if (cp > 0x10ffff) {
            cp = 0xfffd;
            Status = EFI_INVALID_PARAMETER;
        }

        if (dest) {
            if (cp < 0x80) {
                if (left < 1)
                    return EFI_BUFFER_TOO_SMALL;

                *out = (uint8_t)cp;
                out++;

                left--;
            } else if (cp < 0x800) {
                if (left < 2)
                    return EFI_BUFFER_TOO_SMALL;

                *out = 0xc0 | ((cp & 0x7c0) >> 6);
                out++;

                *out = 0x80 | (cp & 0x3f);
                out++;

                left -= 2;
            } else if (cp < 0x10000) {
                if (left < 3)
                    return EFI_BUFFER_TOO_SMALL;

                *out = 0xe0 | ((cp & 0xf000) >> 12);
                out++;

                *out = 0x80 | ((cp & 0xfc0) >> 6);
                out++;

                *out = 0x80 | (cp & 0x3f);
                out++;

                left -= 3;
            } else {
                if (left < 4)
                    return EFI_BUFFER_TOO_SMALL;

                *out = 0xf0 | ((cp & 0x1c0000) >> 18);
                out++;

                *out = 0x80 | ((cp & 0x3f000) >> 12);
                out++;

                *out = 0x80 | ((cp & 0xfc0) >> 6);
                out++;

                *out = 0x80 | (cp & 0x3f);
                out++;

                left -= 4;
            }
        }

        if (cp < 0x80)
            needed++;
        else if (cp < 0x800)
            needed += 2;
        else if (cp < 0x10000)
            needed += 3;
        else
            needed += 4;
    }

    if (dest_len)
        *dest_len = needed;

    return Status;
}
