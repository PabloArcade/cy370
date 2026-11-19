/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Construct kinds of an emitted design.

   The architecture body written by decode.c has a fixed line grammar:
     "\tq<n> <= <expr>;"                                    combinational macrocell (also the carrier "kc")
     "\tPROCESS (...) BEGIN ... END PROCESS;"               register or latch, with optional asynchronous reset / preset
     "\tp<n> <= [NOT ]q<n>[ WHEN (<term>) = '1' ELSE 'Z'];" pin driven by a macrocell
     input-cell processes writing i<n> / j<n> from p<n>
   <expr> is a sum of products of literals [NOT ]p<n> | q<n> | i<n> | j<n> | kc, a constant '0' / '1', or one of the keep
   forms wrapped around a sum of products.  The body is read back line by line and every construct met is given a kind;
   a line outside the grammar is the kind UNPARSED_LINE.  The decoder refuses a design holding a kind that is not in its
   list of supported kinds.  Signal numbers from 201 up are buried macrocells. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy370.h"

#define BURIED_MIN 201
#define NAMELEN 16

static int is_word(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_dig(int c) { return c >= '0' && c <= '9'; }

const char *find_word(const char *text, const char *word)
{
    size_t n = strlen(word);
    const char *p = text;
    while ((p = strstr(p, word)) != NULL) {
        if ((p == text || !is_word((unsigned char)p[-1])) && !is_word((unsigned char)p[n])) return p;
        p++;
    }
    return NULL;
}

/* ---- small matchers: each returns the position after the match, or NULL ---- */

static const char *m_str(const char *s, const char *lit)
{
    size_t n;
    if (!s) return NULL;
    n = strlen(lit);
    return strncmp(s, lit, n) == 0 ? s + n : NULL;
}

static const char *m_digits(const char *s)
{
    if (!s || !is_dig((unsigned char)*s)) return NULL;
    while (is_dig((unsigned char)*s)) s++;
    return s;
}

static const char *m_q(const char *s) { return m_digits(m_str(s, "q")); }

static const char *m_andor(const char *s)
{
    const char *e = m_str(s, "AND");
    return e ? e : m_str(s, "OR");
}

static const char *m_lit(const char *s)
{
    const char *e;
    if (!s) return NULL;
    e = m_str(s, "NOT ");
    if (e) s = e;
    if (*s == 'p' || *s == 'q' || *s == 'i' || *s == 'j') return m_digits(s + 1);
    return m_str(s, "kc");
}

/* a sum of products: "(" literal { " AND " literal } ")" { " OR (" ... ")" } */
static const char *m_sop(const char *s)
{
    const char *e;
    for (;;) {
        s = m_lit(m_str(s, "("));
        if (!s) return NULL;
        while ((e = m_lit(m_str(s, " AND "))) != NULL) s = e;
        s = m_str(s, ")");
        if (!s) return NULL;
        e = m_str(s, " OR (");
        if (!e) return s;
        s = e - 1;
    }
}

static int is_const(const char *e) { return strcmp(e, "'0'") == 0 || strcmp(e, "'1'") == 0; }

static int is_sop(const char *e)                /* a constant or a sum of products, nothing after it */
{
    const char *x;
    if (is_const(e)) return 1;
    x = m_sop(e);
    return x && *x == 0;
}

static int expr_ok(const char *e)
{
    const char *x, *y;
    if (is_sop(e)) return 1;
    /* ((q<n>|kc AND|OR NOT (<sop>)) AND|OR q<n>) */
    x = m_str(e, "((");
    y = m_q(x);
    if (!y) y = m_str(x, "kc");
    x = m_q(m_str(m_andor(m_str(m_str(m_sop(m_str(m_str(m_andor(m_str(y, " ")), " NOT "), "(")), "))"), " ")), " "));
    x = m_str(x, ")");
    if (x && *x == 0) return 1;
    /* (q<n> OR (q<n> AND NOT (<sop>))) */
    x = m_str(m_sop(m_str(m_q(m_str(m_q(m_str(e, "(")), " OR (")), " AND NOT (")), ")))");
    if (x && *x == 0) return 1;
    /* (q<n> AND|OR NOT (<sop>)) */
    x = m_str(m_sop(m_str(m_andor(m_str(m_q(m_str(e, "(")), " ")), " NOT (")), "))");
    return x && *x == 0;
}

/* ---- the parsed body ---- */

typedef struct {
    char name[NAMELEN];
    const char *kind;               /* COMB, DREG_RISE, ..., an input-cell kind, or NULL (second stage of a double register) */
    char *expr;
    char clk[NAMELEN];              /* "" when none */
    int nasy;
    int asy_preset[2];
    char *asy[2];
    int input_cell;
} cell;

typedef struct {
    int pin, inv;
    char q[NAMELEN];
    char *oe;                       /* NULL: always driven */
} pinrec;

typedef struct {
    cell *cells; int ncells;
    pinrec *pins; int npins;
    int nbad;
} body;

static char *dupn(const char *s, size_t n)
{
    char *r = xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static cell *cell_slot(body *b, const char *name)
{
    int i;
    for (i = 0; i < b->ncells; i++)
        if (strcmp(b->cells[i].name, name) == 0) return &b->cells[i];
    memset(&b->cells[b->ncells], 0, sizeof b->cells[0]);
    strcpy(b->cells[b->ncells].name, name);
    return &b->cells[b->ncells++];
}

static void set_cell(body *b, const char *name, const char *kind, char *expr, const char *clk, int nasy, int *ap, char **at, int input)
{
    cell *c = cell_slot(b, name);
    int i;
    c->kind = kind; c->expr = expr; c->nasy = nasy; c->input_cell = input;
    strcpy(c->clk, clk);
    for (i = 0; i < nasy; i++) { c->asy_preset[i] = ap[i]; c->asy[i] = at[i]; }
}

static int ends_with(const char *s, size_t n, const char *suf)
{
    size_t k = strlen(suf);
    return n >= k && memcmp(s + n - k, suf, k) == 0;
}

/* "<digits>" copied as text */
static void numcpy(char *dst, char prefix, const char *a, const char *b)
{
    size_t n = (size_t)(b - a);
    if (n > NAMELEN - 2) n = NAMELEN - 2;
    dst[0] = prefix;
    memcpy(dst + 1, a, n);
    dst[1 + n] = 0;
}

static int same_digits(const char *a, const char *ae, const char *b, const char *be)
{
    return ae - a == be - b && memcmp(a, b, (size_t)(ae - a)) == 0;
}

static int parse_pin(body *b, const char *l, size_t n)
{
    const char *d1, *d1e, *x, *d2, *d2e;
    int inv = 0;
    char *oe = NULL;
    pinrec *p;
    int i, pin;
    d1 = m_str(l, "\tp"); d1e = m_digits(d1);
    x = m_str(d1e, " <= ");
    if (!x) return 0;
    if (m_str(x, "NOT ")) { inv = 1; x += 4; }
    d2 = m_str(x, "q"); d2e = m_digits(d2);
    if (!d2e) return 0;
    if (strcmp(d2e, ";") != 0) {
        const char *w = m_str(d2e, " WHEN (");
        static const char suf[] = ") = '1' ELSE 'Z';";
        size_t rest;
        if (!w) return 0;
        rest = n - (size_t)(w - l);
        if (!ends_with(w, rest, suf)) return 0;
        oe = dupn(w, rest - (sizeof suf - 1));
        if (!expr_ok(oe)) return 0;
    }
    pin = atoi(d1);
    p = NULL;
    for (i = 0; i < b->npins; i++)
        if (b->pins[i].pin == pin) p = &b->pins[i];
    if (!p) p = &b->pins[b->npins++];
    p->pin = pin; p->inv = inv; p->oe = oe;
    numcpy(p->q, 'q', d2, d2e);
    return 1;
}

/* input register: PROCESS (p<c>) BEGIN IF (p<c>'event AND p<c> = '<e>') THEN <i|j><n> <= <p|j><n>; END IF; END PROCESS; */
static int parse_ireg(body *b, const char *l)
{
    const char *c, *ce, *x, *y, *n1, *n1e, *n2, *n2e;
    char edge, dst, src, name[NAMELEN], clk[NAMELEN], ex[NAMELEN];
    const char *kind;
    c = m_str(l, "\tPROCESS (p"); ce = m_digits(c);
    x = m_str(ce, ") BEGIN IF (p"); y = m_digits(x);
    if (!y || !same_digits(c, ce, x, y)) return 0;
    x = m_str(y, "'event AND p"); y = m_digits(x);
    if (!y || !same_digits(c, ce, x, y)) return 0;
    x = m_str(y, " = '");
    if (!x || (*x != '0' && *x != '1')) return 0;
    edge = *x;
    x = m_str(x + 1, "') THEN ");
    if (!x || (*x != 'i' && *x != 'j')) return 0;
    dst = *x; n1 = x + 1; n1e = m_digits(n1);
    x = m_str(n1e, " <= ");
    if (!x || (*x != 'p' && *x != 'j')) return 0;
    src = *x; n2 = x + 1; n2e = m_digits(n2);
    if (!n2e || !same_digits(n1, n1e, n2, n2e)) return 0;
    if (strcmp(n2e, "; END IF; END PROCESS;") != 0) return 0;
    if (dst == 'j' && src == 'p') kind = edge == '1' ? "IDBLREG_RISE" : "IDBLREG_FALL";
    else if (dst == 'i' && src == 'p') kind = edge == '1' ? "IREG_RISE" : "IREG_FALL";
    else if (dst == 'i' && src == 'j') kind = NULL;
    else { b->nbad++; return 1; }
    numcpy(name, dst, n1, n1e); numcpy(ex, src, n1, n1e); numcpy(clk, 'p', c, ce);
    set_cell(b, name, kind, xstrdup(ex), clk, 0, NULL, NULL, 1);
    return 1;
}

/* input latch: PROCESS (p<c>, p<n>) BEGIN IF (p<c> = '<l>') THEN i<n> <= p<n>; END IF; END PROCESS; */
static int parse_ilat(body *b, const char *l)
{
    const char *c, *ce, *n, *ne, *x, *y;
    char lev, name[NAMELEN], clk[NAMELEN], ex[NAMELEN];
    c = m_str(l, "\tPROCESS (p"); ce = m_digits(c);
    n = m_str(ce, ", p"); ne = m_digits(n);
    x = m_str(ne, ") BEGIN IF (p"); y = m_digits(x);
    if (!y || !same_digits(c, ce, x, y)) return 0;
    x = m_str(y, " = '");
    if (!x || (*x != '0' && *x != '1')) return 0;
    lev = *x;
    x = m_str(x + 1, "') THEN i"); y = m_digits(x);
    if (!y || !same_digits(n, ne, x, y)) return 0;
    x = m_str(y, " <= p"); y = m_digits(x);
    if (!y || !same_digits(n, ne, x, y)) return 0;
    if (strcmp(y, "; END IF; END PROCESS;") != 0) return 0;
    numcpy(name, 'i', n, ne); numcpy(ex, 'p', n, ne); numcpy(clk, 'p', c, ce);
    set_cell(b, name, lev == '1' ? "ILATCH_HIGH" : "ILATCH_LOW", xstrdup(ex), clk, 0, NULL, NULL, 1);
    return 1;
}

/* macrocell register / latch process */
static int parse_proc(body *b, const char *l, size_t n)
{
    static const char suf[] = " END PROCESS;";
    const char *x = m_str(l, "\tPROCESS ("), *rest, *e;
    char *r;
    size_t rn;
    int nasy = 0, ap[8];
    char *at[8];
    int is_edge = 0, is_level = 0, i;
    const char *c = NULL, *ce = NULL, *q = NULL, *qe = NULL, *ex = NULL;
    char lv = '1';
    char name[NAMELEN], clk[NAMELEN];
    if (!x) return 0;
    while (*x && *x != ')') x++;
    rest = m_str(x, ") BEGIN ");
    if (!rest) return 0;
    rn = n - (size_t)(rest - l);
    if (!ends_with(rest, rn, suf)) return 0;
    r = dupn(rest, rn - (sizeof suf - 1));
    /* leading asynchronous branches: IF (<term>) = '1' THEN q<n> <= '<v>'; ELS... (the shortest <term> that fits) */
    for (;;) {
        const char *s = m_str(r, "IF ("), *t;
        int found = 0;
        if (!s) break;
        for (t = s; *t; t++) {
            const char *u = m_q(m_str(t, ") = '1' THEN "));
            u = m_str(u, " <= '");
            if (!u || (*u != '0' && *u != '1')) continue;
            if (!m_str(u + 1, "'; ELS")) continue;
            if (nasy < 8) { ap[nasy] = *u == '1'; at[nasy] = dupn(s, (size_t)(t - s)); }
            nasy++;
            r = (char *)(u + 1 + 6);
            found = 1;
            break;
        }
        if (!found) break;
    }
    rn = strlen(r);
    /* IF (p<c>'event AND p<c> = '<e>') THEN q<n> <= <expr>; END IF; */
    c = m_str(r, "IF (p"); ce = m_digits(c);
    e = m_str(ce, "'event AND p");
    if (e) {
        const char *y = m_digits(e);
        if (y && same_digits(c, ce, e, y)) {
            const char *z = m_str(y, " = '");
            if (z && (*z == '0' || *z == '1') && (q = m_str(z + 1, "') THEN ")) != NULL && (qe = m_q(q)) != NULL
                && (ex = m_str(qe, " <= ")) != NULL && ends_with(ex, rn - (size_t)(ex - r), "; END IF;")) { is_edge = 1; lv = *z; }
        }
    }
    if (!is_edge && ce) {
        const char *z = m_str(ce, " = '");
        if (z && (*z == '0' || *z == '1') && (q = m_str(z + 1, "') THEN ")) != NULL && (qe = m_q(q)) != NULL
            && (ex = m_str(qe, " <= ")) != NULL && ends_with(ex, rn - (size_t)(ex - r), "; END IF;")) { is_level = 1; lv = *z; }
    }
    if (!is_edge && !is_level) { b->nbad++; return 1; }
    for (i = 0; i < nasy && i < 8; i++)
        if (!expr_ok(at[i])) { b->nbad++; return 1; }
    if (nasy > 2) { b->nbad++; return 1; }
    {
        char *expr = dupn(ex, rn - (size_t)(ex - r) - 9);
        const char *kind;
        size_t ql = (size_t)(qe - q);
        if (ql > NAMELEN - 1) ql = NAMELEN - 1;
        memcpy(name, q, ql); name[ql] = 0;
        numcpy(clk, 'p', c, ce);
        if (is_edge) {
            /* a toggle register is written "q<n> XOR (<expr>)" */
            size_t el = strlen(expr);
            int tog = 0;
            if (strncmp(expr, name, ql) == 0 && strncmp(expr + ql, " XOR (", 6) == 0 && el >= ql + 7 && expr[el - 1] == ')') {
                expr = dupn(expr + ql + 6, el - ql - 7);
                tog = 1;
            }
            if (!expr_ok(expr)) { b->nbad++; return 1; }
            kind = tog ? (lv == '1' ? "TREG_RISE" : "TREG_FALL") : (lv == '1' ? "DREG_RISE" : "DREG_FALL");
        } else {
            if (!expr_ok(expr)) { b->nbad++; return 1; }
            kind = lv == '1' ? "LATCH_HIGH" : "LATCH_LOW";
        }
        set_cell(b, name, kind, expr, clk, nasy, ap, at, 0);
    }
    return 1;
}

static int parse_comb(body *b, const char *l, size_t n)
{
    const char *x = m_str(l, "\t"), *e, *y;
    char name[NAMELEN];
    char *expr;
    size_t k;
    e = m_q(x);
    if (!e) e = m_str(x, "kc");
    y = m_str(e, " <= ");
    if (!y || n == 0 || l[n - 1] != ';') return 0;
    expr = dupn(y, n - (size_t)(y - l) - 1);
    if (!expr_ok(expr)) return 0;
    k = (size_t)(e - x);
    if (k > NAMELEN - 1) k = NAMELEN - 1;
    memcpy(name, x, k); name[k] = 0;
    set_cell(b, name, "COMB", expr, "", 0, NULL, NULL, 0);
    return 1;
}

static void parse_body(const char *txt, body *b)
{
    const char *s = strstr(txt, "\nBEGIN\n"), *end;
    size_t nlines = 1, i;
    b->ncells = b->npins = b->nbad = 0;
    b->cells = NULL; b->pins = NULL;
    if (!s) { b->cells = xmalloc(sizeof *b->cells); b->pins = xmalloc(sizeof *b->pins); return; }
    s += 7;
    end = strstr(s, "\nEND ");
    if (!end) end = s + strlen(s);
    for (i = 0; s + i < end; i++) nlines += s[i] == '\n';
    b->cells = xmalloc((nlines + 1) * sizeof *b->cells);
    b->pins = xmalloc((nlines + 1) * sizeof *b->pins);
    while (s <= end) {
        const char *e = memchr(s, '\n', (size_t)(end - s));
        size_t n, k;
        char *l;
        int blank = 1;
        if (!e) e = end;
        n = (size_t)(e - s);
        l = dupn(s, n);
        for (k = 0; k < n; k++)
            if (!is_uspace((unsigned char)l[k])) blank = 0;
        if (!blank && !parse_pin(b, l, n) && !parse_ireg(b, l) && !parse_ilat(b, l) && !parse_proc(b, l, n) && !parse_comb(b, l, n))
            b->nbad++;
        s = e + 1;
    }
}

/* ---- sets of signal names ---- */

typedef struct { char (*v)[NAMELEN]; int n, cap; } sset;

static int sset_has(const sset *s, const char *x)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (strcmp(s->v[i], x) == 0) return 1;
    return 0;
}

static void sset_add(sset *s, const char *x)
{
    if (sset_has(s, x)) return;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = realloc(s->v, (size_t)s->cap * sizeof s->v[0]);
        if (!s->v) refuse("OUT_OF_MEMORY");
    }
    strncpy(s->v[s->n], x, NAMELEN - 1);
    s->v[s->n][NAMELEN - 1] = 0;
    s->n++;
}

