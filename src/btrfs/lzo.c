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

// This LZO compression code comes from v0.22 of lzo, written way back in
// 1996, and available here:
// https://www.ibiblio.org/pub/historic-linux/ftp-archives/sunsite.unc.edu/Sep-29-1996/libs/lzo-0.22.tar.gz
// Modern versions of lzo are licensed under the GPL, but the very oldest
// versions are under the LGPL and hence okay to use here.

#include <efibind.h>
#include <efidef.h>
#include <efilink.h>
#include <efierr.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../misc.h"

#define LZO_PAGE_SIZE 4096

typedef struct {
    uint8_t* in;
    uint32_t inlen;
    uint32_t inpos;
    uint8_t* out;
    uint32_t outlen;
    uint32_t outpos;
    bool error;
    void* wrkmem;
} lzo_stream;

void do_print(const char* s);
void do_print_error(const char* func, EFI_STATUS Status);

static uint8_t lzo_nextbyte(lzo_stream* stream) {
    uint8_t c;

    if (stream->inpos >= stream->inlen) {
        stream->error = true;
        return 0;
    }

    c = stream->in[stream->inpos];
    stream->inpos++;

    return c;
}

static int lzo_len(lzo_stream* stream, int byte, int mask) {
    int len = byte & mask;

    if (len == 0) {
        while (!(byte = lzo_nextbyte(stream))) {
            if (stream->error) return 0;

            len += 255;
        }

        len += mask + byte;
    }

    return len;
}

static void lzo_copy(lzo_stream* stream, int len) {
    if (stream->inpos + len > stream->inlen) {
        stream->error = true;
        return;
    }

    if (stream->outpos + len > stream->outlen) {
        stream->error = true;
        return;
    }

    do {
        stream->out[stream->outpos] = stream->in[stream->inpos];
        stream->inpos++;
        stream->outpos++;
        len--;
    } while (len > 0);
}

static void lzo_copyback(lzo_stream* stream, uint32_t back, int len) {
    if (stream->outpos < back) {
        stream->error = true;
        return;
    }

    if (stream->outpos + len > stream->outlen) {
        stream->error = true;
        return;
    }

    do {
        stream->out[stream->outpos] = stream->out[stream->outpos - back];
        stream->outpos++;
        len--;
    } while (len > 0);
}

static __inline unsigned int min(unsigned int a, unsigned int b) {
    if (a < b)
        return a;
    else
        return b;
}

