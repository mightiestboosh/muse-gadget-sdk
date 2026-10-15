/* SPDX-License-Identifier: Apache-2.0 */
#include "muse_hid_text.h"
#include <string.h>

bool muse_hid_ascii(uint8_t c, uint8_t r[8])
{
    memset(r, 0, 8);
    if (c >= 'a' && c <= 'z') r[2] = 4 + c - 'a';
    else if (c >= 'A' && c <= 'Z') { r[0] = 2; r[2] = 4 + c - 'A'; }
    else if (c >= '1' && c <= '9') r[2] = 30 + c - '1';
    else if (c == '0') r[2] = 39;
    else if (c == ' ') r[2] = 44;
    else {
        static const char plain[] = "-=[]\\;\'`,./";
        static const char shift[] = "_+{}|:\"~<>?";
        static const uint8_t codes[] = {45,46,47,48,49,51,52,53,54,55,56};
        static const char digits[] = "!@#$%^&*()";
        const char *p = c ? strchr(plain, c) : NULL;
        const char *q = c ? strchr(shift, c) : NULL;
        const char *d = c ? strchr(digits, c) : NULL;
        if (p) r[2] = codes[p - plain];
        else if (q) { r[0] = 2; r[2] = codes[q - shift]; }
        else if (d) { r[0] = 2; r[2] = 30 + (d - digits); }
        else return false;
    }
    return true;
}

size_t muse_hid_normalize(const char *s, size_t left, char out[3], size_t *used)
{
    *used = 0;
    if (!left) return 0;
    uint8_t c = (uint8_t)s[0];
    *used = 1;
    if (c >= 32 && c <= 126) { out[0] = c; return 1; }
    if (c == '\n' || c == '\r' || c == '\t') { out[0] = ' '; return 1; }
    size_t n = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
    if (n > left) { *used = left; return 0; }
    for (size_t i = 1; i < n; i++) if (((uint8_t)s[i] & 0xc0) != 0x80) return 0;
    *used = n;
    if (n == 3 && c == 0xe2 && (uint8_t)s[1] == 0x80) {
        uint8_t end = s[2];
        if (end == 0x98 || end == 0x99) { out[0] = '\''; return 1; }
        if (end == 0x9c || end == 0x9d) { out[0] = '"'; return 1; }
        if (end >= 0x90 && end <= 0x95) { out[0] = '-'; return 1; }
        if (end == 0xa6) { memcpy(out, "...", 3); return 3; }
    }
    return 0;
}