/* the signals an expression reads: whole words p<n>, q<n>, i<n>, j<n> and kc */
static void refs(const char *e, sset *out)
{
    const char *p = e;
    while (*p) {
        if (is_word((unsigned char)*p)) {
            const char *w = p;
            size_t n;
            while (is_word((unsigned char)*p)) p++;
            n = (size_t)(p - w);
            if (n < NAMELEN) {
                char buf[NAMELEN];
                size_t k;
                int ok = n >= 2 && (*w == 'p' || *w == 'q' || *w == 'i' || *w == 'j');
                for (k = 1; ok && k < n; k++) ok = is_dig((unsigned char)w[k]);
                if (!ok) ok = n == 2 && w[0] == 'k' && w[1] == 'c';
                if (ok) { memcpy(buf, w, n); buf[n] = 0; sset_add(out, buf); }
            }
        } else p++;
    }
}

static void kadd(cy_kinds *k, const char *name, int n)
{
    int i;
    for (i = 0; i < k->n; i++)
        if (strcmp(k->name[i], name) == 0) { k->count[i] += n; return; }
    k->name = realloc(k->name, (size_t)(k->n + 1) * sizeof *k->name);
    k->count = realloc(k->count, (size_t)(k->n + 1) * sizeof *k->count);
    if (!k->name || !k->count) refuse("OUT_OF_MEMORY");
    k->name[k->n] = xstrdup(name);
    k->count[k->n] = n;
    k->n++;
}

