/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The decoder: a FLASH370 fuse file -> a VHDL design Warp accepts, every signal fixed to its package pin.

   Signals of the emitted design, numbered as the I/O cells of the device and renamed to package pins at the end:
     p<n>   pin n                       q<n>   macrocell at pin n, or buried macrocell node n
     i<n>   input cell (register / latch) of dedicated input n;  j<n> the first stage of a double input register
     kc     an added carrier signal (see the keep forms below)
   One line per macrocell: a concurrent assignment for a combinational macrocell, a PROCESS for a register or latch
   (asynchronous reset first, then preset, then the clocked / enabled branch), and one line per driven pin
   ("p<n> <= [NOT ]q<n> [WHEN (<output enable term>) = '1' ELSE 'Z']").

   Every configuration value met must be in the tables of supported values and every construct of the result in the list
   of supported kinds; anything else is refused by name. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy370.h"

/* ---- small containers ---- */

typedef struct { char **v; int n, cap; } strlist;

static void sl_add(strlist *l, char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v);
        if (!l->v) refuse("OUT_OF_MEMORY");
    }
    l->v[l->n++] = s;
}

static int sl_has(const strlist *l, const char *s)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (strcmp(l->v[i], s) == 0) return 1;
    return 0;
}

static void sl_add_unique(strlist *l, char *s) { if (!sl_has(l, s)) sl_add(l, s); }

static int str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void sl_sort(strlist *l) { if (l->n > 1) qsort(l->v, (size_t)l->n, sizeof *l->v, str_cmp); }

typedef struct { int *v; int n, cap; } intlist;

static int il_has(const intlist *l, int x)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->v[i] == x) return 1;
    return 0;
}

static void il_add(intlist *l, int x)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v);
        if (!l->v) refuse("OUT_OF_MEMORY");
    }
    l->v[l->n++] = x;
}

static void il_add_unique(intlist *l, int x) { if (!il_has(l, x)) il_add(l, x); }
static int int_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void il_sort(intlist *l) { if (l->n > 1) qsort(l->v, (size_t)l->n, sizeof *l->v, int_cmp); }

static int in_zlist(const int *z, int x)
{
    for (; *z; z++)
        if (*z == x) return 1;
    return 0;
}

/* ---- state of one run ---- */

typedef struct { int pin; const char *kind; const char *ck; char lv; } ireg;        /* input cell of a dedicated input */

typedef struct {
    int lab, m;
    const cy_mc *mc;
    const char *reg, *pol, *oe;
} usedmc;

typedef struct { int single, nl, pin; char *text; } node;

static const cy_device *DEV;
static const cy_model *MOD;
static cy_decoded D;
static strlist used_pairs;          /* "field\tvalue" */
static intlist reads;               /* pins read by some term */
static ireg iregs[8];
static int niregs;
static intlist used_pins;           /* pins / nodes of the macrocells in use */

sb cy_used;

const cy_device *device_by_name(const unsigned char *name, size_t n)
{
    int i;
    for (i = 0; i < 10; i++) {
        const char *d = cy_devices[i].name;
        size_t k;
        if (strlen(d) != n) continue;
        for (k = 0; k < n; k++) {
            unsigned c = name[k];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != (unsigned char)d[k]) break;
        }
        if (k == n) return &cy_devices[i];
    }
    return NULL;
}

static const char *family(void)      /* the part name without the "i" */
{
    return cy_models[DEV->model].name;
}

static int in_values(const char *values, const char *v)
{
    char *pat = xsprintf("|%s|", v);
    int r = strstr(values, pat) != NULL;
    free(pat);
    return r;
}

/* A configuration value met in the fuse file: recorded, and refused when it is not a supported one. */
static void need(const char *kind, const char *value, const char *where)
{
    const cy_valset *s;
    const cy_famset *f;
    const char *ok = NULL;
    sl_add_unique(&used_pairs, xsprintf("%s\t%s", kind, value));
    for (s = cy_supported; s->field; s++)
        if (strcmp(s->field, kind) == 0) ok = s->values;
    if (!ok || !in_values(ok, value)) refuse("%s_NOT_SUPPORTED %s=%s at %s", kind, kind, value, where);
    for (f = cy_family_checked; f->field; f++)
        if (strcmp(f->field, kind) == 0 && in_values(f->values, value)) {
            const cy_famset *g;
            int found = 0;
            for (g = cy_family_supported; g->field; g++)
                if (strcmp(g->field, kind) == 0 && strcmp(g->family, family()) == 0 && in_values(g->values, value)) found = 1;
            if (!found) refuse("%s_NOT_SUPPORTED_ON_%s %s=%s at %s", kind, family(), kind, value, where);
        }
}

