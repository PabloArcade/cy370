/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fuse model of the FLASH370 logic blocks: fuses -> product terms in terms of pins and macrocell feedbacks.

   A logic block has 16 macrocells and 80 product-term slots plus six bank terms (four output enables, asynchronous reset,
   asynchronous preset).  Its inputs come from the programmable interconnect: each input has a multiplexer whose code (4 or
   5 fuses) selects one source -- a pin, or the feedback of a macrocell -- or marks the input unused.
   A product-term row has two columns per input (true, complement); a '0' fuse connects the literal.  A row of all '1' is
   constant TRUE; a row with both columns of one input connected is constant FALSE.
   The steering field gives each macrocell 12 fuses; each selects one product-term slot or a pair of slots ('1' = selected).
   Configuration fields select one option each: the fuses read as a binary number (first fuse most significant) index the
   option list. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cy370.h"

/* The fuses of a field.  A file may hold fewer fuses than the part has (only the smallest part is read without a fuse-count
   check); the field is then cut short, exactly as far as fuses exist.  Returns the number of fuses available. */
int field_bits(const cy_model *m, const cy_jedec *j, const cy_field *f, const unsigned char **bits)
{
    long avail = j->nfuses - f->start;
    (void)m;
    if (avail < 0) avail = 0;
    if (avail > f->width) avail = f->width;
    *bits = j->fuses + (avail ? f->start : 0);
    return (int)avail;
}

const char *cfg_get(const cy_decoded *d, int item, const char *args)
{
    int i;
    for (i = 0; i < d->ncfg; i++)
        if (d->cfg[i].item == item && strcmp(d->cfg[i].args, args) == 0) return d->cfg[i].value;
    return NULL;
}

static int lit_cmp(const void *a, const void *b) { return strcmp(a, b); }

/* One product-term row.  src[i] = source of block input i (0: unused input). */
static void row_term(const cy_model *m, const unsigned char *bits, int n, const short *src, cy_term *t)
{
    int i, all1 = 1, nl = 0;
    char lits[40][16];
    for (i = 0; i < n; i++)
        if (bits[i] != 1) { all1 = 0; break; }
    t->nlits = 0; t->lits = NULL;
    if (all1) { t->kind = T_TRUE; return; }
    for (i = 0; i < n / 2; i++) {
        int tr = bits[2 * i], co = bits[2 * i + 1];
        if (tr == 0 && co == 0) { t->kind = T_FALSE; return; }
        if (tr == 0 || co == 0) {
            int s = src[i];
            char *p = lits[nl];
            if (s == 0) refuse("LITERAL_ON_UNUSED_PIM_INPUT input %d", i + 1);
            if (tr != 0) *p++ = '/';
            if (s > 0) sprintf(p, "pin%d", s);
            else if (m->buried_min && -s >= m->buried_min) sprintf(p, "fbnode%d", -s);
            else sprintf(p, "fb%d", -s);
            nl++;
        }
    }
    qsort(lits, (size_t)nl, sizeof lits[0], lit_cmp);
    t->kind = T_LITS;
    t->nlits = nl;
    t->lits = xmalloc((size_t)(nl ? nl : 1) * sizeof lits[0]);
    memcpy(t->lits, lits, (size_t)nl * sizeof lits[0]);
}

static const cy_field *lab_field(const cy_model *m, int lab, int item)
{
    int i;
    for (i = 0; i < m->nfields; i++)
        if (m->fields[i].lab == lab && m->fields[i].item == item) return &m->fields[i];
    return NULL;
}

static int int_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