int kinds_get(const cy_kinds *k, const char *name)
{
    int i;
    for (i = 0; i < k->n; i++)
        if (strcmp(k->name[i], name) == 0) return k->count[i];
    return 0;
}

static const cell *cell_by(const body *b, const char *name)
{
    int i;
    for (i = 0; i < b->ncells; i++)
        if (strcmp(b->cells[i].name, name) == 0) return &b->cells[i];
    return NULL;
}

static int is_latch(const cell *c) { return c->kind && strncmp(c->kind, "LATCH", 5) == 0; }
static int is_comb(const cell *c) { return c->kind && strcmp(c->kind, "COMB") == 0; }

/* "NOT <q>" as whole words somewhere in e */
static int has_not(const char *e, const char *q)
{
    char pat[NAMELEN + 8];
    sprintf(pat, "NOT %s", q);
    return find_word(e, pat) != NULL;
}

/* does the expression reach "NOT <q>" directly or through combinational macrocells? */
static int neg_reads(const body *b, const char *e, const char *q, sset *seen)
{
    sset r = {0, 0, 0};
    int i;
    if (has_not(e, q)) return 1;
    refs(e, &r);
    for (i = 0; i < r.n; i++) {
        const cell *c = cell_by(b, r.v[i]);
        if (c && is_comb(c) && strcmp(c->name, "kc") != 0 && !sset_has(seen, r.v[i])) {
            sset_add(seen, r.v[i]);
            if (neg_reads(b, c->expr, q, seen)) return 1;
        }
    }
    return 0;
}