static int pair_cmp(const void *a, const void *b)
{
    /* by field, then by value: the tab separator sorts below every character of a field name */
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void used_lines(sb *out)
{
    int i;
    if (used_pairs.n > 1) qsort(used_pairs.v, (size_t)used_pairs.n, sizeof *used_pairs.v, pair_cmp);
    for (i = 0; i < used_pairs.n; i++) {
        const char *t = strchr(used_pairs.v[i], '\t');
        sb_printf(out, "USED %.*s %s\n", (int)(t - used_pairs.v[i]), used_pairs.v[i], t + 1);
    }
}

static const ireg *ireg_of(int pin)
{
    int i;
    for (i = 0; i < niregs; i++)
        if (iregs[i].pin == pin) return &iregs[i];
    return NULL;
}

/* one literal of the model ("pin12", "/fb7", "fbnode204") as a signal of the design */
static void lit_text(sb *out, const char *l)
{
    int neg = l[0] == '/';
    const char *s = neg ? l + 1 : l;
    char v[24];
    if (strncmp(s, "pin", 3) == 0) {
        int n = atoi(s + 3);
        if (n == DEV->vpp) refuse("VPP_PIN_READ pin %d is the %s VPP pin, not a user pin", n, DEV->name);
        if (in_zlist(DEV->dedicated, n) || in_zlist(DEV->clocks, n)) {
            need("SOURCE", "input", s);
            sprintf(v, ireg_of(n) ? "i%d" : "p%d", n);
        } else {
            need("SOURCE", "pin", s);
            sprintf(v, "p%d", n);
        }
        il_add_unique(&reads, n);
    } else if (strncmp(s, "fbnode", 6) == 0) {
        int n = atoi(s + 6);
        if (!il_has(&used_pins, n)) refuse("FEEDBACK_OF_UNUSED_MACROCELL %s", s);
        need("SOURCE", "buried", s);
        sprintf(v, "q%d", n);
    } else {
        int n = atoi(s + 2);
        if (!il_has(&used_pins, n)) refuse("FEEDBACK_OF_UNUSED_MACROCELL %s", s);
        need("SOURCE", "feedback", s);
        sprintf(v, "q%d", n);
    }
    if (neg) sb_puts(out, "NOT ");
    sb_puts(out, v);
}

/* a sum of products as text: the products sorted, equal products written once */
static char *sop_text(const cy_term *terms, int n)
{
    strlist parts = {0, 0, 0};
    sb out = {0, 0, 0};
    int i, k;
    if (n == 0) return xstrdup("'0'");
    for (i = 0; i < n; i++) {
        sb t = {0, 0, 0};
        if (terms[i].kind == T_TRUE) return xstrdup("'1'");
        sb_puts(&t, "(");
        for (k = 0; k < terms[i].nlits; k++) {
            if (k) sb_puts(&t, " AND ");
            lit_text(&t, terms[i].lits[k]);
        }
        sb_puts(&t, ")");
        sl_add_unique(&parts, t.p);
    }
    sl_sort(&parts);
    for (i = 0; i < parts.n; i++) {
        if (i) sb_puts(&out, " OR ");
        sb_puts(&out, parts.v[i]);
    }
    return out.p;
}

static int node_cmp(const void *a, const void *b)
{
    const node *x = a, *y = b;
    if (x->single != y->single) return x->single - y->single;
    if (x->nl != y->nl) return x->nl - y->nl;
    return x->pin - y->pin;
}

/* the words of an expression: text between blanks and parentheses */
static void words(const char *e, strlist *out, int signals_only)
{
    const char *p = e;
    while (*p) {
        const char *w;
        while (*p == ' ' || *p == '(' || *p == ')' || *p == '\t') p++;
        if (!*p) break;
        w = p;
        while (*p && *p != ' ' && *p != '(' && *p != ')' && *p != '\t') p++;
        if (!signals_only || *w == 'p' || *w == 'q' || *w == 'i' || *w == 'j') {
            char *s = xmalloc((size_t)(p - w) + 1);
            memcpy(s, w, (size_t)(p - w));
            s[p - w] = 0;
            sl_add_unique(out, s);
        }
    }
}

static const cy_term *bank_term(const cy_lab *L, int bank)
{
    /* the bank number is the last word of the field's description; a later field of the same bank replaces an earlier one */
    const cy_term *t = NULL;
    int i;
    for (i = 0; i < L->nbanks; i++) {
        const char *a = strstr(L->banks[i].args, "BANK"), *last = NULL;
        while (a) { last = a; a = strstr(a + 4, "BANK"); }
        if (last) {
            const char *x = last + 4, *e;
            while (is_uspace((unsigned char)*x)) x++;
            e = x + strlen(x);
            while (e > x && is_uspace((unsigned char)e[-1])) e--;
            if ((int)(e - x) > 0 && atoi(x) == bank) {
                char num[16];
                sprintf(num, "%d", bank);
                if (strlen(num) == (size_t)(e - x) && strncmp(num, x, (size_t)(e - x)) == 0) t = &L->banks[i].term;
            }
        }
    }
    return t;
}

static const char *last_word(const char *s)
{
    const char *e = s + strlen(s), *w;
    while (e > s && is_uspace((unsigned char)e[-1])) e--;
    w = e;
    while (w > s && !is_uspace((unsigned char)w[-1])) w--;
    return w;
}

/* Latin-1 text in upper case, written as UTF-8; sharp s becomes SS */
static void sb_upper_latin1(sb *b, const unsigned char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c == 0xdf) { sb_puts(b, "SS"); continue; }
        else if (c == 0xff) { sb_add(b, "\xc5\xb8", 2); continue; }
        else if (c == 0xb5) { sb_add(b, "\xce\x9c", 2); continue; }
        else if (c >= 0xe0 && c <= 0xfe && c != 0xf7) c -= 32;
        {
            unsigned char one = (unsigned char)c;
            sb_latin1(b, &one, 1);
        }
    }
}

/* "NOTE PACKAGE", white space, the shortest run of non-blank characters that is followed by '*' */
static int find_package_note(const unsigned char *d, size_t n, size_t *gs, size_t *gn)
{
    static const char key[] = "NOTE PACKAGE";
    size_t i, kl = sizeof key - 1;
    for (i = 0; i + kl <= n; i++) {
        size_t j, e;
        if (memcmp(d + i, key, kl) != 0) continue;
        j = i + kl;
        if (j >= n || !(d[j] == ' ' || (d[j] >= 9 && d[j] <= 13))) continue;
        while (j < n && (d[j] == ' ' || (d[j] >= 9 && d[j] <= 13))) j++;
        if (j >= n) continue;
        for (e = j + 1; e < n && !(d[e] == ' ' || (d[e] >= 9 && d[e] <= 13)); e++)
            if (d[e] == '*') { *gs = j; *gn = e - j; return 1; }
    }
    return 0;
}

/* ---- text passes over the finished design ---- */

