/* SPDX-License-Identifier: GPL-3.0-or-later */
/* JEDEC fuse file reader for the files Cypress Warp writes for the FLASH370 family.

   Layout: free header text, STX (0x02), fields each ended by '*', ETX (0x03), four hex digits of transmission checksum.
   The first field is the device name.  The other fields are told apart by their first non-blank character:
     Q   QF<fuses> / QP<pins> / QV<vectors>     F   default fuse state     G   security fuse     N   note
     L   L<address> <0/1 digits>                 (binary form; blanks between digits are ignored)
     K   K<address> <hex digits>                 (hex form: most significant bit = lowest fuse; a record runs up to the
                                                 next record's address, its last digit padded with zero bits)
     C   fuse checksum, four hex digits
   Fuse checksum: the fuses taken eight at a time as bytes (fuse 8w = bit 0), summed modulo 65536.
   Transmission checksum, binary form: the bytes STX .. ETX summed modulo 65536.
   Transmission checksum, hex form: 0x17 per line end between STX and ETX, modulo 65536; every line end must be CR LF.

   Text is handled as Latin-1; "blank" means the white space of that range: 0x09 .. 0x0d, 0x1c .. 0x20, 0x85 and 0xa0.
   Any defect is refused by name. */
#include <stdlib.h>
#include <string.h>
#include "cy370.h"

#define DENSE_LIMIT 64000000L       /* largest fuse count held as an array; larger counts are followed by address only */

int is_uspace(unsigned c)
{
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0;
}

static int is_aspace(unsigned c)    /* white space of the plain ASCII rules (used on the raw bytes) */
{
    return c == ' ' || (c >= 0x09 && c <= 0x0d);
}

static int is_digit(unsigned c) { return c >= '0' && c <= '9'; }

static int hexval(unsigned c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

/* ---- unbounded decimal numbers: a file may carry an address or a fuse count of any length ---- */

typedef struct { const unsigned char *d; size_t n; } num;       /* digits without leading zeros; n == 0 is zero */

static num num_make(const unsigned char *d, size_t n)
{
    num x;
    while (n && *d == '0') { d++; n--; }
    x.d = d; x.n = n;
    return x;
}

static int num_cmp(num a, num b)
{
    int c;
    if (a.n != b.n) return a.n < b.n ? -1 : 1;
    c = a.n ? memcmp(a.d, b.d, a.n) : 0;
    return c < 0 ? -1 : c > 0;
}

static int num_small(num a, long long *v)                       /* 1 and the value when it has at most 18 digits */
{
    size_t i;
    long long x = 0;
    if (a.n > 18) return 0;
    for (i = 0; i < a.n; i++) x = x * 10 + (a.d[i] - '0');
    *v = x;
    return 1;
}

/* a - b: -1 when negative, 0 with the value when it has at most 18 digits, 1 when larger */
static int num_diff(num a, num b, long long *out)
{
    long long x, y;
    unsigned char *r;
    size_t i;
    int borrow = 0, res;
    num d;
    if (num_cmp(a, b) < 0) return -1;
    if (num_small(a, &x) && num_small(b, &y)) { *out = x - y; return 0; }
    r = xmalloc(a.n + 1);
    for (i = 0; i < a.n; i++) {
        int da = a.d[a.n - 1 - i] - '0';
        int db = i < b.n ? b.d[b.n - 1 - i] - '0' : 0;
        int v = da - db - borrow;
        borrow = v < 0;
        if (v < 0) v += 10;
        r[a.n - 1 - i] = (unsigned char)('0' + v);
    }
    d = num_make(r, a.n);
    res = num_small(d, out) ? 0 : 1;
    free(r);
    return res;
}

/* ---- fuse records ---- */

typedef struct {
    num addr;
    unsigned char *digits;          /* field text after the address, blanks removed */
    size_t n;
} rec;

static int rec_cmp(const void *pa, const void *pb)
{
    const rec *a = pa, *b = pb;
    size_t m = a->n < b->n ? a->n : b->n;
    int c = num_cmp(a->addr, b->addr);
    if (c) return c;
    c = m ? memcmp(a->digits, b->digits, m) : 0;
    if (c) return c < 0 ? -1 : 1;
    return a->n < b->n ? -1 : a->n > b->n;
}

static size_t count_byte(const unsigned char *p, size_t n, unsigned char c)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) k += p[i] == c;
    return k;
}

