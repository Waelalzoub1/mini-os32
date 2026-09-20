/* as -- a small x86-32 assembler for mini-os32.
 *
 * AT&T syntax, matching the .S files the host toolchain builds:
 *     mov  $4, %eax        source first, destination second
 *     mov  8(%ebp), %ebx   disp(base,index,scale) memory operands
 *     $x   immediate       %x register        x  symbol/absolute address
 *
 * It produces a directly runnable executable, not an object file -- there is
 * no linker in the OS, so `as` emits the same flat ELF image that cc does:
 * one PT_LOAD segment whose vaddr equals its file offset, entry at _start.
 *
 * Two passes.  Pass 1 sizes every instruction and records label offsets;
 * pass 2 emits bytes with symbols resolved.  Branches are always rel32 and
 * symbolic displacements always disp32, so an instruction's size never
 * depends on a value that pass 1 has not seen yet -- the two passes cannot
 * disagree, which is what makes a plain two-pass scheme sufficient here.
 */

#include "libc.h"
#include <stdint.h>

#define MAX_SRC     65536
#define MAX_TEXT    32768
#define MAX_DATA    16384
#define MAX_SYM     512
#define MAX_LINE    256
#define MAX_OPS     3

#define SEC_TEXT 0
#define SEC_DATA 1
#define SEC_BSS  2

#define ELF_HDR_SIZE 84          /* 52-byte ehdr + 32-byte phdr, as in cc */

/* ---- diagnostics ----------------------------------------------------- */

static int  line_no;
static int  err_count;
static char cur_src[PATH_MAX_LEN];

static void err(const char *msg, const char *detail) {
    puts(cur_src);
    puts(":");
    printf("%d", line_no);
    puts(": ");
    puts(msg);
    if (detail && *detail) { puts(": "); puts(detail); }
    putc('\n');
    err_count++;
}

/* ---- symbols --------------------------------------------------------- */

typedef struct {
    char name[32];
    int  section;
    int  offset;     /* within its section, filled in pass 1 */
    int  addr;       /* absolute, computed between passes */
    int  is_abs;     /* .equ/.set value rather than a location */
    int  defined;
} Sym;

static Sym syms[MAX_SYM];
static int sym_count;

static Sym *sym_find(const char *name) {
    for (int i = 0; i < sym_count; i++)
        if (!strcmp(syms[i].name, name)) return &syms[i];
    return 0;
}

static Sym *sym_intern(const char *name) {
    Sym *s = sym_find(name);
    if (s) return s;
    if (sym_count >= MAX_SYM) { err("too many symbols", name); return 0; }
    s = &syms[sym_count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 31);
    s->name[31] = 0;
    return s;
}

/* ---- output buffers -------------------------------------------------- */

static uint8_t text_buf[MAX_TEXT];
static uint8_t data_buf[MAX_DATA];
static int text_len, data_len, bss_len;
static int cur_sec;
static int pass;
static int base_text, base_data, base_bss;

static void emit8(int b) {
    if (cur_sec == SEC_TEXT) {
        if (pass == 2) {
            if (text_len >= MAX_TEXT) { err("text overflow", 0); return; }
            text_buf[text_len] = (uint8_t)b;
        }
        text_len++;
    } else if (cur_sec == SEC_DATA) {
        if (pass == 2) {
            if (data_len >= MAX_DATA) { err("data overflow", 0); return; }
            data_buf[data_len] = (uint8_t)b;
        }
        data_len++;
    } else {
        err("cannot emit data in .bss (use .space)", 0);
    }
}

static void emit16(int v) { emit8(v & 0xFF); emit8((v >> 8) & 0xFF); }
static void emit32(int v) { emit16(v & 0xFFFF); emit16((v >> 16) & 0xFFFF); }

static int sec_len(int sec) {
    return sec == SEC_TEXT ? text_len : sec == SEC_DATA ? data_len : bss_len;
}

static int cur_addr(void) {
    int base = cur_sec == SEC_TEXT ? base_text : cur_sec == SEC_DATA ? base_data : base_bss;
    return base + sec_len(cur_sec);
}

/* ---- lexing ---------------------------------------------------------- */

static const char *lp;   /* cursor into the current line */

static int is_space_ch(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int is_dig(char c) { return c >= '0' && c <= '9'; }
static int is_sym_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$';
}
static int is_sym_ch(char c) { return is_sym_start(c) || is_dig(c); }

static void skip_ws(void) { while (is_space_ch(*lp)) lp++; }

static int eat(char c) {
    skip_ws();
    if (*lp == c) { lp++; return 1; }
    return 0;
}

static int read_name(char *out, int max) {
    skip_ws();
    if (!is_sym_start(*lp)) return 0;
    int n = 0;
    while (is_sym_ch(*lp) && n < max - 1) out[n++] = *lp++;
    out[n] = 0;
    return n;
}

static int esc_char(char c) {
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case 'a': return 7;
    default:  return c;   /* \\ \" \' and anything else is literal */
    }
}

/* ---- expressions ----------------------------------------------------- */