static int is_wordc(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* I/O-cell labels -> package pins: every word [pqij]<digits> is renamed through the map */
static char *rename_signals(const char *txt, const char *const *map)
{
    sb out = {0, 0, 0};
    const char *p = txt;
    while (*p) {
        if (is_wordc((unsigned char)*p) && (p == txt || !is_wordc((unsigned char)p[-1]))) {
            const char *w = p;
            size_t n, k;
            int ok;
            while (is_wordc((unsigned char)*p)) p++;
            n = (size_t)(p - w);
            ok = n >= 2 && (*w == 'p' || *w == 'q' || *w == 'i' || *w == 'j');
            for (k = 1; ok && k < n; k++) ok = w[k] >= '0' && w[k] <= '9';
            if (ok) {
                const char *const *m;
                const char *to = NULL;
                for (m = map; *m; m += 2)
                    if (strlen(m[0]) == n - 1 && strncmp(m[0], w + 1, n - 1) == 0) { to = m[1]; break; }
                sb_add(&out, w, 1);
                if (to) sb_puts(&out, to); else sb_add(&out, w + 1, n - 1);
            } else sb_add(&out, w, n);
        } else {
            const char *w = p;
            if (is_wordc((unsigned char)*p)) while (is_wordc((unsigned char)*p)) p++;
            else p++;
            sb_add(&out, w, (size_t)(p - w));
        }
    }
    sb_add(&out, "", 0);
    return out.p ? out.p : xstrdup("");
}

/* the pin_numbers attribute: every "p<name>:<word>" becomes "p<name>:<name>" */
static char *rename_pin_numbers(const char *txt)
{
    sb out = {0, 0, 0};
    const char *p = txt;
    while (*p) {
        if (*p == 'p' && (p == txt || !is_wordc((unsigned char)p[-1])) && is_wordc((unsigned char)p[1])) {
            /* the name takes as many word characters as leave a ':' and one more word character behind it */
            const char *e = p + 1;
            while (is_wordc((unsigned char)*e)) e++;
            if (*e == ':' && is_wordc((unsigned char)e[1])) {
                const char *v = e + 1;
                while (is_wordc((unsigned char)*v)) v++;
                sb_add(&out, p, (size_t)(e - p) + 1);
                sb_add(&out, p + 1, (size_t)(e - p) - 1);
                p = v;
                continue;
            }
        }
        sb_add(&out, p, 1);
        p++;
    }
    return out.p ? out.p : xstrdup("");
}

static const char *line_end(const char *s) { const char *e = strchr(s, '\n'); return e ? e : s + strlen(s); }

/* ---- the decoder ---- */

void decode(const unsigned char *data, size_t len, const char *top, const char *package, cy_result *R)
{
    cy_jedec J;
    const cy_package *pk, *chosen = NULL, *note_pk = NULL;
    const char *src;
    size_t gs = 0, gn = 0;
    int have_note, npk = 0, lab, i, k;
    const char *dname;
    strlist body = {0, 0, 0}, sig = {0, 0, 0}, keepconst = {0, 0, 0}, racing = {0, 0, 0}, input_cells = {0, 0, 0}, warn = {0, 0, 0};
    intlist ports_in = {0, 0, 0}, ports_out = {0, 0, 0}, ports_inout = {0, 0, 0}, keep = {0, 0, 0}, regpol = {0, 0, 0};
    intlist buried_comb = {0, 0, 0}, free_buried = {0, 0, 0};
    usedmc *mcs;
    int nmcs = 0;
    node *nodes;
    int nnodes = 0;
    char *carrier = NULL;
    struct { const char *clk; char c; int pin; } latkeys[130];
    int nlatkeys = 0;
    struct { int q; const char *ck; char edge; } regs[130];
    int nregs = 0;
    int labpin_pin[130], labpin_lab[130], nlabpin = 0;
    strlist cksorted = {0, 0, 0};
    char *txt;
    cy_kinds K;

    used_pairs.n = 0; reads.n = 0; niregs = 0; used_pins.n = 0;
    jedec_parse(data, len, &J);
    DEV = device_by_name(J.design, J.design_len);
    if (!DEV) {
        sb_puts(&cy_refusal, "DEVICE_NOT_SUPPORTED ");
        sb_latin1(&cy_refusal, J.design, J.design_len);
        refuse_sb();
    }
    dname = DEV->name;
    MOD = &cy_models[DEV->model];
    need("DEVICE", dname, "fuse file");

    /* The package: a Warp fuse file names it ("NOTE PACKAGE <order code>"); --package must agree with the note. */
    for (pk = DEV->packages; pk->code; pk++) npk++;
    if (package) {
        for (pk = DEV->packages; pk->code; pk++)
            if (strcmp(pk->code, package) == 0) chosen = pk;
        if (!chosen) {
            strlist codes = {0, 0, 0};
            sb_printf(&cy_refusal, "PACKAGE_NOT_OF_DEVICE %s has packages ", dname);
            for (pk = DEV->packages; pk->code; pk++) sl_add(&codes, (char *)pk->code);
            sl_sort(&codes);
            for (i = 0; i < codes.n; i++) sb_printf(&cy_refusal, "%s%s", i ? "," : "", codes.v[i]);
            sb_printf(&cy_refusal, ", not %s", package);
            refuse_sb();
        }
    }
    have_note = find_package_note(data, len, &gs, &gn);
    if (have_note) {
        for (pk = DEV->packages; pk->code && !note_pk; pk++) {
            const char *const *oc;
            for (oc = pk->ordercodes; *oc; oc++) {
                size_t q;
                if (strlen(*oc) != gn) continue;
                for (q = 0; q < gn; q++) {
                    unsigned c = data[gs + q];
                    if (c >= 'a' && c <= 'z') c -= 32;
                    if (c != (unsigned char)(*oc)[q]) break;
                }
                if (q == gn) { note_pk = pk; break; }
            }
        }
        if (!note_pk) {
            sb_puts(&cy_refusal, "PACKAGE_NOTE_NOT_OF_DEVICE the fuse file names ");
            sb_upper_latin1(&cy_refusal, data + gs, gn);
            sb_printf(&cy_refusal, ", not an order code of %s", dname);
            refuse_sb();
        }
    }
    if (!chosen) {
        if (note_pk) { chosen = note_pk; src = "note"; }
        else if (npk == 1) { chosen = DEV->packages; src = "single"; }
        else {
            strlist codes = {0, 0, 0};
            sb_printf(&cy_refusal, "PACKAGE_NOT_GIVEN %s is sold in packages ", dname);
            for (pk = DEV->packages; pk->code; pk++) sl_add(&codes, (char *)pk->code);
            sl_sort(&codes);
            for (i = 0; i < codes.n; i++) sb_printf(&cy_refusal, "%s%s", i ? "," : "", codes.v[i]);
            sb_puts(&cy_refusal, ", the fuse file names none; give --package");
            refuse_sb();
            return;
        }
    } else {
        src = "option";
        if (note_pk && note_pk != chosen) {
            sb_puts(&cy_refusal, "PACKAGE_MISMATCH the fuse file names ");
            sb_latin1(&cy_refusal, data + gs, gn);
            sb_printf(&cy_refusal, " (package %s), --package %s", note_pk->code, package);
            refuse_sb();
        }
    }
    if (strcmp(chosen->status, "UNDECIDED") == 0)
        refuse("PACKAGE_MAP_UNDECIDED %s package %s: pin map direction not confirmed by a fit", dname, chosen->code);

    model_decode(MOD, &J, &D);

    /* every routed interconnect input: its multiplexer slot must be a supported one (not checked on the smallest part) */
    if (MOD->check_qf) {
        char kind[32];
        sprintf(kind, "PIM_SLOT_C%s", MOD->name + 1);
        for (i = 0; i < MOD->nfields; i++) {
            const cy_field *f = &MOD->fields[i];
            const unsigned char *b;
            int n;
            if (f->item != IT_PIM) continue;
            n = field_bits(MOD, &J, f, &b);
            for (k = 0; k < MOD->nin; k++) {
                char code[8], where[48], slot[16];
                int q, s = -1;
                for (q = 0; q < MOD->codew && MOD->codew * k + q < n; q++) code[q] = (char)('0' + b[MOD->codew * k + q]);
                code[q] = 0;
                if (strcmp(code, MOD->unused) == 0) continue;
                for (q = 0; q < MOD->nslots; q++)
                    if (strcmp(code, MOD->mux[q]) == 0) { s = q; break; }
                sprintf(slot, "%d", s);
                sprintf(where, "LAB %d input %d", f->lab, k + 1);
                need(kind, slot, where);
            }
        }
    }

    /* clock polarity fields; the clock pins in text order of their numbers */
    for (i = 0; DEV->clocks[i]; i++) sl_add(&cksorted, xsprintf("%d", DEV->clocks[i]));
    sl_sort(&cksorted);
    for (i = 0; i < cksorted.n; i++)
        need("CIPOL", cfg_get(&D, IT_CIPOL, cksorted.v[i]), xsprintf("pin %s", cksorted.v[i]));
    for (lab = 1; lab <= MOD->nlabs; lab++)
        for (i = 0; i < cksorted.n; i++) {
            char *a = xsprintf("%d %s", lab, cksorted.v[i]);
            need("LABCLKPOL", cfg_get(&D, IT_LABCLKPOL, a), xsprintf("LAB %d pin %s", lab, cksorted.v[i]));
        }
    for (i = 0; i < D.ncfg; i++) {
        if (D.cfg[i].item == IT_FPOL) need("FPOL", D.cfg[i].value, D.cfg[i].args);
        if (D.cfg[i].item == IT_IREG2) need("IREG2", D.cfg[i].value, xsprintf("pin %s", D.cfg[i].args));
    }

    /* Input cells: a dedicated input may be registered or latched by a clock pin (double-registered through j<n>).
       The clock is selected by the input's clock-select field; the edge / level by that clock pin's polarity field. */
    {
        strlist args = {0, 0, 0};
        for (i = 0; i < D.ncfg; i++)
            if (D.cfg[i].item == IT_IREG2) sl_add(&args, (char *)D.cfg[i].args);
        sl_sort(&args);
        for (i = 0; i < args.n; i++) {
            const char *v = cfg_get(&D, IT_IREG2, args.v[i]), *ck;
            int n;
            char lv;
            if (strcmp(v, "COMB") == 0) continue;
            n = atoi(args.v[i]);
            if (n == DEV->vpp) refuse("VPP_PIN_INPUT_CELL pin %d is the %s VPP pin and carries an input cell %s", n, dname, v);
            ck = cfg_get(&D, IT_CI, args.v[i]);
            if (!ck || !*ck) ck = cfg_get(&D, IT_CI2, args.v[i]);
            if (!ck)
                refuse("INPUT_CELL_ON_CLOCK_PIN_NO_CI pin %s carries an input cell %s and %s has no clock-select field for it (vendor model clocks it from itself)", args.v[i], v, dname);
            need("CI", ck, xsprintf("pin %s", args.v[i]));
            lv = strcmp(cfg_get(&D, IT_CIPOL, ck), "AL") == 0 ? '0' : '1';
            if (lv == '0' && strcmp(family(), "c373") == 0)
                refuse("INPUT_CELL_POLARITY_UNMODELLED pin %s input cell %s on clock pin %s with CIPOL AL: the %s vendor model clocks input cells without the polarity cell", args.v[i], v, ck, dname);
            iregs[niregs].pin = n; iregs[niregs].kind = v; iregs[niregs].ck = ck; iregs[niregs].lv = lv;
            niregs++;
            sl_add(&input_cells, xsprintf("p%d %s clock p%s level %c", n, v, ck, lv));
        }
    }

    /* the macrocells in use: any macrocell with a product term, a register / latch, or an enabled output */
    mcs = xmalloc((size_t)(MOD->nlabs * 16 + 1) * sizeof *mcs);
    for (lab = 1; lab <= MOD->nlabs; lab++)
        for (i = 0; i < 16; i++) {
            const cy_mc *mc = &D.labs[lab].mcs[i];
            char p[16];
            const char *reg, *pol, *oe;
            sprintf(p, "%d", mc->pin);
            reg = cfg_get(&D, IT_REG2, p);
            pol = cfg_get(&D, IT_POL, p);
            oe = cfg_get(&D, IT_OE2, p);
            if (!pol) pol = "AH";
            if (!oe) oe = "GND";
            if (mc->nterms == 0 && strcmp(reg, "COMB") == 0 && strcmp(oe, "GND") == 0) continue;
            if (mc->buried) {
                const char *ip = cfg_get(&D, IT_IP, p);
                need("BURIED", reg, xsprintf("LAB %d mc%d node %s", lab, i, p));
                need("IP", ip && strcmp(ip, p) == 0 ? "NODE" : "PIN", xsprintf("node %s", p));
            }
            il_add_unique(&used_pins, mc->pin);
            mcs[nmcs].lab = lab; mcs[nmcs].m = i; mcs[nmcs].mc = mc; mcs[nmcs].reg = reg; mcs[nmcs].pol = pol; mcs[nmcs].oe = oe;
            nmcs++;
        }

    /* input cells first */
    {
        int order[8], t;
        for (i = 0; i < niregs; i++) order[i] = i;
        for (i = 0; i < niregs; i++)
            for (k = i + 1; k < niregs; k++)
                if (iregs[order[k]].pin < iregs[order[i]].pin) { t = order[i]; order[i] = order[k]; order[k] = t; }
        for (i = 0; i < niregs; i++) {
            const ireg *r = &iregs[order[i]];
            il_add_unique(&ports_in, atoi(r->ck));
            il_add_unique(&ports_in, r->pin);
            if (strcmp(r->kind, "LAT") == 0) {
                sl_add(&body, xsprintf("\tPROCESS (p%s, p%d) BEGIN IF (p%s = '%c') THEN i%d <= p%d; END IF; END PROCESS;", r->ck, r->pin, r->ck, r->lv, r->pin, r->pin));
                sl_add(&sig, xsprintf("i%d", r->pin));
            } else if (strcmp(r->kind, "REG") == 0) {
                sl_add(&body, xsprintf("\tPROCESS (p%s) BEGIN IF (p%s'event AND p%s = '%c') THEN i%d <= p%d; END IF; END PROCESS;", r->ck, r->ck, r->ck, r->lv, r->pin, r->pin));
                sl_add(&sig, xsprintf("i%d", r->pin));
            } else if (strcmp(r->kind, "DBLREG") == 0) {
                sl_add(&body, xsprintf("\tPROCESS (p%s) BEGIN IF (p%s'event AND p%s = '%c') THEN j%d <= p%d; END IF; END PROCESS;", r->ck, r->ck, r->ck, r->lv, r->pin, r->pin));
                sl_add(&body, xsprintf("\tPROCESS (p%s) BEGIN IF (p%s'event AND p%s = '%c') THEN i%d <= j%d; END IF; END PROCESS;", r->ck, r->ck, r->ck, r->lv, r->pin, r->pin));
                sl_add(&sig, xsprintf("j%d", r->pin));
                sl_add(&sig, xsprintf("i%d", r->pin));
            } else
                refuse("IREG2_NOT_MODELLED IREG2=%s at pin %d", r->kind, r->pin);
        }
    }

    /* Keep forms.  Warp folds a register or latch whose next state is a constant (or its own output) into a constant and
       loses its power-up value.  Written through a combinational macrocell q<x> that drives a pin -- a node Warp keeps --
       the same value survives: constant 0 as (q<x> AND NOT (F)), constant 1 as (q<x> OR NOT (F)), F = q<x>'s own sum of
       products; a D register also carries its own feedback so that equal registers are not merged.  q<x> is the
       combinational pin macrocell with the fewest literals: one product of two or more literals preferred. */
    nodes = xmalloc((size_t)(nmcs + 1) * sizeof *nodes);
    for (i = 0; i < nmcs; i++) {
        const cy_mc *mc = mcs[i].mc;
        int comb = strcmp(mcs[i].reg, "COMB") == 0, has_true = 0, nl = 0;
        if (comb && mc->buried && mc->nterms) { il_add(&buried_comb, mc->pin); continue; }
        for (k = 0; k < mc->nterms; k++) { has_true |= mc->terms[k].kind == T_TRUE; nl += mc->terms[k].kind == T_TRUE ? 4 : mc->terms[k].nlits; }
        if (comb && mc->nterms && !has_true && strcmp(mcs[i].oe, "GND") != 0 && nl >= 2) {
            nodes[nnodes].single = mc->nterms == 1 ? 0 : 1;
            nodes[nnodes].nl = nl;
            nodes[nnodes].pin = mc->pin;
            nodes[nnodes].text = sop_text(mc->terms, mc->nterms);
            nnodes++;
        }
    }
    if (nnodes > 1) qsort(nodes, (size_t)nnodes, sizeof *nodes, node_cmp);
    for (lab = 1; lab <= MOD->nlabs; lab++)
        for (i = 0; i < 16; i++) {
            const cy_mc *mc = &D.labs[lab].mcs[i];
            if (mc->buried && !il_has(&used_pins, mc->pin)) il_add(&free_buried, mc->pin);
        }

    for (i = 0; i < nmcs; i++) {
        const cy_mc *mc = mcs[i].mc;
        const cy_lab *L = &D.labs[mcs[i].lab];
        const char *reg = mcs[i].reg, *pol = mcs[i].pol, *oe = mcs[i].oe, *oek;
        int p = mc->pin, is_comb = strcmp(reg, "COMB") == 0;
        char *where = xsprintf("LAB %d mc%d %s %d", mcs[i].lab, mcs[i].m, mc->buried ? "node" : "pin", p);
        char pstr[16], *s;
        const node *carrier_node = NULL;
        sprintf(pstr, "%d", p);
        lab = mcs[i].lab;
        labpin_pin[nlabpin] = p; labpin_lab[nlabpin] = lab; nlabpin++;
        need("REG2", reg, where);
        if (!mc->buried) need("POL", pol, where);
        if (strcmp(oe, "VCC") == 0) oek = "VCC";
        else if (strcmp(oe, "GND") == 0) oek = "GND";
        else {
            int b = atoi(last_word(oe)), o = DEV->oe0 + 4 * lab;
            oek = (b == o || b == o + 2) && strlen(last_word(oe)) == strspn(last_word(oe), "0123456789") ? "BANK_A" : "BANK_B";
        }
        if (!mc->buried) need("OE2", oek, where);
        s = sop_text(mc->terms, mc->nterms);
        sl_add(&sig, xsprintf("q%d", p));
        if (is_comb) {
            sl_add(&body, xsprintf("\tq%d <= %s;", p, s));
        } else {
            const char *ck, *lvs;
            const cy_term *ar, *ap;
            char *rst = NULL, *pst = NULL, *core;
            char lv;
            strlist sens = {0, 0, 0}, allw = {0, 0, 0};
            sb sl = {0, 0, 0};
            char *own = xsprintf("(q%d)", p);
            int is_lat = strcmp(reg, "LAT") == 0, is_d = strcmp(reg, "D") == 0;
            /* the node that may carry this register: it must not read the register, itself, or the register's clock pin */
            {
                const char *cko = cfg_get(&D, IT_CLK, pstr);
                char qp[16], pc[24];
                sprintf(qp, "q%d", p);
                sprintf(pc, "p%s", cko ? cko : "");
                for (k = 0; k < nnodes && !carrier_node; k++) {
                    char qn[16];
                    sprintf(qn, "q%d", nodes[k].pin);
                    if (find_word(nodes[k].text, qp) || find_word(nodes[k].text, qn)) continue;
                    if (cko && *cko && find_word(nodes[k].text, pc)) continue;
                    carrier_node = &nodes[k];
                }
            }
            if (strcmp(s, own) == 0) {
                /* a register holding its own output keeps its power-up value: written (q OR (q<x> AND NOT (F))) */
                if (!is_d) refuse("KEEP_HOLD_KIND_NOT_SUPPORTED REG2=%s holding its own output at %s", reg, where);
                if (!carrier_node)
                    refuse("KEEP_HOLD_NO_NODE register at %s holds its own output and the design has no combinational macrocell of >= 2 literals that does not read it", where);
                s = xsprintf("(q%d OR (q%d AND NOT (%s)))", p, carrier_node->pin, carrier_node->text);
                sl_add(&keepconst, xsprintf("q%d hold via q%d", p, carrier_node->pin));
            }
            if (strcmp(s, "'0'") == 0 || strcmp(s, "'1'") == 0) {
                int zero = s[1] == '0';
                if (!is_d && !is_lat) refuse("KEEP_CONSTANT_KIND_NOT_SUPPORTED REG2=%s with a constant input at %s", reg, where);
                if (carrier_node) {
                    if (is_lat) {
                        /* two constant latches on the same enable with the same constant would be one equation */
                        const char *lk = cfg_get(&D, IT_CLK, pstr);
                        for (k = 0; k < nlatkeys; k++)
                            if (strcmp(latkeys[k].clk, lk) == 0 && latkeys[k].c == s[1])
                                refuse("KEEP_CONSTANT_LATCH_SHARED latch at %s has the same enable and constant as the latch at pin %d", where, latkeys[k].pin);
                        latkeys[nlatkeys].clk = lk; latkeys[nlatkeys].c = s[1]; latkeys[nlatkeys].pin = p; nlatkeys++;
                        s = zero ? xsprintf("(q%d AND NOT (%s))", carrier_node->pin, carrier_node->text)
                                 : xsprintf("(q%d OR NOT (%s))", carrier_node->pin, carrier_node->text);
                    } else {
                        s = zero ? xsprintf("((q%d AND NOT (%s)) AND q%d)", carrier_node->pin, carrier_node->text, p)
                                 : xsprintf("((q%d OR NOT (%s)) OR q%d)", carrier_node->pin, carrier_node->text, p);
                    }
                    sl_add(&keepconst, xsprintf("q%d via q%d%s", p, carrier_node->pin, is_lat ? " (latch plain)" : ""));
                } else if (is_d) {
                    /* no pin node: the constant is carried by an added internal signal that Warp places in a free
                       buried macrocell (attribute synthesis_off keeps it a macrocell) */
                    if (free_buried.n == 0)
                        refuse("KEEP_CONSTANT_NO_FREE_NODE register at %s: no combinational pin macrocell of >= 2 literals and no free buried macrocell for a carrier", where);
                    if (!carrier) {
                        int nd = 0, a, b2, t[5];
                        for (k = 0; DEV->dedicated[k]; k++) t[nd++] = DEV->dedicated[k];
                        if (nd < 2) refuse("KEEP_CONSTANT_NO_CARRIER_INPUTS %s has fewer than two dedicated input pins for a carrier", dname);
                        a = t[0]; b2 = t[1];
                        need("SOURCE", "input", "carrier");
                        il_add_unique(&reads, a); il_add_unique(&reads, b2);
                        carrier = xsprintf("(p%d AND p%d)", a, b2);
                    }
                    s = zero ? xsprintf("((kc AND NOT (%s)) AND q%d)", carrier, p) : xsprintf("((kc OR NOT (%s)) OR q%d)", carrier, p);
                    sl_add(&keepconst, xsprintf("q%d via kc (buried carrier)", p));
                } else if (buried_comb.n) {
                    sb_printf(&cy_refusal, "KEEP_CONSTANT_BURIED_NODE_NOT_SUPPORTED register at %s: the only combinational macrocells are buried (", where);
                    for (k = 0; k < buried_comb.n; k++) sb_printf(&cy_refusal, "%s%d", k ? "," : "", buried_comb.v[k]);
                    sb_puts(&cy_refusal, "), not pin nodes");
                    refuse_sb();
                } else
                    refuse("KEEP_CONSTANT_NO_NODE register at %s has a constant next state and the design has no combinational macrocell of >= 2 literals to carry it", where);
            }
            ck = cfg_get(&D, IT_CLK, pstr);
            need("CLK", ck, where);
            il_add_unique(&ports_in, atoi(ck));
            ar = bank_term(L, 2 * lab);
            ap = bank_term(L, 2 * lab - 1);
            need("AR", !ar || ar->kind == T_FALSE ? "FALSE" : "TERM", xsprintf("LAB %d", lab));
            need(DEV->model == 0 ? "AP" : xsprintf("AP_%s", family()), !ap || ap->kind == T_FALSE ? "FALSE" : "TERM", xsprintf("LAB %d", lab));
            if ((ar && ar->kind == T_TRUE) || (ap && ap->kind == T_TRUE)) refuse("CONSTANT_TRUE_RESET LAB %d", lab);
            if (ar && ar->kind != T_FALSE) rst = sop_text(ar, 1);
            if (ap && ap->kind != T_FALSE) pst = sop_text(ap, 1);
            sl_add(&sens, xsprintf("p%s", ck));
            if (rst) words(rst, &sens, 1);
            if (pst) words(pst, &sens, 1);
            lvs = cfg_get(&D, IT_LABCLKPOL, xsprintf("%d %s", lab, ck));
            lv = lvs && strcmp(lvs, "AL") == 0 ? '0' : '1';
            if (is_lat) {
                words(s, &sens, 1);
                core = xsprintf("IF (p%s = '%c') THEN q%d <= %s; END IF;", ck, lv, p, s);
            } else if (is_d) {
                core = xsprintf("IF (p%s'event AND p%s = '%c') THEN q%d <= %s; END IF;", ck, ck, lv, p, s);
            } else {
                core = xsprintf("IF (p%s'event AND p%s = '%c') THEN q%d <= q%d XOR (%s); END IF;", ck, ck, lv, p, p, s);
            }
            if (!is_lat) { regs[nregs].q = p; regs[nregs].ck = ck; regs[nregs].edge = lv; nregs++; }
            words(s, &allw, 0);
            if (rst) words(rst, &allw, 0);
            if (pst) words(pst, &allw, 0);
            if (sl_has(&allw, xsprintf("p%s", ck))) sl_add(&racing, xsprintf("q%d clock p%s", p, ck));
            if (pst) core = xsprintf("IF (%s) = '1' THEN q%d <= '1'; ELS%s", pst, p, core);   /* reset has priority over preset */
            if (rst) core = xsprintf("IF (%s) = '1' THEN q%d <= '0'; ELS%s", rst, p, core);
            sl_sort(&sens);
            for (k = 0; k < sens.n; k++) sb_printf(&sl, "%s%s", k ? ", " : "", sens.v[k]);
            sl_add(&body, xsprintf("\tPROCESS (%s) BEGIN %s END PROCESS;", sl.p, core));
        }
        if (!is_comb && strcmp(pol, "AL") == 0 && !mc->buried) il_add(&regpol, p);
        if (!is_comb) il_add_unique(&keep, p);
        if (strcmp(oe, "GND") != 0 && !mc->buried) {
            char *val = strcmp(pol, "AH") == 0 ? xsprintf("q%d", p) : xsprintf("NOT q%d", p);
            if (strcmp(oe, "VCC") == 0) {
                sl_add(&body, xsprintf("\tp%d <= %s;", p, val));
            } else {
                const char *bank = last_word(oe);
                const cy_term *t = strlen(bank) == strspn(bank, "0123456789") && *bank ? bank_term(L, atoi(bank)) : NULL;
                if (!t || t->kind != T_LITS) refuse("CONSTANT_ENABLE_BANK %s at %s", bank, where);
                sl_add(&body, xsprintf("\tp%d <= %s WHEN (%s) = '1' ELSE 'Z';", p, val, sop_text(t, 1)));
            }
            il_add_unique(&ports_out, p);
        }
    }

    /* ports: pins read by a term or used as a clock are inputs; driven pins are outputs; both -> INOUT */
    for (i = 0; i < reads.n; i++) il_add_unique(&ports_in, reads.v[i]);
    for (i = 0; i < ports_in.n; i++)
        if (il_has(&ports_out, ports_in.v[i])) il_add_unique(&ports_inout, ports_in.v[i]);
    {
        intlist ins = {0, 0, 0}, allp = {0, 0, 0};
        sb out = {0, 0, 0};
        int first = 1;
        for (i = 0; i < ports_in.n; i++)
            if (!il_has(&ports_out, ports_in.v[i])) il_add(&ins, ports_in.v[i]);
        il_sort(&ins); il_sort(&ports_out);
        if (ins.n + ports_out.n == 0) refuse("EMPTY_DESIGN");
        for (i = 0; i < ins.n; i++) il_add(&allp, ins.v[i]);
        for (i = 0; i < ports_out.n; i++) il_add(&allp, ports_out.v[i]);
        sb_printf(&out, "-- %s design recovered from a JEDEC fuse file by the cy370 decoder tool (DARKSOFT).\n", dname);
        sb_printf(&out, "-- Device %s, package code %s (", DEV->fullname, chosen->code);
        for (i = 0; chosen->ordercodes[i]; i++) sb_printf(&out, "%s%s", i ? " " : "", chosen->ordercodes[i]);
        sb_puts(&out, "); every signal is fixed to its package pin.\n");
        if (DEV->buried_note) sb_puts(&out, "-- Buried macrocells are internal signals q<node> (node numbers of the device); they have no pin.\n");
        sb_puts(&out, "library ieee;\nuse ieee.std_logic_1164.all;\n\n");
        sb_printf(&out, "ENTITY %s IS\n\tPORT (", top);
        for (i = 0; i < ins.n; i++) { sb_printf(&out, "%sp%d : IN std_logic", first ? "" : ";\n\t      ", ins.v[i]); first = 0; }
        for (i = 0; i < ports_out.n; i++) {
            sb_printf(&out, "%sp%d : %s std_logic", first ? "" : ";\n\t      ", ports_out.v[i], il_has(&ports_inout, ports_out.v[i]) ? "INOUT" : "OUT");
            first = 0;
        }
        sb_puts(&out, ");\n");
        sb_printf(&out, "ATTRIBUTE pin_numbers of %s:ENTITY IS\n\t  ", top);
        for (i = 0; i < allp.n; i += 8) {
            if (i) sb_puts(&out, "\n\t& ");
            sb_puts(&out, "\"");
            for (k = i; k < i + 8 && k < allp.n; k++) sb_printf(&out, "%sp%d:%d", k > i ? " " : "", allp.v[k], allp.v[k]);
            sb_puts(&out, " \"");
        }
        sb_puts(&out, ";\n");
        sb_printf(&out, "END %s;\n\nARCHITECTURE recovered OF %s IS\n", top, top);
        if (carrier) sl_add(&sig, xstrdup("kc"));
        if (sig.n) {
            sb_puts(&out, "\tSIGNAL ");
            for (i = 0; i < sig.n; i++) sb_printf(&out, "%s%s", i ? ", " : "", sig.v[i]);
            sb_puts(&out, ": std_logic;\n");
        }
        if (carrier) sb_puts(&out, "\tATTRIBUTE synthesis_off OF kc : SIGNAL IS true;\n");
        /* a register or latch keeps the polarity it has in the fuse file (Warp attribute polarity, value pl_keep) */
        il_sort(&keep);
        for (i = 0; i < keep.n; i++) sb_printf(&out, "\tATTRIBUTE polarity OF q%d : SIGNAL IS pl_keep;\n", keep.v[i]);
        sb_puts(&out, "BEGIN\n");
        if (carrier) sb_printf(&out, "\tkc <= %s;\n", carrier);
        for (i = 0; i < body.n; i++) sb_printf(&out, "%s\n", body.v[i]);
        sb_puts(&out, "END recovered;\n");
        txt = out.p;
        R->ports = allp.n;
        R->macrocells = nmcs;

        /* Warnings (the design is written; Warp may refuse to fit it).
           1. a one-literal combinational copy of a register, in a block whose registers use that register's clock pin
              on the other edge */
        for (i = 0; i < body.n; i++) {
            const char *l = body.v[i], *x, *y, *z;
            int a, b2, neg = 0, ri = -1, la = -1, same = 0, other = 0;
            if (strncmp(l, "\tq", 2) != 0) continue;
            x = l + 2;
            while (*x >= '0' && *x <= '9') x++;
            if (x == l + 2 || strncmp(x, " <= (", 5) != 0) continue;
            y = x + 5;
            if (strncmp(y, "NOT ", 4) == 0) { neg = 1; y += 4; }
            if (*y != 'q') continue;
            z = y + 1;
            while (*z >= '0' && *z <= '9') z++;
            if (z == y + 1 || strcmp(z, ");") != 0) continue;
            a = atoi(l + 2); b2 = atoi(y + 1);
            for (k = 0; k < nregs; k++) if (regs[k].q == b2) ri = k;
            for (k = 0; k < nlabpin; k++) if (labpin_pin[k] == a) la = labpin_lab[k];
            if (ri < 0 || la < 0) continue;
            for (k = 0; k < nregs; k++) {
                int lq = -1, q;
                if (strcmp(regs[k].ck, regs[ri].ck) != 0) continue;
                for (q = 0; q < nlabpin; q++) if (labpin_pin[q] == regs[k].q) lq = labpin_lab[q];
                if (lq != la) continue;
                if (regs[k].edge == regs[ri].edge) same = 1; else other = 1;
            }
            if (other && !same)
                sl_add(&warn, xsprintf("POLARITY_COPY_CONFLICT q%d <= %s(q%d): LAB %d registers on clock p%s use the other edge; Warp may refuse to fit this design (Preplaced signals use conflicting clock polarity)", a, neg ? "NOT " : "", b2, la, regs[ri].ck));
        }
        /* 2. a combinational macrocell holding itself on a clock-pin literal (a transparent latch written as a loop);
              with its pin inverted the design is refused further down */
        {
            int inv_a = 0, inv_c = 0, have_inv = 0;
            for (i = 0; i < body.n; i++) {
                const char *l = body.v[i], *x, *e;
                int a;
                char h1[24], t2[24];
                if (strncmp(l, "\tq", 2) != 0) continue;
                x = l + 2;
                while (*x >= '0' && *x <= '9') x++;
                if (x == l + 2 || strncmp(x, " <= ", 4) != 0 || l[strlen(l) - 1] != ';') continue;
                a = atoi(l + 2);
                sprintf(h1, "(q%d AND ", a);
                sprintf(t2, " AND q%d)", a);
                e = x + 4;
                while (*e) {
                    const char *m = NULL, *d0 = NULL, *d1 = NULL;
                    if (strncmp(e, h1, strlen(h1)) == 0) {
                        const char *y = e + strlen(h1);
                        if (strncmp(y, "NOT ", 4) == 0) y += 4;
                        if (*y == 'p') {
                            d0 = y + 1; d1 = d0;
                            while (*d1 >= '0' && *d1 <= '9') d1++;
                            if (d1 > d0 && *d1 == ')') m = d1 + 1;
                        }
                    }
                    if (!m && *e == '(') {
                        const char *y = e + 1;
                        if (strncmp(y, "NOT ", 4) == 0) y += 4;
                        if (*y == 'p') {
                            d0 = y + 1; d1 = d0;
                            while (*d1 >= '0' && *d1 <= '9') d1++;
                            if (d1 > d0 && strncmp(d1, t2, strlen(t2)) == 0) m = d1 + strlen(t2);
                        }
                    }
                    if (m) {
                        int c = atoi(d0);
                        if (in_zlist(DEV->clocks, c)) {
                            char pre[48];
                            int inverted = 0, q;
                            sprintf(pre, "\tp%d <= NOT q%d", a, a);
                            for (q = 0; q < body.n; q++)
                                if (strncmp(body.v[q], pre, strlen(pre)) == 0 && !is_wordc((unsigned char)body.v[q][strlen(pre)])) inverted = 1;
                            if (inverted) { if (!have_inv) { inv_a = a; inv_c = c; have_inv = 1; } }
                            else
                                sl_add(&warn, xsprintf("CLOCK_PIN_HOLD_LOOP q%d holds itself on clock pin p%d (a transparent latch implemented as a combinational loop); Warp may refuse to fit this design (Preplaced signals use conflicting clock polarity)", a, c));
                        }
                        e = m;
                    } else e++;
                }
            }

            /* Refusals read on the finished text. */
            kinds_of(txt, &K);
            if (kinds_get(&K, "CYCLE_HOLD_PAD_OE"))
                refuse("CYCLE_HOLD_PAD_OE a cycle of combinational macrocells with a hold-only member and a pad read feeds an output enable: Warp may store the hold node complemented when it fits the recovered design");
            /* A falling-edge register fed by one dedicated / clock input literal that no other line reads, driving its
               pin under an enable that reads the register itself: Warp turns the input into a falling-edge input
               register, which the simulation model of this family does not represent. */
            if (strcmp(family(), "c373") == 0) {
                for (i = 0; i < body.n; i++) {
                    const char *l = body.v[i], *c0, *c1, *x, *y, *a0, *a1, *n0, *n1;
                    int c, a, n, others = 0, q;
                    char pw[24], pre[48];
                    c0 = strncmp(l, "\tPROCESS (p", 11) == 0 ? l + 11 : NULL;
                    if (!c0) continue;
                    c1 = c0; while (*c1 >= '0' && *c1 <= '9') c1++;
                    if (c1 == c0) continue;
                    c = atoi(c0);
                    x = xsprintf(") BEGIN IF (p%.*s'event AND p%.*s = '0') THEN q", (int)(c1 - c0), c0, (int)(c1 - c0), c0);
                    if (strncmp(c1, x, strlen(x)) != 0) continue;
                    a0 = c1 + strlen(x); a1 = a0; while (*a1 >= '0' && *a1 <= '9') a1++;
                    if (a1 == a0 || strncmp(a1, " <= (", 5) != 0) continue;
                    y = a1 + 5;
                    if (strncmp(y, "NOT ", 4) == 0) y += 4;
                    if (*y != 'p') continue;
                    n0 = y + 1; n1 = n0; while (*n1 >= '0' && *n1 <= '9') n1++;
                    if (n1 == n0 || strcmp(n1, "); END IF; END PROCESS;") != 0) continue;
                    a = atoi(a0); n = atoi(n0);
                    if (!in_zlist(DEV->dedicated, n) && !in_zlist(DEV->clocks, n)) continue;
                    sprintf(pw, "p%d", n);
                    for (q = 0; q < body.n; q++)
                        if (strcmp(body.v[q], l) != 0 && find_word(body.v[q], pw)) others = 1;
                    if (carrier && find_word(carrier, pw)) others = 1;
                    if (others) continue;
                    sprintf(pre, "\tp%d <= ", a);
                    for (q = 0; q < body.n; q++) {
                        const char *d = body.v[q], *w, *oe_s;
                        size_t dl = strlen(d);
                        static const char suf[] = ") = '1' ELSE 'Z';";
                        char qa[24];
                        if (strncmp(d, pre, strlen(pre)) != 0) continue;
                        w = d + strlen(pre);
                        if (strncmp(w, "NOT ", 4) == 0) w += 4;
                        sprintf(qa, "q%d WHEN (", a);
                        if (strncmp(w, qa, strlen(qa)) != 0) continue;
                        oe_s = w + strlen(qa);
                        if (dl < sizeof suf - 1 || strcmp(d + dl - (sizeof suf - 1), suf) != 0 || d + dl - (sizeof suf - 1) < oe_s) continue;
                        {
                            char *oe_t = xmalloc(dl + 1);
                            size_t on = (size_t)(d + dl - (sizeof suf - 1) - oe_s);
                            memcpy(oe_t, oe_s, on); oe_t[on] = 0;
                            sprintf(qa, "q%d", a);
                            if (find_word(oe_t, qa))
                                refuse("REFIT_INPUT_REGISTER_CONVERSION_c373 register q%d <= p%d on the falling edge of p%d, p%d read by no other term, q%d driving its pin under an enable that reads q%d: Warp would turn p%d into a falling-edge input register, which the %s simulation model does not represent", a, n, c, n, a, a, n, dname);
                        }
                        break;
                    }
                }
            }
            /* a latch whose input reads the latch itself */
            if (!have_inv && kinds_get(&K, "LATCH_SELF_FEEDBACK"))
                refuse("LATCH_SELF_FEEDBACK a latch's input reads the latch on %d cell(s): Warp may store the self-feedback latch node complemented when it fits the recovered design", kinds_get(&K, "LATCH_SELF_FEEDBACK"));
            /* every construct kind of the design must be a supported one */
            {
                strlist unw = {0, 0, 0};
                for (i = 0; i < K.n; i++) {
                    const char *const *s;
                    int ok = 0;
                    if (strcmp(K.name[i], "LATCH_SELF_FEEDBACK") == 0) continue;
                    for (s = cy_kinds_supported; *s; s++)
                        if (strcmp(*s, K.name[i]) == 0) ok = 1;
                    if (!ok || strcmp(K.name[i], "UNPARSED_LINE") == 0) sl_add(&unw, K.name[i]);
                }
                sl_sort(&unw);
                if (have_inv && unw.n == 0)
                    refuse("CLOCK_PIN_HOLD_LOOP_INVERTED q%d holds itself on clock pin p%d and drives its pin inverted: Warp may store the cell complemented when it fits the recovered design", inv_a, inv_c);
                if (unw.n) {
                    sb_puts(&cy_refusal, "STRUCTURE_NOT_SUPPORTED");
                    for (i = 0; i < unw.n; i++) sb_printf(&cy_refusal, " %s", unw.v[i]);
                    refuse_sb();
                }
            }
        }

        /* package pins: signal names and the pin_numbers attribute; buried nodes keep their numbers */
        if (chosen->map) {
            char *t1 = rename_signals(txt, chosen->map);
            txt = rename_pin_numbers(t1);
        }
        sb_puts(&R->text, txt);
        for (i = 0; i < allp.n; i++) {
            char lbl[16];
            const char *to = lbl;
            sprintf(lbl, "%d", allp.v[i]);
            if (chosen->map) {
                const char *const *m;
                for (m = chosen->map; *m; m += 2)
                    if (strcmp(m[0], lbl) == 0) { to = m[1]; break; }
            }
            sb_printf(&R->pinmap, "%s %s\n", to, lbl);
        }
    }

    /* the report */
    sb_printf(&R->report, "PACKAGE %s source %s\n", chosen->code, src);
    used_lines(&R->report);
    for (i = 0; i < racing.n; i++) sb_printf(&R->report, "RACING %s\n", racing.v[i]);
    for (i = 0; i < keepconst.n; i++) sb_printf(&R->report, "KEEP_CONSTANT %s\n", keepconst.v[i]);
    for (i = 0; i < input_cells.n; i++) sb_printf(&R->report, "INPUT_CELL %s\n", input_cells.v[i]);
    for (i = 0; i < warn.n; i++) sb_printf(&R->report, "WARNING %s\n", warn.v[i]);
    sb_puts(&R->report, "INVERTED_REGISTER_PINS ");
    for (i = 0; i < regpol.n; i++) {
        char lbl[16];
        const char *to = lbl;
        sprintf(lbl, "%d", regpol.v[i]);
        if (chosen->map) {
            const char *const *m;
            for (m = chosen->map; *m; m += 2)
                if (strcmp(m[0], lbl) == 0) { to = m[1]; break; }
        }
        sb_printf(&R->report, "%s%s", i ? "," : "", to);
    }
    sb_puts(&R->report, "\n");
    (void)line_end;
}
