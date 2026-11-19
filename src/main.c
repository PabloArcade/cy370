/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cy370dump: command line, text buffers, the refusal exit.

   Copyright (C) DARKSOFT

   This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
   version.  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
   implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
   details.  You should have received a copy of the GNU General Public License along with this program.  If not, see
   <https://www.gnu.org/licenses/>. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "cy370.h"

sb cy_refusal;
static jmp_buf on_refusal;

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("REFUSED: OUT_OF_MEMORY\n", stdout); exit(1); }
    return p;
}

void sb_add(sb *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *p;
        while (cap < b->n + n + 1) cap *= 2;
        p = realloc(b->p, cap);
        if (!p) { fputs("REFUSED: OUT_OF_MEMORY\n", stdout); exit(1); }
        b->p = p; b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void sb_puts(sb *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_vprintf(sb *b, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    char small[256];
    va_copy(ap2, ap);
    n = vsnprintf(small, sizeof small, fmt, ap);
    if (n < 0) n = 0;
    if ((size_t)n < sizeof small) sb_add(b, small, (size_t)n);
    else {
        char *big = xmalloc((size_t)n + 1);
        vsnprintf(big, (size_t)n + 1, fmt, ap2);
        sb_add(b, big, (size_t)n);
        free(big);
    }
    va_end(ap2);
}

void sb_printf(sb *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

void sb_latin1(sb *b, const unsigned char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char two[2];
        if (s[i] < 0x80) { two[0] = (char)s[i]; sb_add(b, two, 1); }
        else { two[0] = (char)(0xc0 | (s[i] >> 6)); two[1] = (char)(0x80 | (s[i] & 0x3f)); sb_add(b, two, 2); }
    }
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xsprintf(const char *fmt, ...)
{
    sb b = {0, 0, 0};
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    if (!b.p) sb_add(&b, "", 0);
    return b.p;
}

void refuse_sb(void) { longjmp(on_refusal, 1); }

void refuse(const char *fmt, ...)
{
    va_list ap;
    cy_refusal.n = 0;
    va_start(ap, fmt);
    sb_vprintf(&cy_refusal, fmt, ap);
    va_end(ap);
    longjmp(on_refusal, 1);
}

static void usage(void)
{
    fputs("cy370 decoder tool by Darksoft v" CY_VERSION ". Use at your own risk.\n"
          "\n"
          "Commands:\n"
          "  cy370dump devices\n"
          "      list every supported part and package with its order codes\n"
          "  cy370dump decode <file.jed> --viewlist\n"
          "      list the packages the part named in the fuse file can be decoded for\n"
          "  cy370dump decode <file.jed> <out.vhd> [--top NAME] [--package P] [--pinmap FILE]\n"
          "      write the recovered VHDL design, every signal fixed to its package pin\n"
          "      --top NAME      entity name (default: design)\n"
          "      --package P     package code (A, G, J, U, Y); default: the package the fuse file names\n"
          "      --pinmap FILE   also write \"<package pin> <I/O-cell label>\" per pin\n", stdout);
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    size_t cap = 1 << 16, n = 0, r;
    unsigned char *buf;
    if (!f) return NULL;
    buf = xmalloc(cap);
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            unsigned char *p = realloc(buf, cap * 2);
            if (!p) { fclose(f); free(buf); return NULL; }
            buf = p; cap *= 2;
        }
    }
    if (ferror(f)) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = n;
    return buf;
}

static int write_file(const char *path, const sb *b)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (b->n && fwrite(b->p, 1, b->n, f) != b->n) { fclose(f); return 0; }
    return fclose(f) == 0;
}

static int pk_cmp(const void *a, const void *b)
{
    return strcmp((*(const cy_package *const *)a)->code, (*(const cy_package *const *)b)->code);
}

/* the packages of a part in order of their code letters */
static int sorted_packages(const cy_device *d, const cy_package **out)
{
    int n = 0;
    const cy_package *p;
    for (p = d->packages; p->code; p++) out[n++] = p;
    qsort(out, (size_t)n, sizeof *out, pk_cmp);
    return n;
}