/* Value plus a note on whether any symbol took part.  Instruction sizes may
 * only depend on `has_sym`, never on `val`, because pass 1 cannot know the
 * value of a symbol defined further down the file. */
typedef struct { int val; int has_sym; } Expr;

static void parse_expr(Expr *e);

static void parse_atom(Expr *e) {
    skip_ws();
    e->val = 0;
    e->has_sym = 0;

    if (*lp == '(') {
        lp++;
        parse_expr(e);
        if (!eat(')')) err("expected )", 0);
        return;
    }
    if (*lp == '-') { lp++; parse_atom(e); e->val = -e->val; return; }
    if (*lp == '+') { lp++; parse_atom(e); return; }

    if (*lp == '\'') {
        lp++;
        int c = *lp++;
        if (c == '\\') c = esc_char(*lp++);
        e->val = c;
        if (*lp == '\'') lp++;
        return;
    }
    if (is_dig(*lp)) {
        int base = 10;
        if (lp[0] == '0' && (lp[1] == 'x' || lp[1] == 'X')) { base = 16; lp += 2; }
        else if (lp[0] == '0' && (lp[1] == 'b' || lp[1] == 'B')) { base = 2; lp += 2; }
        unsigned int v = 0;
        for (;;) {
            char c = *lp;
            int d;
            if (is_dig(c)) d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (d >= base) break;
            v = v * (unsigned)base + (unsigned)d;
            lp++;
        }
        e->val = (int)v;
        return;
    }
    /* "." on its own is the location counter, so `.equ len, . - msg` works. */
    if (*lp == '.' && !is_sym_ch(lp[1])) {
        lp++;
        e->val = cur_addr();
        e->has_sym = 1;   /* value differs between passes -- force a fixed size */
        return;
    }
    if (is_sym_start(*lp)) {
        char name[32];
        read_name(name, sizeof(name));
        e->has_sym = 1;
        if (pass == 2) {
            Sym *s = sym_find(name);
            if (!s || !s->defined) { err("undefined symbol", name); e->val = 0; }
            else e->val = s->addr;
        }
        return;
    }
    err("bad expression", 0);
    /* consume something so the caller cannot spin */
    if (*lp) lp++;
}

static void parse_expr(Expr *e) {
    parse_atom(e);
    for (;;) {
        skip_ws();
        if (*lp == '+' || *lp == '-') {
            int minus = (*lp == '-');
            lp++;
            Expr r;
            parse_atom(&r);
            e->val = minus ? e->val - r.val : e->val + r.val;
            e->has_sym |= r.has_sym;
        } else break;
    }
}

/* ---- registers ------------------------------------------------------- */

static const char *reg32[8] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
static const char *reg8[8]  = { "al","cl","dl","bl","ah","ch","dh","bh" };
static const char *reg16[8] = { "ax","cx","dx","bx","sp","bp","si","di" };

/* Returns register number, or -1.  *size gets 1, 2 or 4. */
static int reg_lookup(const char *name, int *size) {
    for (int i = 0; i < 8; i++) if (!strcmp(name, reg32[i])) { *size = 4; return i; }
    for (int i = 0; i < 8; i++) if (!strcmp(name, reg8[i]))  { *size = 1; return i; }
    for (int i = 0; i < 8; i++) if (!strcmp(name, reg16[i])) { *size = 2; return i; }
    return -1;
}

/* ---- operands -------------------------------------------------------- */

typedef enum { OP_NONE, OP_REG, OP_IMM, OP_MEM } OpKind;

typedef struct {
    OpKind kind;
    int reg, size;              /* OP_REG */
    int base, index, scale;     /* OP_MEM, -1 when absent */
    Expr disp;                  /* OP_MEM displacement / OP_IMM value */
} Operand;

static int parse_reg_operand(Operand *o) {
    char name[32];
    if (!read_name(name, sizeof(name))) { err("expected register", 0); return 0; }
    int size;
    int r = reg_lookup(name, &size);
    if (r < 0) { err("unknown register", name); return 0; }
    o->reg = r;
    o->size = size;
    return 1;
}

static int parse_operand(Operand *o) {
    memset(o, 0, sizeof(*o));
    o->base = o->index = -1;
    o->scale = 1;
    skip_ws();
    if (!*lp) { o->kind = OP_NONE; return 0; }

    if (*lp == '%') {
        lp++;
        o->kind = OP_REG;
        return parse_reg_operand(o);
    }
    if (*lp == '$') {
        lp++;
        o->kind = OP_IMM;
        parse_expr(&o->disp);
        return 1;
    }
    if (*lp == '*') { lp++; return parse_operand(o); }   /* indirect call/jmp marker */

    o->kind = OP_MEM;
    skip_ws();
    if (*lp != '(') parse_expr(&o->disp);     /* leading displacement or symbol */

    skip_ws();
    if (*lp == '(') {
        lp++;
        skip_ws();
        if (*lp == '%') {
            lp++;
            Operand t;
            if (!parse_reg_operand(&t)) return 0;
            if (t.size != 4) { err("base register must be 32-bit", 0); return 0; }
            o->base = t.reg;
        }
        skip_ws();
        if (*lp == ',') {
            lp++;
            skip_ws();
            if (*lp == '%') {
                lp++;
                Operand t;
                if (!parse_reg_operand(&t)) return 0;
                if (t.size != 4) { err("index register must be 32-bit", 0); return 0; }
                if (t.reg == 4) { err("%esp cannot be an index register", 0); return 0; }
                o->index = t.reg;
            }
            skip_ws();
            if (*lp == ',') {
                lp++;
                Expr sc;
                parse_expr(&sc);
                o->scale = sc.val;
            }
        }
        if (!eat(')')) { err("expected )", 0); return 0; }
    }
    return 1;
}