static int in_path(const cell *c) { return c && (is_comb(c) || is_latch(c)); }

/* the driven pads an expression reads, directly or through combinational macrocells and transparent latches */
static void cone_pads(const body *b, const char *e, sset *seen, int *pads, int *npads)
{
    sset r = {0, 0, 0};
    int i, k;
    refs(e, &r);
    for (i = 0; i < r.n; i++) {
        const cell *c;
        if (r.v[i][0] == 'p') {
            int p = atoi(r.v[i] + 1), have = 0;
            for (k = 0; k < b->npins; k++)
                if (b->pins[k].pin == p) have = 1;
            for (k = 0; have && k < *npads; k++)
                if (pads[k] == p) have = 0;
            if (have) pads[(*npads)++] = p;
        }
        c = cell_by(b, r.v[i]);
        if (in_path(c) && !sset_has(seen, r.v[i])) {
            sset_add(seen, r.v[i]);
            cone_pads(b, c->expr, seen, pads, npads);
        }
    }
}

/* every signal in the combinational cone of an expression */
static void cone_cells(const body *b, const char *e, sset *seen, sset *out)
{
    sset r = {0, 0, 0};
    int i;
    refs(e, &r);
    for (i = 0; i < r.n; i++) {
        const cell *c = cell_by(b, r.v[i]);
        sset_add(out, r.v[i]);
        if (c && is_comb(c) && !sset_has(seen, r.v[i])) {
            sset_add(seen, r.v[i]);
            cone_cells(b, c->expr, seen, out);
        }
    }
}