/* "*", optional white space, the letter, a digit: a fuse record of that form somewhere in the segment */
static int has_record(const unsigned char *p, size_t n, unsigned char letter)
{
    size_t i, j;
    for (i = 0; i < n; i++) {
        if (p[i] != '*') continue;
        j = i + 1;
        while (j < n && is_aspace(p[j])) j++;
        if (j + 1 < n && p[j] == letter && is_digit(p[j + 1])) return 1;
    }
    return 0;
}

/* skips blanks from *i up to n */
static void skip_blank(const unsigned char *t, size_t n, size_t *i)
{
    while (*i < n && is_uspace(t[*i])) (*i)++;
}

void jedec_parse(const unsigned char *data, size_t len, cy_jedec *out)
{
    size_t stx = 0, etx = 0, i, nstx, netx, nrecs = 0, nkrecs = 0, caprec = 0, capk = 0;
    const unsigned char *body, *seg;
    size_t blen, start, fstart, nparts;
    int hexform, have_qf = 0, have_csum = 0;
    unsigned tx_file = 0, tx_calc, csum_file = 0, csum_calc;
    num qf;
    long long qfv = 0;
    rec *recs = NULL, *krecs = NULL;
    unsigned char *fuses;
    long long defined = 0;

    qf.d = NULL; qf.n = 0;
    nstx = count_byte(data, len, 0x02);
    netx = count_byte(data, len, 0x03);
    if (nstx == 0) refuse("NO_STX");
    if (nstx > 1) refuse("MULTIPLE_STX");
    if (netx == 0) refuse("NO_ETX");
    if (netx > 1) refuse("MULTIPLE_ETX");
    for (i = 0; i < len; i++) { if (data[i] == 0x02) stx = i; if (data[i] == 0x03) etx = i; }
    if (etx < stx) refuse("NO_ETX");
    seg = data + stx;
    hexform = has_record(seg, etx - stx, 'K');
    if (hexform && has_record(seg, etx - stx, 'L')) refuse("MIXED_FORM");
    if (len - (etx + 1) < 4) refuse("MISSING_TRANSMISSION_CHECKSUM");
    for (i = 0; i < 4; i++) {
        int h = hexval(data[etx + 1 + i]);
        if (h < 0) refuse("MISSING_TRANSMISSION_CHECKSUM");
        tx_file = tx_file * 16 + (unsigned)h;
    }
    if (hexform) {
        size_t lf = 0, crlf = 0;
        for (i = 0; i < etx - stx; i++)
            if (seg[i] == '\n') { lf++; if (i > 0 && seg[i - 1] == '\r') crlf++; }
        if (lf != crlf) refuse("BAD_LINE_END");
        tx_calc = (unsigned)((0x17 * (unsigned long long)lf) & 0xffff);
    } else {
        unsigned long long s = 0;
        for (i = stx; i <= etx; i++) s += data[i];
        tx_calc = (unsigned)(s & 0xffff);
    }
    if (tx_file != tx_calc) refuse("BAD_TRANSMISSION_CHECKSUM");

    body = data + stx + 1;
    blen = etx - stx - 1;
    nparts = count_byte(body, blen, '*') + 1;
    {   /* text after the last '*' must be blank */
        size_t last = blen;
        while (last > 0 && body[last - 1] != '*') last--;
        for (i = last; i < blen; i++)
            if (!is_uspace(body[i])) refuse("BAD_FIELD");
    }
    {   /* device name: the first part, blanks stripped at both ends */
        size_t a = 0, b = 0;
        while (b < blen && body[b] != '*') b++;
        fstart = b + 1;
        while (a < b && is_uspace(body[a])) a++;
        while (b > a && is_uspace(body[b - 1])) b--;
        out->design = body + a;
        out->design_len = b - a;
    }
    /* the fields between the first and the last '*' */
    start = fstart;
    for (i = 1; i + 1 < nparts; i++) {
        size_t end = start, p, tn;
        const unsigned char *t;
        while (body[end] != '*') end++;
        p = start;
        start = end + 1;
        while (p < end && is_uspace(body[p])) p++;
        if (p == end) continue;
        t = body + p;
        tn = end - p;
        if (t[0] == 'N') {
            /* note */
        } else if (t[0] == 'Q') {
            size_t k = 2, d0;
            if (tn < 2 || (t[1] != 'F' && t[1] != 'P' && t[1] != 'V')) refuse("BAD_FIELD");
            skip_blank(t, tn, &k);
            d0 = k;
            while (k < tn && is_digit(t[k])) k++;
            if (k == d0) refuse("BAD_FIELD");
            if (t[1] == 'F') { qf = num_make(t + d0, k - d0); have_qf = 1; }
            skip_blank(t, tn, &k);
            if (k != tn) refuse("BAD_FIELD");
        } else if (t[0] == 'F' || t[0] == 'G') {
            size_t k = 1;
            skip_blank(t, tn, &k);
            if (k >= tn || (t[k] != '0' && t[k] != '1')) refuse("BAD_FIELD");
            k++;
            skip_blank(t, tn, &k);
            if (k != tn) refuse("BAD_FIELD");
        } else if (t[0] == 'C') {
            size_t k = 1, q;
            unsigned v = 0;
            skip_blank(t, tn, &k);
            if (tn - k < 4) refuse("BAD_FIELD");
            for (q = 0; q < 4; q++) {
                int h = hexval(t[k + q]);
                if (h < 0) refuse("BAD_FIELD");
                v = v * 16 + (unsigned)h;
            }
            k += 4;
            skip_blank(t, tn, &k);
            if (k != tn) refuse("BAD_FIELD");
            csum_file = v; have_csum = 1;
        } else if (t[0] == 'L' || t[0] == 'K') {
            size_t k = 1, d0 = 1, q;
            rec r;
            while (k < tn && is_digit(t[k])) k++;
            if (k == d0 || k >= tn || !is_uspace(t[k])) refuse("BAD_FIELD");
            r.addr = num_make(t + d0, k - d0);
            r.digits = xmalloc(tn - k + 1);
            r.n = 0;
            for (q = k; q < tn; q++)
                if (!is_uspace(t[q])) r.digits[r.n++] = t[q];
            if (t[0] == 'L') {
                if (nrecs == caprec) { caprec = caprec ? caprec * 2 : 64; recs = realloc(recs, caprec * sizeof *recs); if (!recs) refuse("OUT_OF_MEMORY"); }
                recs[nrecs++] = r;
            } else {
                if (nkrecs == capk) { capk = capk ? capk * 2 : 64; krecs = realloc(krecs, capk * sizeof *krecs); if (!krecs) refuse("OUT_OF_MEMORY"); }
                krecs[nkrecs++] = r;
            }
        } else {
            refuse("UNKNOWN_FIELD");
        }
    }
    if (!have_qf) refuse("QF_MISSING");
    if (nrecs && nkrecs) refuse("MIXED_FORM");
    if (nkrecs) {
        /* hex records in address order; each is turned into a record of 0/1 digits */
        size_t j;
        qsort(krecs, nkrecs, sizeof *krecs, rec_cmp);
        recs = xmalloc(nkrecs * sizeof *recs);
        for (j = 0; j < nkrecs; j++) {
            num next = j + 1 < nkrecs ? krecs[j + 1].addr : qf;
            long long n = 0;
            size_t q;
            int big;
            unsigned char *bits;
            for (q = 0; q < krecs[j].n; q++)
                if (hexval(krecs[j].digits[q]) < 0) refuse("BAD_FUSE_DIGIT");
            big = num_diff(next, krecs[j].addr, &n);
            if (big != 0 || (long long)krecs[j].n != (n + 3) / 4) refuse("HEX_RECORD_LENGTH");
            bits = xmalloc(krecs[j].n * 4 + 1);
            for (q = 0; q < krecs[j].n; q++) {
                int h = hexval(krecs[j].digits[q]), b;
                for (b = 0; b < 4; b++) bits[4 * q + b] = (unsigned char)('0' + ((h >> (3 - b)) & 1));
            }
            for (q = (size_t)n; q < krecs[j].n * 4; q++)
                if (bits[q] == '1') refuse("HEX_PADDING");
            recs[j].addr = krecs[j].addr;
            recs[j].digits = bits;
            recs[j].n = (size_t)n;
        }
        nrecs = nkrecs;
    }

    if (!num_small(qf, &qfv) || qfv > DENSE_LIMIT) {
        /* A fuse count beyond any device: the records cannot fill it.  They are still checked in file order, each as the
           address range it covers, so that the first defect met is the one reported. */
        size_t j, k;
        unsigned long long total = 0;
        for (j = 0; j < nrecs; j++) {
            long long ioor = -1, idup = -1, g;
            size_t q;
            for (q = 0; q < recs[j].n; q++)
                if (recs[j].digits[q] != '0' && recs[j].digits[q] != '1') refuse("BAD_FUSE_DIGIT");
            if (recs[j].n == 0) continue;
            if (num_cmp(recs[j].addr, qf) >= 0) ioor = 0;
            else if (num_diff(qf, recs[j].addr, &g) == 0 && g < (long long)recs[j].n) ioor = g;
            for (k = 0; k < j; k++) {
                long long off;
                int c;
                if (recs[k].n == 0) continue;
                c = num_cmp(recs[k].addr, recs[j].addr);
                if (c <= 0) {
                    if (num_diff(recs[j].addr, recs[k].addr, &off) == 0 && off < (long long)recs[k].n) off = 0; else off = -1;
                } else {
                    if (!(num_diff(recs[k].addr, recs[j].addr, &off) == 0 && off < (long long)recs[j].n)) off = -1;
                }
                if (off >= 0 && (idup < 0 || off < idup)) idup = off;
            }
            if (ioor >= 0 && (idup < 0 || ioor <= idup)) refuse("FUSE_INDEX_OUT_OF_RANGE");
            if (idup >= 0) refuse("FUSE_DEFINED_TWICE");
            total += recs[j].n;
        }
        if (total >= (unsigned long long)DENSE_LIMIT) refuse("QF_TOO_LARGE");
        refuse("QF_MISMATCH");
    }

    fuses = xmalloc((size_t)qfv + 1);
    memset(fuses, 2, (size_t)qfv + 1);                           /* 2 = not defined yet */
    {
        size_t j;
        for (j = 0; j < nrecs; j++) {
            size_t q;
            long long a = 0;
            int small = num_small(recs[j].addr, &a);
            for (q = 0; q < recs[j].n; q++)
                if (recs[j].digits[q] != '0' && recs[j].digits[q] != '1') refuse("BAD_FUSE_DIGIT");
            for (q = 0; q < recs[j].n; q++) {
                if (!small || a >= qfv || (long long)q >= qfv - a) refuse("FUSE_INDEX_OUT_OF_RANGE");
                if (fuses[a + (long long)q] != 2) refuse("FUSE_DEFINED_TWICE");
                fuses[a + (long long)q] = (unsigned char)(recs[j].digits[q] - '0');
                defined++;
            }
        }
    }
    if (defined != qfv) refuse("QF_MISMATCH");
    if (!have_csum) refuse("FUSE_CHECKSUM_MISSING");
    {
        unsigned long long s = 0;
        long long w;
        for (w = 0; w < qfv; w += 8) {
            unsigned v = 0;
            int b;
            for (b = 0; b < 8 && w + b < qfv; b++) v |= (unsigned)(fuses[w + b] & 1) << b;
            s += v;
        }
        csum_calc = (unsigned)(s & 0xffff);
    }
    if (csum_calc != csum_file) refuse("BAD_FUSE_CHECKSUM");
    out->nfuses = (long)qfv;
    out->fuses = fuses;
}