/* ---- encoding -------------------------------------------------------- */

static int scale_bits(int s) {
    switch (s) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    default: err("scale must be 1, 2, 4 or 8", 0); return 0;
    }
}

/* Emit the ModRM byte (plus SIB and displacement) for `o`, with `reg` in the
 * middle field -- either a register number or an opcode extension digit. */
static void emit_modrm(int reg, Operand *o) {
    reg &= 7;

    if (o->kind == OP_REG) { emit8(0xC0 | (reg << 3) | o->reg); return; }
    if (o->kind != OP_MEM) { err("expected memory or register operand", 0); return; }

    /* absolute address: no base, no index */
    if (o->base < 0 && o->index < 0) {
        emit8(0x00 | (reg << 3) | 5);
        emit32(o->disp.val);
        return;
    }
    /* index but no base: SIB with base=101 forces a disp32 */
    if (o->base < 0) {
        emit8(0x00 | (reg << 3) | 4);
        emit8((scale_bits(o->scale) << 6) | (o->index << 3) | 5);
        emit32(o->disp.val);
        return;
    }

    int mod;
    if (o->disp.has_sym) mod = 2;                       /* symbol -> always disp32 */
    else if (o->disp.val == 0 && o->base != 5) mod = 0; /* %ebp always needs a disp */
    else if (o->disp.val >= -128 && o->disp.val <= 127) mod = 1;
    else mod = 2;

    if (o->index >= 0 || o->base == 4) {                /* %esp base requires a SIB */
        emit8((mod << 6) | (reg << 3) | 4);
        int idx = (o->index < 0) ? 4 : o->index;        /* 4 = "no index" */
        emit8((scale_bits(o->scale) << 6) | (idx << 3) | o->base);
    } else {
        emit8((mod << 6) | (reg << 3) | o->base);
    }

    if (mod == 1) emit8(o->disp.val & 0xFF);
    else if (mod == 2) emit32(o->disp.val);
}

/* An imm8 form is only safe when pass 1 could already see the value. */
static int imm_fits8(Operand *o) {
    return !o->disp.has_sym && o->disp.val >= -128 && o->disp.val <= 127;
}

static void emit_rel32(Expr *target) {
    /* rel32 is measured from the end of the instruction, i.e. after these 4 bytes */
    int here = cur_addr() + 4;
    emit32(pass == 2 ? target->val - here : 0);
}

/* ---- instruction table ----------------------------------------------- */

typedef struct { const char *name; int base; int digit; } AluOp;

/* The eight classic ALU ops share one encoding pattern: base+0x01 for
 * "reg -> r/m", base+0x03 for "r/m -> reg", 0x81 /digit for an imm32. */
static const AluOp alu_ops[] = {
    { "add", 0x00, 0 }, { "or",  0x08, 1 }, { "adc", 0x10, 2 }, { "sbb", 0x18, 3 },
    { "and", 0x20, 4 }, { "sub", 0x28, 5 }, { "xor", 0x30, 6 }, { "cmp", 0x38, 7 },
};

typedef struct { const char *name; int cc; } CcOp;

static const CcOp cc_ops[] = {
    { "o", 0x0 }, { "no", 0x1 }, { "b", 0x2 }, { "nae", 0x2 }, { "c", 0x2 },
    { "ae", 0x3 }, { "nb", 0x3 }, { "nc", 0x3 },
    { "e", 0x4 }, { "z", 0x4 }, { "ne", 0x5 }, { "nz", 0x5 },
    { "be", 0x6 }, { "na", 0x6 }, { "a", 0x7 }, { "nbe", 0x7 },
    { "s", 0x8 }, { "ns", 0x9 }, { "p", 0xA }, { "np", 0xB },
    { "l", 0xC }, { "nge", 0xC }, { "ge", 0xD }, { "nl", 0xD },
    { "le", 0xE }, { "ng", 0xE }, { "g", 0xF }, { "nle", 0xF },
};

static int cc_lookup(const char *s) {
    for (int i = 0; i < (int)(sizeof(cc_ops) / sizeof(cc_ops[0])); i++)
        if (!strcmp(s, cc_ops[i].name)) return cc_ops[i].cc;
    return -1;
}

/* Shift ops share 0xC1//0xD3 with a digit selecting the direction. */
static int shift_digit(const char *m) {
    if (!strcmp(m, "shl") || !strcmp(m, "sal")) return 4;
    if (!strcmp(m, "shr")) return 5;
    if (!strcmp(m, "sar")) return 7;
    if (!strcmp(m, "rol")) return 0;
    if (!strcmp(m, "ror")) return 1;
    return -1;
}