/* every product of the sum carries <q> as a positive literal: the cell can keep or lose its value, never set it */
static int hold_only(const char *q, const char *e)
{
    const char *p = e;
    int nterms = 0;
    if (is_const(e) || !is_sop(e)) return 0;
    while ((p = strchr(p, '(')) != NULL) {
        const char *c = strchr(p, ')');
        char *t = dupn(p + 1, (size_t)(c - p - 1));
        const char *w = t;
        int ok = 0;
        while ((w = find_word(w, q)) != NULL) {
            if (!(w - t >= 4 && strncmp(w - 4, "NOT ", 4) == 0)) { ok = 1; break; }
            w++;
        }
        if (!ok) return 0;
        nterms++;
        p = c + 1;
    }
    return nterms > 0;
}

/* ---- a one-cell loop with no settled value ---- */

typedef struct { int n; int var[40]; signed char val[40]; } pterm;      /* a product as required values of variables */

typedef struct { sset names; } vars;

static int var_of(vars *v, const char *s)
{
    int i;
    for (i = 0; i < v->names.n; i++)
        if (strcmp(v->names.v[i], s) == 0) return i;
    sset_add(&v->names, s);
    return v->names.n - 1;
}

/* The products of a sum with the cell at value v and its pin at (v XOR inv); a product with a false literal or two
   opposite literals is dropped.  Returns the number kept. */
static int cofactor(const char *e, const char *q, const char *pn, int inv, int v, vars *vs, pterm *out)
{
    const char *p = e;
    int n = 0;
    while ((p = strchr(p, '(')) != NULL) {
        const char *c = strchr(p, ')');
        const char *s = p + 1;
        pterm t;
        int ok = 1;
        t.n = 0;
        while (s < c && ok) {
            int neg = 0, k, id, want;
            char name[NAMELEN];
            size_t L;
            const char *e2;
            if (strncmp(s, "NOT ", 4) == 0) { neg = 1; s += 4; }
            e2 = s;
            while (e2 < c && *e2 != ' ') e2++;
            L = (size_t)(e2 - s);
            if (L > NAMELEN - 1) L = NAMELEN - 1;
            memcpy(name, s, L); name[L] = 0;
            s = e2;
            if (strncmp(s, " AND ", 5) == 0) s += 5;
            if ((q && strcmp(name, q) == 0) || (pn && strcmp(name, pn) == 0)) {
                int val = (q && strcmp(name, q) == 0) ? v : (v != inv);
                if (val == neg) ok = 0;
                continue;
            }
            id = var_of(vs, name);
            want = !neg;
            for (k = 0; k < t.n; k++)
                if (t.var[k] == id) break;
            if (k < t.n) { if (t.val[k] != want) ok = 0; }
            else if (t.n < 40) { t.var[t.n] = id; t.val[t.n] = (signed char)want; t.n++; }
        }
        if (ok) out[n++] = t;
        p = c + 1;
    }
    return n;
}

/* can the partial assignment be extended so that every product is false? */
static int all_false(signed char *asg, int nv, const pterm *t, int nt)
{
    int i, k;
    for (i = 0; i < nt; i++) {
        int dead = 0;
        for (k = 0; k < t[i].n; k++)
            if (asg[t[i].var[k]] >= 0 && asg[t[i].var[k]] != t[i].val[k]) dead = 1;
        if (dead) continue;
        for (k = 0; k < t[i].n; k++) {
            int id = t[i].var[k];
            if (asg[id] < 0) {
                asg[id] = (signed char)!t[i].val[k];
                if (all_false(asg, nv, t + i + 1, nt - i - 1)) { asg[id] = -1; return 1; }
                asg[id] = -1;
            }
        }
        return 0;
    }
    return 1;
}

static int count_terms(const char *e)
{
    int n = 0;
    for (; *e; e++) n += *e == '(';
    return n;
}

static int no_settled_value(const char *e, const char *q, const char *pn, int inv, const char *oe)
{
    vars vs;
    int nt = count_terms(e), nhi, nlo, nen, h, g, k, res = 0;
    pterm *hi = xmalloc((size_t)(nt + 1) * sizeof *hi), *lo = xmalloc((size_t)(nt + 1) * sizeof *lo), *en;
    signed char *asg;
    memset(&vs, 0, sizeof vs);
    nhi = cofactor(e, q, pn, inv, 0, &vs, hi);
    nlo = cofactor(e, q, pn, inv, 1, &vs, lo);
    if (oe && !is_const(oe)) {
        en = xmalloc((size_t)(count_terms(oe) + 1) * sizeof *en);
        nen = cofactor(oe, NULL, NULL, 0, 0, &vs, en);
    } else {
        en = xmalloc(sizeof *en);
        en[0].n = 0;
        nen = (oe && strcmp(oe, "'0'") == 0) ? 0 : 1;
    }
    asg = xmalloc((size_t)vs.names.n + 1);
    for (h = 0; h < nhi && !res; h++)
        for (g = 0; g < nen && !res; g++) {
            int ok = 1;
            memset(asg, -1, (size_t)vs.names.n + 1);
            for (k = 0; k < hi[h].n; k++) asg[hi[h].var[k]] = hi[h].val[k];
            for (k = 0; k < en[g].n; k++) {
                int id = en[g].var[k];
                if (asg[id] >= 0 && asg[id] != en[g].val[k]) ok = 0;
                asg[id] = en[g].val[k];
            }
            if (ok && all_false(asg, vs.names.n, lo, nlo)) res = 1;
        }
    return res;
}