static void print_codes(const cy_package *p)
{
    int i;
    for (i = 0; p->ordercodes[i]; i++) printf("%s%s", i ? " " : "", p->ordercodes[i]);
    fputs("\n", stdout);
}

static int cmd_devices(void)
{
    int i, k, n;
    for (i = 0; i < 10; i++) {
        const cy_package *pk[8];
        n = sorted_packages(&cy_devices[i], pk);
        for (k = 0; k < n; k++) {
            printf("%-6s %s  %-9s ", cy_devices[i].name, pk[k]->code, pk[k]->status);
            print_codes(pk[k]);
        }
    }
    return 0;
}

static int cmd_viewlist(const char *path)
{
    size_t len = 0;
    unsigned char *data = read_file(path, &len);
    cy_jedec j;
    const cy_device *d;
    const cy_package *pk[8];
    int k, n;
    if (!data) { printf("REFUSED: CANNOT_READ %s\n", path); return 1; }
    jedec_parse(data, len, &j);
    d = device_by_name(j.design, j.design_len);
    if (!d) {
        /* the name is shown in lower case */
        sb m = {0, 0, 0};
        size_t i;
        for (i = 0; i < j.design_len; i++) {
            unsigned char c = j.design[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) c = (unsigned char)(c + 32);
            sb_latin1(&m, &c, 1);
        }
        fputs("REFUSED: DEVICE_NOT_SUPPORTED ", stdout);
        if (m.n) fwrite(m.p, 1, m.n, stdout);
        fputs("\n", stdout);
        return 1;
    }
    n = sorted_packages(d, pk);
    for (k = 0; k < n; k++) {
        printf("%s %s %s ", d->name, pk[k]->code, pk[k]->status);
        print_codes(pk[k]);
    }
    return 0;
}

static int cmd_decode(const char *jed, const char *out, const char *top, const char *package, const char *pinmap)
{
    size_t len = 0;
    unsigned char *data = read_file(jed, &len);
    cy_result r;
    memset(&r, 0, sizeof r);
    if (!data) { printf("REFUSED: CANNOT_READ %s\n", jed); return 1; }
    decode(data, len, top, package, &r);
    if (!write_file(out, &r.text)) { printf("REFUSED: CANNOT_WRITE %s\n", out); return 1; }
    if (pinmap && !write_file(pinmap, &r.pinmap)) { printf("REFUSED: CANNOT_WRITE %s\n", pinmap); return 1; }
    printf("decoded %s: %d macrocells, %d ports\n", jed, r.macrocells, r.ports);
    if (r.report.n) fwrite(r.report.p, 1, r.report.n, stdout);
    return 0;
}

int main(int argc, char **argv)
{
    volatile int rc = 2;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);       /* line ends are LF on every system */
#endif
    if (argc < 2) { usage(); return 0; }
    if (setjmp(on_refusal)) {
        sb u = {0, 0, 0};
        fputs("REFUSED: ", stdout);
        if (cy_refusal.n) fwrite(cy_refusal.p, 1, cy_refusal.n, stdout);
        fputs("\n", stdout);
        used_lines(&u);
        if (u.n) fwrite(u.p, 1, u.n, stdout);
        fflush(stdout);
        return 1;
    }
    if (strcmp(argv[1], "devices") == 0 && argc == 2) {
        rc = cmd_devices();
    } else if (strcmp(argv[1], "decode") == 0 && argc == 4 && strcmp(argv[3], "--viewlist") == 0) {
        rc = cmd_viewlist(argv[2]);
    } else if (strcmp(argv[1], "decode") == 0 && argc >= 4 && argc % 2 == 0) {
        const char *top = "design", *package = NULL, *pinmap = NULL;
        int i, ok = 1;
        for (i = 4; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--top") == 0) top = argv[i + 1];
            else if (strcmp(argv[i], "--package") == 0) package = argv[i + 1];
            else if (strcmp(argv[i], "--pinmap") == 0) pinmap = argv[i + 1];
            else ok = 0;
        }
        if (ok) rc = cmd_decode(argv[2], argv[3], top, package, pinmap);
        else usage();
    } else {
        usage();
    }
    fflush(stdout);
    return rc;
}