/* ---- instruction assembly -------------------------------------------- */

/* size: 1, 2 or 4 bytes of operand width. */
static void asm_insn(const char *m, Operand *ops, int nops, int size) {
    int i;

    /* --- ALU group --- */
    for (i = 0; i < 8; i++) {
        if (strcmp(m, alu_ops[i].name)) continue;
        if (nops != 2) { err("expected two operands", m); return; }
        int base = alu_ops[i].base, digit = alu_ops[i].digit;
        if (ops[0].kind == OP_IMM) {
            /* the accumulator has a one-byte-shorter form with no ModRM */
            int acc = (ops[1].kind == OP_REG && ops[1].reg == 0);
            if (size == 1) {
                if (acc) { emit8(base + 0x04); }
                else { emit8(0x80); emit_modrm(digit, &ops[1]); }
                emit8(ops[0].disp.val & 0xFF);
            } else if (imm_fits8(&ops[0])) {
                emit8(0x83); emit_modrm(digit, &ops[1]); emit8(ops[0].disp.val & 0xFF);
            } else if (acc) {
                emit8(base + 0x05); emit32(ops[0].disp.val);
            } else {
                emit8(0x81); emit_modrm(digit, &ops[1]); emit32(ops[0].disp.val);
            }
        } else if (ops[0].kind == OP_REG) {
            emit8(base + (size == 1 ? 0x00 : 0x01));
            emit_modrm(ops[0].reg, &ops[1]);
        } else if (ops[1].kind == OP_REG) {
            emit8(base + (size == 1 ? 0x02 : 0x03));
            emit_modrm(ops[1].reg, &ops[0]);
        } else {
            err("invalid operand combination", m);
        }
        return;
    }

    /* --- mov --- */
    if (!strcmp(m, "mov")) {
        if (nops != 2) { err("expected two operands", m); return; }
        /* accumulator <-> absolute address: shorter form, no ModRM */
        int m0_abs = (ops[0].kind == OP_MEM && ops[0].base < 0 && ops[0].index < 0);
        int m1_abs = (ops[1].kind == OP_MEM && ops[1].base < 0 && ops[1].index < 0);
        if (m0_abs && ops[1].kind == OP_REG && ops[1].reg == 0) {
            emit8(size == 1 ? 0xA0 : 0xA1); emit32(ops[0].disp.val); return;
        }
        if (ops[0].kind == OP_REG && ops[0].reg == 0 && m1_abs) {
            emit8(size == 1 ? 0xA2 : 0xA3); emit32(ops[1].disp.val); return;
        }
        if (ops[0].kind == OP_IMM) {
            if (ops[1].kind == OP_REG) {
                if (size == 1) { emit8(0xB0 + ops[1].reg); emit8(ops[0].disp.val & 0xFF); }
                else { emit8(0xB8 + ops[1].reg); emit32(ops[0].disp.val); }
            } else {
                emit8(size == 1 ? 0xC6 : 0xC7);
                emit_modrm(0, &ops[1]);
                if (size == 1) emit8(ops[0].disp.val & 0xFF); else emit32(ops[0].disp.val);
            }
        } else if (ops[0].kind == OP_REG) {
            emit8(size == 1 ? 0x88 : 0x89);
            emit_modrm(ops[0].reg, &ops[1]);
        } else if (ops[1].kind == OP_REG) {
            emit8(size == 1 ? 0x8A : 0x8B);
            emit_modrm(ops[1].reg, &ops[0]);
        } else {
            err("mov needs a register operand", 0);
        }
        return;
    }

    /* --- zero/sign extending loads --- */
    if (!strcmp(m, "movzbl") || !strcmp(m, "movsbl") ||
        !strcmp(m, "movzwl") || !strcmp(m, "movswl")) {
        if (nops != 2 || ops[1].kind != OP_REG) { err("expected src, %reg32", m); return; }
        int wide = (m[4] == 'w');
        emit8(0x0F);
        emit8((m[3] == 'z' ? 0xB6 : 0xBE) + (wide ? 1 : 0));
        emit_modrm(ops[1].reg, &ops[0]);
        return;
    }

    /* --- lea --- */
    if (!strcmp(m, "lea")) {
        if (nops != 2 || ops[0].kind != OP_MEM || ops[1].kind != OP_REG) {
            err("expected lea mem, %reg", 0); return;
        }
        emit8(0x8D);
        emit_modrm(ops[1].reg, &ops[0]);
        return;
    }

    /* --- test --- */
    if (!strcmp(m, "test")) {
        if (nops != 2) { err("expected two operands", m); return; }
        if (ops[0].kind == OP_IMM) {
            if (ops[1].kind == OP_REG && ops[1].reg == 0) {   /* accumulator form */
                emit8(size == 1 ? 0xA8 : 0xA9);
            } else {
                emit8(size == 1 ? 0xF6 : 0xF7);
                emit_modrm(0, &ops[1]);
            }
            if (size == 1) emit8(ops[0].disp.val & 0xFF); else emit32(ops[0].disp.val);
        } else if (ops[0].kind == OP_REG) {
            emit8(size == 1 ? 0x84 : 0x85);
            emit_modrm(ops[0].reg, &ops[1]);
        } else if (ops[1].kind == OP_REG) {
            emit8(size == 1 ? 0x84 : 0x85);
            emit_modrm(ops[1].reg, &ops[0]);
        } else err("invalid operands", m);
        return;
    }

    /* --- xchg (register with r/m) --- */
    if (!strcmp(m, "xchg")) {
        if (nops != 2) { err("expected two operands", m); return; }
        /* xchg with %eax is a single byte */
        if (size == 4 && ops[0].kind == OP_REG && ops[1].kind == OP_REG &&
            (ops[0].reg == 0 || ops[1].reg == 0)) {
            emit8(0x90 + (ops[0].reg == 0 ? ops[1].reg : ops[0].reg));
            return;
        }
        if (ops[0].kind == OP_REG)      { emit8(0x87); emit_modrm(ops[0].reg, &ops[1]); }
        else if (ops[1].kind == OP_REG) { emit8(0x87); emit_modrm(ops[1].reg, &ops[0]); }
        else err("xchg needs a register", 0);
        return;
    }

    /* --- push / pop --- */
    if (!strcmp(m, "push")) {
        if (nops != 1) { err("expected one operand", m); return; }
        if (ops[0].kind == OP_REG) emit8(0x50 + ops[0].reg);
        else if (ops[0].kind == OP_IMM) {
            if (imm_fits8(&ops[0])) { emit8(0x6A); emit8(ops[0].disp.val & 0xFF); }
            else { emit8(0x68); emit32(ops[0].disp.val); }
        } else { emit8(0xFF); emit_modrm(6, &ops[0]); }
        return;
    }
    if (!strcmp(m, "pop")) {
        if (nops != 1) { err("expected one operand", m); return; }
        if (ops[0].kind == OP_REG) emit8(0x58 + ops[0].reg);
        else { emit8(0x8F); emit_modrm(0, &ops[0]); }
        return;
    }

    /* --- inc / dec --- */
    if (!strcmp(m, "inc") || !strcmp(m, "dec")) {
        int is_dec = (m[0] == 'd');
        if (nops != 1) { err("expected one operand", m); return; }
        if (ops[0].kind == OP_REG && size == 4) { emit8((is_dec ? 0x48 : 0x40) + ops[0].reg); return; }
        emit8(size == 1 ? 0xFE : 0xFF);
        emit_modrm(is_dec ? 1 : 0, &ops[0]);
        return;
    }

    /* --- unary F7 group --- */
    {
        int digit = -1;
        if (!strcmp(m, "not")) digit = 2;
        else if (!strcmp(m, "neg")) digit = 3;
        else if (!strcmp(m, "mul")) digit = 4;
        else if (!strcmp(m, "div")) digit = 6;
        else if (!strcmp(m, "idiv")) digit = 7;
        if (digit >= 0) {
            if (nops != 1) { err("expected one operand", m); return; }
            emit8(size == 1 ? 0xF6 : 0xF7);
            emit_modrm(digit, &ops[0]);
            return;
        }
    }

    /* --- imul: one-operand (F7 /5) or two-operand (0F AF) --- */
    if (!strcmp(m, "imul")) {
        if (nops == 1) { emit8(0xF7); emit_modrm(5, &ops[0]); return; }
        if (nops == 2 && ops[1].kind == OP_REG) {
            emit8(0x0F); emit8(0xAF); emit_modrm(ops[1].reg, &ops[0]);
            return;
        }
        if (nops == 3 && ops[0].kind == OP_IMM && ops[2].kind == OP_REG) {
            if (imm_fits8(&ops[0])) {
                emit8(0x6B); emit_modrm(ops[2].reg, &ops[1]); emit8(ops[0].disp.val & 0xFF);
            } else {
                emit8(0x69); emit_modrm(ops[2].reg, &ops[1]); emit32(ops[0].disp.val);
            }
            return;
        }
        err("bad imul operands", 0);
        return;
    }

    /* --- shifts --- */
    {
        int digit = shift_digit(m);
        if (digit >= 0) {
            if (nops == 1) {                       /* implicit shift by 1 */
                emit8(size == 1 ? 0xD0 : 0xD1);
                emit_modrm(digit, &ops[0]);
            } else if (nops == 2 && ops[0].kind == OP_IMM) {
                if (!ops[0].disp.has_sym && ops[0].disp.val == 1) {
                    emit8(size == 1 ? 0xD0 : 0xD1);   /* shift-by-1 needs no imm */
                    emit_modrm(digit, &ops[1]);
                } else {
                    emit8(size == 1 ? 0xC0 : 0xC1);
                    emit_modrm(digit, &ops[1]);
                    emit8(ops[0].disp.val & 0xFF);
                }
            } else if (nops == 2 && ops[0].kind == OP_REG && ops[0].reg == 1 && ops[0].size == 1) {
                emit8(size == 1 ? 0xD2 : 0xD3);    /* shift by %cl */
                emit_modrm(digit, &ops[1]);
            } else {
                err("shift count must be $imm or %cl", 0);
            }
            return;
        }
    }

    /* --- control flow --- */
    if (!strcmp(m, "jmp")) {
        if (nops != 1) { err("expected one operand", m); return; }
        if (ops[0].kind == OP_MEM && ops[0].base < 0 && ops[0].index < 0) {
            emit8(0xE9); emit_rel32(&ops[0].disp);
        } else { emit8(0xFF); emit_modrm(4, &ops[0]); }
        return;
    }
    if (!strcmp(m, "call")) {
        if (nops != 1) { err("expected one operand", m); return; }
        if (ops[0].kind == OP_MEM && ops[0].base < 0 && ops[0].index < 0) {
            emit8(0xE8); emit_rel32(&ops[0].disp);
        } else { emit8(0xFF); emit_modrm(2, &ops[0]); }
        return;
    }
    if (m[0] == 'j') {
        int cc = cc_lookup(m + 1);
        if (cc >= 0) {
            if (nops != 1) { err("expected one operand", m); return; }
            emit8(0x0F); emit8(0x80 + cc);
            emit_rel32(&ops[0].disp);
            return;
        }
    }
    if (!strncmp(m, "set", 3)) {
        int cc = cc_lookup(m + 3);
        if (cc >= 0) {
            if (nops != 1) { err("expected one operand", m); return; }
            emit8(0x0F); emit8(0x90 + cc);
            emit_modrm(0, &ops[0]);
            return;
        }
    }
    if (!strncmp(m, "cmov", 4)) {
        int cc = cc_lookup(m + 4);
        if (cc >= 0) {
            if (nops != 2 || ops[1].kind != OP_REG) { err("expected src, %reg", m); return; }
            emit8(0x0F); emit8(0x40 + cc);
            emit_modrm(ops[1].reg, &ops[0]);
            return;
        }
    }

    /* --- no-operand forms --- */
    if (!strcmp(m, "ret"))   { emit8(0xC3); return; }
    if (!strcmp(m, "leave")) { emit8(0xC9); return; }
    if (!strcmp(m, "nop"))   { emit8(0x90); return; }
    if (!strcmp(m, "hlt"))   { emit8(0xF4); return; }
    if (!strcmp(m, "cdq") || !strcmp(m, "cltd")) { emit8(0x99); return; }
    if (!strcmp(m, "cwde") || !strcmp(m, "cwtl")) { emit8(0x98); return; }
    if (!strcmp(m, "pushal") || !strcmp(m, "pusha")) { emit8(0x60); return; }
    if (!strcmp(m, "popal") || !strcmp(m, "popa"))   { emit8(0x61); return; }
    if (!strcmp(m, "pushfl") || !strcmp(m, "pushf")) { emit8(0x9C); return; }
    if (!strcmp(m, "popfl") || !strcmp(m, "popf"))   { emit8(0x9D); return; }
    if (!strcmp(m, "int")) {
        if (nops != 1 || ops[0].kind != OP_IMM) { err("expected int $n", 0); return; }
        emit8(0xCD); emit8(ops[0].disp.val & 0xFF);
        return;
    }

    err("unknown instruction", m);
}