/* ---- strongly connected components among combinational macrocells ---- */

typedef struct {
    int n;
    int **adj; int *nadj;
    int *idx, *low, *on, *stack, sp, counter;
    int *comp;                      /* component number of each node, -1 when it is alone */
    int ncomp;
} graph;

static void scc(graph *g, int v)
{
    int i;
    g->idx[v] = g->low[v] = g->counter++;
    g->stack[g->sp++] = v; g->on[v] = 1;
    for (i = 0; i < g->nadj[v]; i++) {
        int w = g->adj[v][i];
        if (g->idx[w] < 0) { scc(g, w); if (g->low[w] < g->low[v]) g->low[v] = g->low[w]; }
        else if (g->on[w] && g->idx[w] < g->low[v]) g->low[v] = g->idx[w];
    }
    if (g->low[v] == g->idx[v]) {
        int start = g->sp, w;
        do { w = g->stack[--g->sp]; g->on[w] = 0; } while (w != v);
        if (start - g->sp > 1) {
            for (i = g->sp; i < start; i++) g->comp[g->stack[i]] = g->ncomp;
            g->ncomp++;
        }
    }
}

void kinds_of(const char *txt, cy_kinds *K)
{
    body b;
    sset driven = {0, 0, 0}, driven_pins = {0, 0, 0}, combs = {0, 0, 0}, regs = {0, 0, 0};
    int i, k;
    char buf[64];
    K->n = 0; K->name = NULL; K->count = NULL;
    parse_body(txt, &b);
    if (b.nbad) kadd(K, "UNPARSED_LINE", b.nbad);
    for (i = 0; i < b.npins; i++) {
        sset_add(&driven, b.pins[i].q);
        sprintf(buf, "p%d", b.pins[i].pin);
        sset_add(&driven_pins, buf);
    }
    for (i = 0; i < b.ncells; i++) {
        const cell *c = &b.cells[i];
        if (is_comb(c)) { if (strcmp(c->name, "kc") != 0) sset_add(&combs, c->name); }
        else if (!c->input_cell) sset_add(&regs, c->name);
    }
    for (i = 0; i < b.ncells; i++) {
        const cell *c = &b.cells[i];
        const char *q = c->name, *e = c->expr;
        sset r = {0, 0, 0};
        int self, hit;
        if (strcmp(q, "kc") == 0) { kadd(K, "CARRIER_BURIED", 1); continue; }
        if (c->input_cell) { if (c->kind) kadd(K, c->kind, 1); continue; }
        sprintf(buf, "%s@%s", c->kind, atoi(q + 1) >= BURIED_MIN ? "BURIED" : sset_has(&driven, q) ? "PIN" : "INTERNAL");
        kadd(K, buf, 1);
        refs(e, &r);
        self = sset_has(&r, q);
        if (is_comb(c) && is_const(e)) kadd(K, "COMB_CONST", 1);
        for (hit = 0, k = 0; k < r.n; k++) hit |= strcmp(r.v[k], q) != 0 && sset_has(&regs, r.v[k]);
        if (hit) kadd(K, "FB_REG", 1);
        if (self && !is_comb(c) && !is_latch(c)) kadd(K, "FB_REG", 1);
        for (hit = 0, k = 0; k < r.n; k++) hit |= strcmp(r.v[k], q) != 0 && sset_has(&combs, r.v[k]);
        if (hit) kadd(K, "FB_COMB", 1);
        for (hit = 0, k = 0; k < r.n; k++) hit |= sset_has(&driven_pins, r.v[k]);
        if (hit) kadd(K, "FB_PIN_OF_OUTPUT", 1);
        for (hit = 0, k = 0; k < r.n; k++) hit |= r.v[k][0] == 'q' && atoi(r.v[k] + 1) >= BURIED_MIN;
        if (hit) kadd(K, "FB_BURIED", 1);
        if (is_comb(c) && self) kadd(K, "COMB_SELF_LOOP", 1);
        if (is_latch(c) && self) kadd(K, "LATCH_SELF_FEEDBACK", 1);
        if (is_latch(c)) {
            sset seen = {0, 0, 0};
            if (neg_reads(&b, e, q, &seen)) kadd(K, "LATCH_SELF_NEGATION", 1);
        }
        if (c->nasy) {
            int rs = 0, ps = 0;
            for (k = 0; k < c->nasy; k++) { if (c->asy_preset[k]) ps = 1; else rs = 1; }
            kadd(K, rs && ps ? "ASYNC_RESET_PRESET" : rs ? "ASYNC_RESET" : "ASYNC_PRESET", 1);
        }
        {
            sset ar = {0, 0, 0};
            for (k = 0; k < c->nasy; k++) refs(c->asy[k], &ar);
            for (hit = 0, k = 0; k < ar.n; k++) hit |= ar.v[k][0] == 'i';
            if (hit) kadd(K, "IREG_IN_ASYNC", 1);
            if (c->clk[0]) {
                size_t el = strlen(e), ql = strlen(q);
                const char *x;
                int done = 0;
                if (sset_has(&r, c->clk) || sset_has(&ar, c->clk)) kadd(K, "CLOCK_IN_LOGIC", 1);
                /* keep forms: recognised by their fixed head and tail around any text */
                sprintf(buf, " %s)", q);
                x = m_str(m_andor(m_str(m_q(m_str(e, "((")), " ")), " NOT (");
                if (x) {
                    size_t tail = ql + 2;                           /* " q<n>)" */
                    if (el >= tail && strcmp(e + el - tail, buf) == 0) {
                        size_t body_end = el - tail;                 /* text before " q<n>)" must end "))" + " AND" | " OR" */
                        size_t op = body_end >= 4 && strncmp(e + body_end - 4, " AND", 4) == 0 ? 4 : body_end >= 3 && strncmp(e + body_end - 3, " OR", 3) == 0 ? 3 : 0;
                        if (op && body_end - op >= 2 && strncmp(e + body_end - op - 2, "))", 2) == 0 && e + body_end - op - 2 >= x) { kadd(K, "KEEP_NODE_D", 1); done = 1; }
                    }
                }
                if (!done) {
                    char head[NAMELEN + 8];
                    sprintf(head, "(%s OR (", q);
                    x = m_str(m_q(m_str(e, head)), " AND NOT (");
                    if (x && el >= 3 && strcmp(e + el - 3, ")))") == 0 && e + el - 3 >= x) { kadd(K, "KEEP_HOLD", 1); done = 1; }
                }
                if (!done && is_latch(c)) {
                    x = m_str(m_andor(m_str(m_q(m_str(e, "(")), " ")), " NOT (");
                    if (x && el >= 2 && strcmp(e + el - 2, "))") == 0 && e + el - 2 >= x) { kadd(K, "KEEP_NODE_LATCH", 1); done = 1; }
                }
                if (!done) {
                    x = m_str(m_andor(m_str(e, "((kc ")), " NOT (");
                    if (x) {
                        size_t tail = ql + 2;
                        if (el >= tail && strcmp(e + el - tail, buf) == 0) {
                            size_t body_end = el - tail;
                            size_t op = body_end >= 4 && strncmp(e + body_end - 4, " AND", 4) == 0 ? 4 : body_end >= 3 && strncmp(e + body_end - 3, " OR", 3) == 0 ? 3 : 0;
                            if (op && body_end - op >= 2 && strncmp(e + body_end - op - 2, "))", 2) == 0 && e + body_end - op - 2 >= x) kadd(K, "KEEP_NODE_D", 1);
                        }
                    }
                }
            }
        }
    }
    /* a combinational macrocell (pad form: through its own pin) with no settled value */
    {
        for (i = 0; i < combs.n; i++) {
            const cell *c = cell_by(&b, combs.v[i]);
            const char *q = c->name, *e = c->expr, *oe = NULL;
            sset r = {0, 0, 0};
            int pin = -1, inv = 0;
            char pn[NAMELEN];
            if (!is_sop(e) || is_const(e)) continue;
            refs(e, &r);
            for (k = 0; k < b.npins; k++) {
                sprintf(buf, "p%d", b.pins[k].pin);
                if (strcmp(b.pins[k].q, q) == 0 && sset_has(&r, buf) && (pin < 0 || b.pins[k].pin < pin)) {
                    pin = b.pins[k].pin; inv = b.pins[k].inv; oe = b.pins[k].oe;
                }
            }
            if (pin < 0 && !sset_has(&r, q)) continue;
            pn[0] = 0;
            if (pin >= 0) sprintf(pn, "p%d", pin);
            if (oe) {
                sset ro = {0, 0, 0};
                if (!is_sop(oe)) continue;
                refs(oe, &ro);
                if (sset_has(&ro, q) || (pin >= 0 && sset_has(&ro, pn))) continue;
            }
            if (no_settled_value(e, q, pin >= 0 ? pn : NULL, inv, oe))
                kadd(K, pin >= 0 ? "PAD_SELF_LOOP_NOT_SETTLED" : "COMB_SELF_LOOP_NOT_SETTLED", 1);
        }
    }
    /* a combinational macrocell whose whole sum of products is its own feedback: the only signal it reads is the cell
       itself, so no input can ever change it (the fitter removes such a cell and leaves its pin undriven) */
    {
        for (i = 0; i < combs.n; i++) {
            const cell *c = cell_by(&b, combs.v[i]);
            sset r = {0, 0, 0};
            if (!is_sop(c->expr) || is_const(c->expr)) continue;
            refs(c->expr, &r);
            if (r.n == 1 && sset_has(&r, c->name)) kadd(K, "COMB_HOLD_SELF_ONLY", 1);
        }
    }
    /* "\tATTRIBUTE synthesis_off OF q<n> : SIGNAL IS true;" lines anywhere in the text */
    {
        const char *s = txt;
        while (s && *s) {
            const char *e = strchr(s, '\n');
            const char *x = m_str(m_q(m_str(s, "\tATTRIBUTE synthesis_off OF ")), " : SIGNAL IS true;");
            if (x && (*x == '\n' || *x == 0)) kadd(K, "COMB_KEPT_SYNTHESIS_OFF", 1);
            s = e ? e + 1 : NULL;
        }
    }
    /* pads: pin outputs, enables that read a driven pad, enable loops through pads */
    {
        int np = b.npins;
        int **val = xmalloc((size_t)(np + 1) * sizeof *val), *nval = xmalloc((size_t)(np + 1) * sizeof *nval);
        int **oep = xmalloc((size_t)(np + 1) * sizeof *oep), *noep = xmalloc((size_t)(np + 1) * sizeof *noep);
        for (i = 0; i < np; i++) {
            const cell *c = cell_by(&b, b.pins[i].q);
            sset seen = {0, 0, 0}, seen2 = {0, 0, 0};
            val[i] = xmalloc((size_t)(np + 1) * sizeof(int)); nval[i] = 0;
            oep[i] = xmalloc((size_t)(np + 1) * sizeof(int)); noep[i] = 0;
            if (in_path(c)) cone_pads(&b, c->expr, &seen, val[i], &nval[i]);
            if (b.pins[i].oe) cone_pads(&b, b.pins[i].oe, &seen2, oep[i], &noep[i]);
        }
        for (i = 0; i < np; i++) {
            kadd(K, b.pins[i].oe ? "OUT_TRISTATE" : "OUT_ALWAYS", 1);
            if (b.pins[i].inv) kadd(K, "OUT_INVERTED", 1);
            if (b.pins[i].oe) {
                sset ro = {0, 0, 0};
                int hit = 0, a;
                refs(b.pins[i].oe, &ro);
                for (k = 0; k < ro.n; k++) hit |= sset_has(&driven_pins, ro.v[k]);
                if (hit) kadd(K, "FB_PIN_OF_OUTPUT", 1);
                /* edge A -> B when pad B's value cone or enable cone reads pad A; the enable of this pin closes a loop
                   when some pad its cone reads is reachable from this pin's pad */
                hit = 0;
                for (a = 0; a < noep[i] && !hit; a++) {
                    int target = oep[i][a], sp = 0, x;
                    int *todo = xmalloc((size_t)(np * np + np + 2) * sizeof *todo);
                    char *seenp = xmalloc((size_t)np + 1);
                    memset(seenp, 0, (size_t)np + 1);
                    todo[sp++] = b.pins[i].pin;
                    while (sp && !hit) {
                        int cur = todo[--sp], ci = -1;
                        if (cur == target) { hit = 1; break; }
                        for (x = 0; x < np; x++) if (b.pins[x].pin == cur) ci = x;
                        if (ci < 0 || seenp[ci]) continue;
                        seenp[ci] = 1;
                        for (x = 0; x < np; x++) {
                            int y, reads = 0;
                            for (y = 0; y < nval[x]; y++) reads |= val[x][y] == cur;
                            for (y = 0; y < noep[x]; y++) reads |= oep[x][y] == cur;
                            if (reads) todo[sp++] = b.pins[x].pin;
                        }
                    }
                }
                if (hit) kadd(K, "OE_PAD_LOOP", 1);
            }
        }
    }
    /* cycles of two or more combinational macrocells */
    {
        graph g;
        int n = combs.n, ncyc_hold = 0;
        memset(&g, 0, sizeof g);
        g.n = n;
        g.adj = xmalloc((size_t)(n + 1) * sizeof *g.adj); g.nadj = xmalloc((size_t)(n + 1) * sizeof *g.nadj);
        g.idx = xmalloc((size_t)(n + 1) * sizeof(int)); g.low = xmalloc((size_t)(n + 1) * sizeof(int));
        g.on = xmalloc((size_t)(n + 1) * sizeof(int)); g.stack = xmalloc((size_t)(n + 1) * sizeof(int));
        g.comp = xmalloc((size_t)(n + 1) * sizeof(int));
        for (i = 0; i < n; i++) {
            sset r = {0, 0, 0};
            g.idx[i] = -1; g.on[i] = 0; g.comp[i] = -1; g.nadj[i] = 0;
            g.adj[i] = xmalloc((size_t)(n + 1) * sizeof(int));
            refs(cell_by(&b, combs.v[i])->expr, &r);
            for (k = 0; k < n; k++)
                if (k != i && sset_has(&r, combs.v[k])) g.adj[i][g.nadj[i]++] = k;
        }
        for (i = 0; i < n; i++)
            if (g.idx[i] < 0) scc(&g, i);
        if (g.ncomp) kadd(K, "COMB_CYCLE", g.ncomp);
        /* a cycle with a hold-only member, a member reading a driven pad, and an output enable whose cone reads a member */
        for (k = 0; k < g.ncomp; k++) {
            int hold = 0, pad = 0, en = 0, x, y;
            for (i = 0; i < n; i++) {
                const cell *c;
                sset r = {0, 0, 0};
                if (g.comp[i] != k) continue;
                c = cell_by(&b, combs.v[i]);
                if (hold_only(c->name, c->expr)) hold = 1;
                refs(c->expr, &r);
                for (x = 0; x < r.n; x++) pad |= sset_has(&driven_pins, r.v[x]);
            }
            for (x = 0; x < b.npins && !en; x++) {
                sset seen = {0, 0, 0}, out = {0, 0, 0};
                if (!b.pins[x].oe) continue;
                cone_cells(&b, b.pins[x].oe, &seen, &out);
                for (y = 0; y < out.n && !en; y++)
                    for (i = 0; i < n; i++)
                        if (g.comp[i] == k && strcmp(out.v[y], combs.v[i]) == 0) en = 1;
            }
            if (hold && pad && en) ncyc_hold++;
        }
        if (ncyc_hold) kadd(K, "CYCLE_HOLD_PAD_OE", ncyc_hold);
    }
}