static EFI_STATUS do_lzo_decompress(lzo_stream* stream) {
    uint8_t byte;
    uint32_t len, back;
    bool backcopy = false;

    stream->error = false;

    byte = lzo_nextbyte(stream);
    if (stream->error) return EFI_INVALID_PARAMETER;

    if (byte > 17) {
        lzo_copy(stream, min((uint8_t)(byte - 17), (uint32_t)(stream->outlen - stream->outpos)));
        if (stream->error) return EFI_INVALID_PARAMETER;

        if (stream->outlen == stream->outpos)
            return EFI_SUCCESS;

        byte = lzo_nextbyte(stream);
        if (stream->error) return EFI_INVALID_PARAMETER;

        if (byte < 16) return EFI_INVALID_PARAMETER;
    }

    while (1) {
        if (byte >> 4) {
            backcopy = true;
            if (byte >> 6) {
                len = (byte >> 5) - 1;
                back = (lzo_nextbyte(stream) << 3) + ((byte >> 2) & 7) + 1;
                if (stream->error) return EFI_INVALID_PARAMETER;
            } else if (byte >> 5) {
                len = lzo_len(stream, byte, 31);
                if (stream->error) return EFI_INVALID_PARAMETER;

                byte = lzo_nextbyte(stream);
                if (stream->error) return EFI_INVALID_PARAMETER;

                back = (lzo_nextbyte(stream) << 6) + (byte >> 2) + 1;
                if (stream->error) return EFI_INVALID_PARAMETER;
            } else {
                len = lzo_len(stream, byte, 7);
                if (stream->error) return EFI_INVALID_PARAMETER;

                back = (1 << 14) + ((byte & 8) << 11);

                byte = lzo_nextbyte(stream);
                if (stream->error) return EFI_INVALID_PARAMETER;

                back += (lzo_nextbyte(stream) << 6) + (byte >> 2);
                if (stream->error) return EFI_INVALID_PARAMETER;

                if (back == (1 << 14)) {
                    if (len != 1)
                        return EFI_INVALID_PARAMETER;
                    break;
                }
            }
        } else if (backcopy) {
            len = 0;
            back = (lzo_nextbyte(stream) << 2) + (byte >> 2) + 1;
            if (stream->error) return EFI_INVALID_PARAMETER;
        } else {
            len = lzo_len(stream, byte, 15);
            if (stream->error) return EFI_INVALID_PARAMETER;

            lzo_copy(stream, min(len + 3, stream->outlen - stream->outpos));
            if (stream->error) return EFI_INVALID_PARAMETER;

            if (stream->outlen == stream->outpos)
                return EFI_SUCCESS;

            byte = lzo_nextbyte(stream);
            if (stream->error) return EFI_INVALID_PARAMETER;

            if (byte >> 4)
                continue;

            len = 1;
            back = (1 << 11) + (lzo_nextbyte(stream) << 2) + (byte >> 2) + 1;
            if (stream->error) return EFI_INVALID_PARAMETER;

            break;
        }

        lzo_copyback(stream, back, min(len + 2, stream->outlen - stream->outpos));
        if (stream->error) return EFI_INVALID_PARAMETER;

        if (stream->outlen == stream->outpos)
            return EFI_SUCCESS;

        len = byte & 3;

        if (len) {
            lzo_copy(stream, min(len, stream->outlen - stream->outpos));
            if (stream->error) return EFI_INVALID_PARAMETER;

            if (stream->outlen == stream->outpos)
                return EFI_SUCCESS;
        } else
            backcopy = !backcopy;

        byte = lzo_nextbyte(stream);
        if (stream->error) return EFI_INVALID_PARAMETER;
    }

    return EFI_SUCCESS;
}

EFI_STATUS lzo_decompress(uint8_t* inbuf, uint32_t inlen, uint8_t* outbuf, uint32_t outlen, uint32_t inpageoff) {
    EFI_STATUS Status;
    uint32_t partlen, inoff, outoff;
    lzo_stream stream;

    inoff = 0;
    outoff = 0;

    do {
        partlen = *(uint32_t*)&inbuf[inoff];

        if (partlen + inoff > inlen) {
            char s[255], *p;

            p = stpcpy(s, "overflow: ");
            p = dec_to_str(p, partlen);
            p = stpcpy(p, " + ");
            p = dec_to_str(p, inoff);
            p = stpcpy(p," > ");
            p = dec_to_str(p, inlen);
            p = stpcpy(p, "\n");

            do_print(s);
            return EFI_INVALID_PARAMETER;
        }

        inoff += sizeof(uint32_t);

        stream.in = &inbuf[inoff];
        stream.inlen = partlen;
        stream.inpos = 0;
        stream.out = &outbuf[outoff];
        stream.outlen = min(outlen, LZO_PAGE_SIZE);
        stream.outpos = 0;

        Status = do_lzo_decompress(&stream);
        if (EFI_ERROR(Status)) {
            do_print_error("do_lzo_decompress", Status);
            return Status;
        }

        if (stream.outpos < stream.outlen)
            memset(&stream.out[stream.outpos], 0, stream.outlen - stream.outpos);

        inoff += partlen;
        outoff += stream.outlen;

        if (LZO_PAGE_SIZE - ((inpageoff + inoff) % LZO_PAGE_SIZE) < sizeof(uint32_t))
            inoff = ((((inpageoff + inoff) / LZO_PAGE_SIZE) + 1) * LZO_PAGE_SIZE) - inpageoff;

        outlen -= stream.outlen;
    } while (inoff < inlen && outlen > 0);

    return EFI_SUCCESS;
}