/* ---- directives ------------------------------------------------------ */

static void emit_string(int terminate) {
    for (;;) {
        skip_ws();
        if (*lp != '"') { err("expected string", 0); return; }
        lp++;
        while (*lp && *lp != '"') {
            int c = *lp++;
            if (c == '\\' && *lp) c = esc_char(*lp++);
            emit8(c);
        }
        if (*lp == '"') lp++;
        if (terminate) emit8(0);
        skip_ws();
        if (*lp == ',') { lp++; continue; }
        break;
    }
}

/* Returns 1 if the token was a directive. */
static int do_directive(const char *d) {
    if (!strcmp(d, ".text")) { cur_sec = SEC_TEXT; return 1; }
    if (!strcmp(d, ".data")) { cur_sec = SEC_DATA; return 1; }
    if (!strcmp(d, ".bss"))  { cur_sec = SEC_BSS;  return 1; }

    /* Accepted and ignored: everything ends up in one flat image anyway. */
    if (!strcmp(d, ".globl") || !strcmp(d, ".global") || !strcmp(d, ".extern") ||
        !strcmp(d, ".type")  || !strcmp(d, ".size")   || !strcmp(d, ".section") ||
        !strcmp(d, ".code32") || !strcmp(d, ".file")  || !strcmp(d, ".ident")) {
        return 1;
    }

    if (!strcmp(d, ".byte") || !strcmp(d, ".word") || !strcmp(d, ".short") ||
        !strcmp(d, ".long") || !strcmp(d, ".int")) {
        int w = (!strcmp(d, ".byte")) ? 1 : (!strcmp(d, ".long") || !strcmp(d, ".int")) ? 4 : 2;
        for (;;) {
            Expr e;
            parse_expr(&e);
            if (w == 1) emit8(e.val & 0xFF);
            else if (w == 2) emit16(e.val & 0xFFFF);
            else emit32(e.val);
            skip_ws();
            if (*lp == ',') { lp++; continue; }
            break;
        }
        return 1;
    }
    if (!strcmp(d, ".ascii"))  { emit_string(0); return 1; }
    if (!strcmp(d, ".asciz") || !strcmp(d, ".string")) { emit_string(1); return 1; }

    if (!strcmp(d, ".space") || !strcmp(d, ".skip")) {
        Expr n;
        parse_expr(&n);
        if (n.has_sym) { err(".space needs a constant size", 0); return 1; }
        int fill = 0;
        skip_ws();
        if (*lp == ',') { lp++; Expr f; parse_expr(&f); fill = f.val; }
        if (cur_sec == SEC_BSS) bss_len += n.val;
        else for (int i = 0; i < n.val; i++) emit8(fill);
        return 1;
    }
    if (!strcmp(d, ".align") || !strcmp(d, ".balign") || !strcmp(d, ".p2align")) {
        Expr a;
        parse_expr(&a);
        int align = a.val;
        if (!strcmp(d, ".p2align")) { align = 1; for (int i = 0; i < a.val; i++) align *= 2; }
        if (align <= 1) return 1;
        while (sec_len(cur_sec) % align) {
            if (cur_sec == SEC_BSS) bss_len++;
            else emit8(cur_sec == SEC_TEXT ? 0x90 : 0x00);
        }
        return 1;
    }
    if (!strcmp(d, ".equ") || !strcmp(d, ".set")) {
        char name[32];
        if (!read_name(name, sizeof(name))) { err("expected symbol name", 0); return 1; }
        if (!eat(',')) { err("expected , after symbol", 0); return 1; }
        Expr v;
        parse_expr(&v);
        /* Evaluated in both passes: in pass 1 any symbol it refers to is still
         * unknown, so the real value only settles in pass 2.  Safe because
         * every symbolic reference is emitted at a fixed width. */
        Sym *s = sym_intern(name);
        if (s) { s->is_abs = 1; s->defined = 1; s->addr = v.val; }
        return 1;
    }

    err("unknown directive", d);
    return 1;
}

