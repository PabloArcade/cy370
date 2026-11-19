/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cy370dump: shared declarations.
   Cypress FLASH370 CPLDs (CY7C371 .. CY7C375 and the "i" parts): JEDEC fuse file -> VHDL design and pin assignment. */
#ifndef CY370_H
#define CY370_H

#include <stddef.h>

#define CY_VERSION "1.00"

/* ---- device tables (tables.c) ---- */

#define CY_NC 32767                 /* interconnect multiplexer slot with no source */

enum {                              /* fuse field kinds, in the order of cy_item_name[] */
    IT_PT, IT_PIM, IT_PTM, IT_STEER, IT_CLK, IT_CLK2, IT_REG2, IT_POL, IT_OE2, IT_CI, IT_CI2, IT_IREG2, IT_CIPOL,
    IT_LABCLKPOL, IT_FPOL, IT_IP
};

typedef struct {
    int start, width;               /* first fuse and number of fuses */
    int lab;                        /* logic block the field is listed under */
    int item;                       /* IT_... */
    int cfg;                        /* 1: a configuration field (value = option selected by the fuses read as a binary number) */
    const char *args;               /* what the field belongs to: a pin or node number, "<block> <clock pin>", a bank */
    const char *options;            /* option names separated by '|' (configuration fields) */
} cy_field;

typedef struct {
    const char *name;               /* base part: c371 .. c375 */
    int qf;                         /* number of fuses */
    int nlabs;                      /* logic blocks, 16 macrocells each */
    int w;                          /* columns of a product term: 2 per logic block input (true, complement) */
    int nin;                        /* logic block inputs fed by the interconnect */
    int codew;                      /* fuses per interconnect multiplexer code */
    int nslots;                     /* multiplexer slots per input */
    const char *const *mux;         /* code of each slot */
    const char *unused;             /* code of an unused input */
    int buried_min;                 /* numbers from here on are buried macrocell nodes, not pins (0: the part has none) */
    int check_qf;                   /* 1: a fuse count other than qf is refused before anything is read */
    const cy_field *fields;
    int nfields;
    const short *pim;               /* [block][input][slot]: > 0 pin, < 0 feedback of the macrocell at pin / node -n, CY_NC */
    const short *mcpin;             /* [block][macrocell]: pin or node number */
} cy_model;

typedef struct {
    const char *code;               /* package letter */
    const char *const *ordercodes;  /* 0-terminated */
    const char *const *map;         /* pairs I/O-cell label, package pin; 0-terminated; NULL: the label is the pin */
    const char *status;
} cy_package;

typedef struct {
    const char *name;               /* c371, c371i, ... */
    const char *fullname;           /* CY7C371, ... */
    int model;                      /* index into cy_models */
    int dedicated[5];               /* dedicated input pins, 0-terminated */
    int vpp;                        /* programming-voltage pin of an "i" part (0: none) */
    int clocks[5];                  /* clock / input pins, 0-terminated */
    int oe0;                        /* output-enable bank numbers of block k: oe0 + 4k .. oe0 + 4k + 3; preset 2k - 1, reset 2k */
    int buried_note;                /* 1: the design header carries the buried-macrocell comment */
    const cy_package *packages;     /* terminated by code == NULL */
} cy_device;

typedef struct { const char *field, *values; } cy_valset;             /* values: "|v1|v2|" */
typedef struct { const char *field, *family, *values; } cy_famset;

extern const signed char cy_steer[16][12][2];
extern const char *const cy_item_name[];
extern const cy_model cy_models[5];
extern const cy_device cy_devices[10];
extern const cy_valset cy_supported[];
extern const cy_famset cy_family_checked[];
extern const cy_famset cy_family_supported[];
extern const char *const cy_kinds_supported[];

/* ---- growing text buffer and helpers (main.c) ---- */

typedef struct { char *p; size_t n, cap; } sb;

void *xmalloc(size_t n);
void sb_add(sb *b, const char *s, size_t n);
void sb_puts(sb *b, const char *s);
void sb_printf(sb *b, const char *fmt, ...);
void sb_latin1(sb *b, const unsigned char *s, size_t n);   /* bytes 0x80..0xff written as two UTF-8 bytes */
char *xstrdup(const char *s);
char *xsprintf(const char *fmt, ...);

/* A refusal: the message "<NAME> <detail>" is kept and control returns to main(), which prints "REFUSED: <message>". */
extern sb cy_refusal;
void refuse(const char *fmt, ...);
void refuse_sb(void);               /* the message is already in cy_refusal */

/* ---- JEDEC reader (jedec.c) ---- */

typedef struct {
    const unsigned char *design;    /* device name: first field after STX, blanks stripped */
    size_t design_len;
    long nfuses;                    /* QF */
    unsigned char *fuses;           /* 0 / 1 */
} cy_jedec;

void jedec_parse(const unsigned char *data, size_t len, cy_jedec *out);
int is_uspace(unsigned c);

/* ---- fuse model (model.c) ---- */

typedef struct {                    /* one product term */
    int kind;                       /* 0 = sum of literals, 1 = constant TRUE, 2 = constant FALSE */
    int nlits;
    char (*lits)[16];               /* "pin12", "/pin12", "fb7", "/fbnode204": sorted */
} cy_term;

enum { T_LITS, T_TRUE, T_FALSE };

typedef struct {
    int pin;                        /* pin or node number */
    int buried;
    int nterms;
    cy_term *terms;                 /* the product terms steered to this macrocell that are not constant FALSE */
} cy_mc;

typedef struct { const char *args; cy_term term; } cy_bank;

typedef struct {
    cy_mc mcs[16];
    int nbanks;
    cy_bank *banks;
} cy_lab;

typedef struct { int item; const char *args; const char *value; } cy_cfg;

typedef struct {
    const cy_model *m;
    cy_lab *labs;                   /* [1 .. nlabs] */
    int ncfg;
    cy_cfg *cfg;                    /* in fuse-field order; the selected clock of CLK2 fields repeated as CLK entries at the end */
} cy_decoded;

void model_decode(const cy_model *m, const cy_jedec *j, cy_decoded *d);
const char *cfg_get(const cy_decoded *d, int item, const char *args);   /* NULL when the part has no such field */
int field_bits(const cy_model *m, const cy_jedec *j, const cy_field *f, const unsigned char **bits);

/* ---- construct kinds of an emitted design (structures.c) ---- */

typedef struct { int n; char **name; int *count; } cy_kinds;

void kinds_of(const char *txt, cy_kinds *k);
int kinds_get(const cy_kinds *k, const char *name);
const char *find_word(const char *text, const char *word);             /* first occurrence delimited by non-word characters */

/* ---- decoder (decode.c) ---- */

typedef struct {
    sb text;                        /* the VHDL design */
    sb pinmap;                      /* "<package pin> <I/O-cell label>" lines */
    sb report;                      /* the lines printed after "decoded ..." */
    int macrocells, ports;
} cy_result;

extern sb cy_used;                  /* "USED <field> <value>" lines collected so far, sorted on request */
void used_lines(sb *out);
void decode(const unsigned char *data, size_t len, const char *top, const char *package, cy_result *r);
const cy_device *device_by_name(const unsigned char *name, size_t n);

#endif