void model_decode(const cy_model *m, const cy_jedec *j, cy_decoded *d)
{
    int lab, i, k;
    d->m = m;
    if (m->check_qf && j->nfuses != m->qf) refuse("FUSE_COUNT %ld (%s: %d)", j->nfuses, m->name, m->qf);
    d->labs = xmalloc((size_t)(m->nlabs + 1) * sizeof *d->labs);
    for (lab = 1; lab <= m->nlabs; lab++) {
        cy_lab *L = &d->labs[lab];
        const unsigned char *pimf, *ptm, *st;
        int npim, nptm, nst, nb = 0;
        short src[40];
        cy_term slots[80];
        npim = field_bits(m, j, lab_field(m, lab, IT_PIM), &pimf);
        /* interconnect: the source of each block input */
        for (i = 0; i < m->nin; i++) {
            char code[8];
            int n = npim - m->codew * i, slot = -1;
            if (n < 0) n = 0;
            if (n > m->codew) n = m->codew;
            for (k = 0; k < n; k++) code[k] = (char)('0' + pimf[m->codew * i + k]);
            code[n] = 0;
            src[i] = 0;
            if (strcmp(code, m->unused) == 0) continue;
            for (k = 0; k < m->nslots; k++)
                if (strcmp(code, m->mux[k]) == 0) { slot = k; break; }
            if (slot < 0) refuse("PIM_CODE_NOT_MODELLED %s lab %d input %d", code, lab, i + 1);
            src[i] = m->pim[((lab - 1) * m->nin + i) * m->nslots + slot];
            if (src[i] == CY_NC) refuse("PIM_SLOT_NC code %s lab %d input %d", code, lab, i + 1);
        }
        /* the 80 product-term slots */
        nptm = field_bits(m, j, lab_field(m, lab, IT_PTM), &ptm);
        for (k = 0; k < 80; k++) {
            int n = nptm - m->w * k;
            if (n < 0) n = 0;
            if (n > m->w) n = m->w;
            row_term(m, ptm + (n ? m->w * k : 0), n, src, &slots[k]);
        }
        /* steering: the slots of each macrocell, in slot order; constant-FALSE slots add nothing */
        nst = field_bits(m, j, lab_field(m, lab, IT_STEER), &st);
        for (i = 0; i < 16; i++) {
            int used[24], nu = 0, n = nst - 12 * i;
            cy_mc *mc = &L->mcs[i];
            if (n < 0) n = 0;
            if (n > 12) n = 12;
            for (k = 0; k < n; k++)
                if (st[12 * i + k] == 1) {
                    used[nu++] = cy_steer[i][k][0];
                    if (cy_steer[i][k][1] >= 0) used[nu++] = cy_steer[i][k][1];
                }
            qsort(used, (size_t)nu, sizeof used[0], int_cmp);
            mc->pin = m->mcpin[(lab - 1) * 16 + i];
            mc->buried = m->buried_min && mc->pin >= m->buried_min;
            mc->terms = xmalloc(24 * sizeof *mc->terms);
            mc->nterms = 0;
            for (k = 0; k < nu; k++)
                if (slots[used[k]].kind != T_FALSE) mc->terms[mc->nterms++] = slots[used[k]];
        }
        /* bank terms, in field order */
        for (i = 0; i < m->nfields; i++) nb += m->fields[i].lab == lab && m->fields[i].item == IT_PT;
        L->banks = xmalloc((size_t)(nb ? nb : 1) * sizeof *L->banks);
        L->nbanks = 0;
        for (i = 0; i < m->nfields; i++) {
            const cy_field *f = &m->fields[i];
            const unsigned char *b;
            int n;
            if (f->lab != lab || f->item != IT_PT) continue;
            n = field_bits(m, j, f, &b);
            L->banks[L->nbanks].args = f->args;
            row_term(m, b, n, src, &L->banks[L->nbanks].term);
            L->nbanks++;
        }
    }
    /* configuration fields */
    d->cfg = xmalloc((size_t)(2 * m->nfields + 1) * sizeof *d->cfg);
    d->ncfg = 0;
    for (i = 0; i < m->nfields; i++) {
        const cy_field *f = &m->fields[i];
        const unsigned char *b;
        const char *o;
        int n, v = 0, len;
        if (!f->cfg) continue;
        n = field_bits(m, j, f, &b);
        if (n == 0) refuse("FUSE_COUNT %ld (%s: %d)", j->nfuses, m->name, m->qf);
        for (k = 0; k < n; k++) v = v * 2 + b[k];
        o = f->options;
        while (v > 0) { o = strchr(o, '|') + 1; v--; }
        len = (int)strcspn(o, "|");
        d->cfg[d->ncfg].item = f->item;
        d->cfg[d->ncfg].args = f->args;
        d->cfg[d->ncfg].value = xsprintf("%.*s", len, o);
        d->ncfg++;
    }
    /* A buried macrocell's input-select field must select its own node; the clock selected by a CLK2 field is also
       entered under CLK. */
    {
        int n0 = d->ncfg;
        for (i = 0; i < n0; i++) {
            if (d->cfg[i].item == IT_CLK2 && m->check_qf) {
                d->cfg[d->ncfg].item = IT_CLK;
                d->cfg[d->ncfg].args = d->cfg[i].args;
                d->cfg[d->ncfg].value = d->cfg[i].value;
                d->ncfg++;
            }
            if (d->cfg[i].item == IT_IP && m->check_qf && strcmp(d->cfg[i].value, d->cfg[i].args) != 0)
                refuse("IP_PIN_INPUT_NOT_MODELLED buried node %s takes option %s (pin input of a buried node: not supported)",
                       d->cfg[i].args, d->cfg[i].value);
        }
    }
}