/* ---- one line -------------------------------------------------------- */

static void asm_line(char *line) {
    lp = line;

    for (;;) {
        skip_ws();
        if (!*lp || *lp == '#' || (lp[0] == '/' && lp[1] == '/')) return;

        /* label? */
        const char *save = lp;
        char name[32];
        if (is_sym_start(*lp) && read_name(name, sizeof(name))) {
            skip_ws();
            if (*lp == ':') {
                lp++;
                if (pass == 1) {
                    Sym *s = sym_intern(name);
                    if (s) {
                        if (s->defined) err("duplicate label", name);
                        s->defined = 1;
                        s->section = cur_sec;
                        s->offset = sec_len(cur_sec);
                    }
                }
                continue;   /* a label may be followed by an instruction */
            }
            lp = save;
        }
        break;
    }

    skip_ws();
    if (!*lp || *lp == '#') return;

    char mnem[32];
    if (!read_name(mnem, sizeof(mnem))) { err("expected instruction", 0); return; }

    if (mnem[0] == '.') { do_directive(mnem); return; }

    /* Size suffix: only strip one if the stem is itself a real mnemonic, so
     * "call", "shl" and "mul" are not mistaken for suffixed forms. */
    int size = 0;
    char stem[32];
    strcpy(stem, mnem);
    int ml = strlen(mnem);
    if (ml > 2) {
        char last = mnem[ml - 1];
        int cand = (last == 'l') ? 4 : (last == 'w') ? 2 : (last == 'b') ? 1 : 0;
        /* movzbl/movsbl and friends keep their suffix -- handled explicitly */
        if (cand && strncmp(mnem, "movz", 4) && strncmp(mnem, "movs", 4)) {
            char t[32];
            strncpy(t, mnem, ml - 1);
            t[ml - 1] = 0;
            /* accept the stem only if it looks like a known mnemonic */
            static const char *stems[] = {
                "mov","add","or","adc","sbb","and","sub","xor","cmp","test","lea",
                "push","pop","inc","dec","not","neg","mul","imul","div","idiv",
                "shl","sal","shr","sar","rol","ror","xchg","cmp",0
            };
            for (int i = 0; stems[i]; i++) {
                if (!strcmp(t, stems[i])) { strcpy(stem, t); size = cand; break; }
            }
        }
    }

    Operand ops[MAX_OPS];
    int nops = 0;
    skip_ws();
    while (*lp && *lp != '#' && !(lp[0] == '/' && lp[1] == '/')) {
        if (nops >= MAX_OPS) { err("too many operands", stem); return; }
        if (!parse_operand(&ops[nops])) return;
        nops++;
        skip_ws();
        if (*lp == ',') { lp++; continue; }
        break;
    }

    /* Infer width from a register operand when no suffix was given.  Scan from
     * the last operand back: in AT&T order that is the destination, which is
     * what sets the width.  Scanning forwards would read `shl %cl, %eax` as an
     * 8-bit shift because the *count* is %cl. */
    if (!size) {
        size = 4;
        for (int i = nops - 1; i >= 0; i--)
            if (ops[i].kind == OP_REG) { size = ops[i].size; break; }
    }

    asm_insn(stem, ops, nops, size);
}

/* ---- driver ---------------------------------------------------------- */

static char src[MAX_SRC];

static void run_pass(int which) {
    pass = which;
    cur_sec = SEC_TEXT;
    text_len = data_len = bss_len = 0;
    line_no = 1;

    char line[MAX_LINE];
    const char *p = src;
    while (*p) {
        int n = 0;
        while (*p && *p != '\n' && n < MAX_LINE - 1) line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n') p++;
        asm_line(line);
        line_no++;
        if (err_count > 20) { puts("too many errors\n"); return; }
    }
}

static int build_elf(uint8_t *out, int out_max) {
    int file_sz = ELF_HDR_SIZE + text_len + data_len;
    if (file_sz > out_max) return -1;

    int entry = base_text;
    Sym *s = sym_find("_start");
    if (!s || !s->defined) s = sym_find("main");
    if (s && s->defined) entry = s->addr;

    uint8_t *p = out;
    memset(p, 0, ELF_HDR_SIZE);
    p[0] = 0x7F; p[1] = 'E'; p[2] = 'L'; p[3] = 'F';
    p[4] = 1; p[5] = 1; p[6] = 1;
    *(uint16_t*)(p + 16) = 2;            /* ET_EXEC */
    *(uint16_t*)(p + 18) = 3;            /* EM_386 */
    *(uint32_t*)(p + 20) = 1;            /* version */
    *(uint32_t*)(p + 24) = entry;
    *(uint32_t*)(p + 28) = 52;           /* phoff */
    *(uint16_t*)(p + 40) = 52;           /* ehsize */
    *(uint16_t*)(p + 42) = 32;           /* phentsize */
    *(uint16_t*)(p + 44) = 1;            /* phnum */

    p += 52;
    *(uint32_t*)(p + 0)  = 1;            /* PT_LOAD */
    *(uint32_t*)(p + 4)  = 0;            /* offset */
    *(uint32_t*)(p + 8)  = 0;            /* vaddr == file offset */
    *(uint32_t*)(p + 12) = 0;
    *(uint32_t*)(p + 16) = file_sz;
    *(uint32_t*)(p + 20) = file_sz + bss_len;
    *(uint32_t*)(p + 24) = 7;            /* RWX */
    *(uint32_t*)(p + 28) = 0x1000;

    memcpy(out + ELF_HDR_SIZE, text_buf, text_len);
    memcpy(out + ELF_HDR_SIZE + text_len, data_buf, data_len);
    return file_sz;
}

static uint8_t image[MAX_TEXT + MAX_DATA + ELF_HDR_SIZE];

int main(void) {
    char srcname[PATH_MAX_LEN], outname[PATH_MAX_LEN];

    puts("as v1 (x86-32, AT&T)\n");
    if (cwd_get()[0]) { puts("dir: /"); puts(cwd_get()); putc('\n'); }
    puts("source file: ");
    readline(srcname, sizeof(srcname));
    puts("output file: ");
    readline(outname, sizeof(outname));

    char srcpath[PATH_MAX_LEN], outpath[PATH_MAX_LEN];
    if (!path_fs(cwd_get(), srcname, srcpath, sizeof(srcpath))) {
        puts("bad source path\n"); return 1;
    }
    if (!path_fs(cwd_get(), outname, outpath, sizeof(outpath))) {
        puts("bad output path\n"); return 1;
    }
    strcpy(cur_src, srcpath);

    int n = sys_load(srcpath, src, sizeof(src) - 1);
    if (n <= 0) { puts("read fail: "); puts(srcpath); putc('\n'); return 1; }
    src[n] = 0;

    /* Pass 1 sizes everything; section bases follow from those sizes. */
    run_pass(1);
    if (err_count) { puts("errors\n"); return 1; }

    base_text = ELF_HDR_SIZE;
    base_data = base_text + text_len;
    base_bss  = base_data + data_len;

    for (int i = 0; i < sym_count; i++) {
        Sym *s = &syms[i];
        if (s->is_abs || !s->defined) continue;
        int base = s->section == SEC_TEXT ? base_text : s->section == SEC_DATA ? base_data : base_bss;
        s->addr = base + s->offset;
    }

    /* Run the emit pass twice: a `.equ` whose expression referred to a symbol
     * defined further down only settles once that symbol has been seen.  Sizes
     * are already fixed, so the second run lays out identically -- it just has
     * the right values.  Diagnostics come from the final run only. */
    run_pass(2);
    err_count = 0;
    run_pass(2);
    if (err_count) { puts("errors\n"); return 1; }

    int size = build_elf(image, sizeof(image));
    if (size < 0) { puts("image too large\n"); return 1; }
    if (sys_save(outpath, image, size) != 0) { puts("save fail\n"); return 1; }

    printf("ok: %d text, %d data, %d bss, %d bytes\n", text_len, data_len, bss_len, size);
    return 0;
}
