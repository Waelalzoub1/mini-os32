#include "libc.h"
#include <stdint.h>

/* Base address programs are linked at.  0 in the OS, where user pointers are
 * offsets into a segment.  The host test harness overrides this so the same
 * binary can be mapped and executed flat under Linux. */
#ifndef LINK_BASE
#define LINK_BASE 0
#endif

/* Sized so cc can compile cc.c, which is the largest program this OS has:
 * 203 functions, 77KB of code, and a 7.5MB .bss.  MAX_NODE is the exception --
 * the pool is recycled per top-level declaration (see parse_program), so it
 * only has to hold the biggest single function, not the whole file. */
#define MAX_CODE 393216   /* cc's codegen is ~3x less dense than gcc's */
#define MAX_RODATA 98304
#define MAX_DATA 32768
/* A bound check, not a buffer: costs nothing.  20 MB: cc's own bss is
   ~18.3 MB and the user arena tops out near 23 MB with the stack.
   (Kept above the #define: cc's preprocessor is line-based and cannot
   digest a block comment that opens on a directive line.) */
#define MAX_BSS 20971520
#define MAX_TOK 4096
#define MAX_NODE 4096
#define MAX_SYM 1024
#define MAX_FUNC 512
#define MAX_FIX 8192    /* label/symbol fixups: two arrays, cheap */
/* Per-object relocation tables, so this one is multiplied by MAX_OBJ.
 * gcc emits 2589 relocations for cc.c, so 4096 is comfortable. */
#define MAX_RELOC 12288
/* cc.c needs >2048 labels; overflowing this array clobbered label_count
   itself (the next thing in .bss) and silently reused label indices. */
#define MAX_LABEL 8192
#define MAX_TYPE 4096   /* cc.c itself needs a bit over 2048 */
#define MAX_STRUCT 128
#define MAX_FIELD 64
#define MAX_TYPEDEF 256
#define MAX_CASE 256
#define MAX_PARAM 16   /* cc.c has 9-argument functions */
#define MAX_OBJ 6   /* start + stdio + three sources is the self-host case */
#define MAX_OUT_TEXT (MAX_CODE * MAX_OBJ)
#define MAX_OUT_RODATA (MAX_RODATA * MAX_OBJ)
#define MAX_OUT_DATA (MAX_DATA * MAX_OBJ)

typedef enum { TY_INT, TY_SHORT, TY_FLOAT, TY_CHAR, TY_BOOL, TY_VOID, TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION } TypeKind;

typedef struct Type Type;
typedef struct StructDef StructDef;

typedef struct Field {
    char name[32];
    Type *type;
    int offset;
} Field;

struct StructDef {
    char name[32];
    Field fields[MAX_FIELD];
    int field_count;
    int size;
    int align;
    Type *type;
    int is_union;
};

struct Type {
    TypeKind kind;
    Type *base;
    StructDef *sdef;
    int array_len;
    int size;
    int align;
    int is_unsigned;
};

#define TOK_TEXT_MAX 192
#define LINEBUF_MAX 1536
#define SRCBUF_MAX 262144   /* cc.c is ~167KB and has to fit whole */
#define PPBUF_MAX 393216
#define INC_BUF_MAX 65536   /* headers only; the pool is 9 deep */
#define PP_MAX_DEPTH 9

typedef struct {
    int kind;
    int pos;
    int end;
    char text[TOK_TEXT_MAX];
    int ival;
    float fval;
} Token;

enum {
    TOK_EOF=0,
    TOK_ID,
    TOK_NUM,
    TOK_FNUM,
    TOK_STR,
    TOK_CHAR,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_SEMI, TOK_COMMA,
    TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV,
    TOK_ASSIGN,
    TOK_EQ, TOK_NE, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_RETURN, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_DO, TOK_GOTO,
    TOK_SWITCH, TOK_CASE, TOK_DEFAULT, TOK_BREAK, TOK_CONTINUE,
    TOK_TYPEDEF, TOK_STRUCT, TOK_UNION, TOK_EXTERN, TOK_ENUM,
    TOK_INT, TOK_FLOAT, TOK_KW_CHAR, TOK_BOOL, TOK_VOID, TOK_SIZEOF, TOK_UNSIGNED, TOK_SIGNED,
    TOK_CONST, TOK_VOLATILE, TOK_STATIC, TOK_AUTO, TOK_REGISTER, TOK_SHORT, TOK_LONG,
    TOK_DOT, TOK_LBRACK, TOK_RBRACK, TOK_COLON, TOK_ARROW, TOK_AMP, TOK_ELLIPSIS,
    TOK_OR, TOK_XOR, TOK_TILDE, TOK_MOD, TOK_SHL, TOK_SHR,
    TOK_INC, TOK_DEC, TOK_OPASSIGN,
    TOK_LAND, TOK_LOR, TOK_LOGNOT, TOK_QUESTION
};

typedef struct Node {
    int kind;
    Type *type; /* for casts or resolved types */
    int op;
    int ival;
    float fval;
    char name[TOK_TEXT_MAX];
    struct Node *lhs;
    struct Node *rhs;
    struct Node *callee;
    struct Node *args[MAX_PARAM];
    int argc;
} Node;

typedef struct {
    char name[32];
    char link_name[32];
    Type *type;
    int offset; /* locals: ebp+offset, globals: data offset */
    int is_global;
    int is_extern;
    int is_static;
    int section; /* SEC_DATA/SEC_BSS for globals */
    int alias_global; /* for static locals backed by globals */
} Sym;

typedef struct {
    char name[32];
    char link_name[32];
    Type *ret;
    int label;
    int defined;
    int is_static;
    int param_count;
    Type *params[MAX_PARAM];
    int is_varargs;
    int declared;
} Func;

/* Powers of ten as floats.  Every entry up to 1e10 is exact; past that the
 * value is the nearest float, which is still a better multiplier than
 * repeated scaling because the result is rounded only once. */
static const float pow10_tab[39] = {
    1e0f,  1e1f,  1e2f,  1e3f,  1e4f,  1e5f,  1e6f,  1e7f,  1e8f,  1e9f,
    1e10f, 1e11f, 1e12f, 1e13f, 1e14f, 1e15f, 1e16f, 1e17f, 1e18f, 1e19f,
    1e20f, 1e21f, 1e22f, 1e23f, 1e24f, 1e25f, 1e26f, 1e27f, 1e28f, 1e29f,
    1e30f, 1e31f, 1e32f, 1e33f, 1e34f, 1e35f, 1e36f, 1e37f, 1e38f
};

/* v * 10^e, in one rounding step wherever the exponent fits the table. */
static float scale_pow10(float v, int e) {
    if (e == 0) return v;
    if (e > 0) {
        if (e <= 38) return v * pow10_tab[e];
        /* Past 1e38 a float is infinite anyway; two steps get there. */
        return v * pow10_tab[38] * pow10_tab[e - 38 > 38 ? 38 : e - 38];
    }
    if (e >= -38) return v / pow10_tab[-e];
    return v / pow10_tab[38] / pow10_tab[(-e) - 38 > 38 ? 38 : (-e) - 38];
}

static const char *src;
static int pos;
static Token tok;
static char cur_file[64];
static char incbuf_pool[PP_MAX_DEPTH][INC_BUF_MAX];

/* The parser sees the preprocessed buffer, where #include splices files
 * together and directive lines vanish, so buffer line numbers drift from the
 * file the user edited.  The preprocessor records, for every line it emits,
 * which file and line it came from; diagnostics translate through this map. */
#define MAX_LM_LINES 24576
#define MAX_LM_FILES 24
static int lm_line[MAX_LM_LINES];
static uint8_t lm_file[MAX_LM_LINES];
static char lm_files[MAX_LM_FILES][64];
static int lm_files_len;
static int lm_count;

/* A for-loop's post expression (and loop conditions) are parsed where they
 * appear but code-generated after the body, when the lexer has moved on.
 * Errors raised during that late generation point here instead of at the
 * current token.  Active while diag_ovr_end > 0. */
static int diag_ovr_pos;
static int diag_ovr_end;
static int warn_count;
static int func_has_return;
static int dbg_for = 0;
static int in_func = 0;
static int static_local_id = 0;
static int unit_counter = 0;
static int current_unit_id = 0;
static int static_sym_id = 0;
static char current_func_name[32];
static void next_token(void);

static uint8_t code[MAX_CODE];
static int code_len;
static uint8_t rodata[MAX_RODATA];
static int rodata_len;
static uint8_t data_seg[MAX_DATA];
static int data_len;
static int bss_len;

static Node nodes[MAX_NODE];
static int node_len;

static Type type_pool[MAX_TYPE];
static int type_pool_len;
static StructDef structs[MAX_STRUCT];
static int structs_len;
typedef struct { char name[32]; Type *type; } Typedef;
static Typedef typedefs[MAX_TYPEDEF];
static int typedefs_len;

typedef struct { char name[32]; int val; } EnumConst;
static EnumConst enum_consts[512];
static int enum_const_len;

static Type type_int, type_float, type_char, type_bool, type_void;
static Type type_uint, type_uchar;
static Type type_short, type_ushort;

static Sym globals[MAX_SYM];
static int globals_len;
static Sym locals[MAX_SYM];
static int locals_len;
static int local_offset;

static Func funcs[MAX_FUNC];
static int funcs_len;

enum { SEC_TEXT = 1, SEC_RODATA = 2, SEC_DATA = 3, SEC_BSS = 4 };
enum { RELOC_ABS32 = 1, RELOC_REL32 = 2 };

typedef struct {
    char name[32];
    int section;
    int value;
    int size;
    int defined;
    int is_func;
} ObjSym;

typedef struct {
    int section;
    int offset;
    int type;
    int target_section;
    int addend;
} ObjRelocLoc;

typedef struct {
    int section;
    int offset;
    int type;
    int addend;
    char name[32];
} ObjRelocSym;

typedef struct {
    uint8_t text[MAX_CODE];
    int text_len;
    uint8_t rodata[MAX_RODATA];
    int rodata_len;
    uint8_t data[MAX_DATA];
    int data_len;
    int bss_len;
    ObjSym syms[MAX_SYM];
    int sym_len;
    ObjRelocLoc lreloc[MAX_RELOC];
    int lreloc_len;
    ObjRelocSym sreloc[MAX_RELOC];
    int sreloc_len;
} Obj;

/* large object list: keep out of stack */
static Obj objs[MAX_OBJ + 1];

typedef struct {
    int count;
    Type *types[MAX_PARAM];
    char names[MAX_PARAM][32];
    int is_varargs;
} ParamInfo;

static int labels_pos[MAX_LABEL];
static int labels_def[MAX_LABEL];
static int label_count;

typedef struct { int pos; int label; } Fix;
static Fix fixups[MAX_FIX];
static int fixup_len;

typedef struct { int pos; int kind; int index; } AddrFix;
static AddrFix addr_fix[MAX_FIX];
static int addr_fix_len;

typedef struct { int pos; int type; char name[32]; } SymFix;
static SymFix sym_fix[MAX_FIX];
static int sym_fix_len;

enum { AF_STR = 1, AF_GLOB = 2, AF_STR_DATA = 3 };

typedef struct { int break_label; int continue_label; } BreakCtx;
static BreakCtx break_stack[16];
static int break_depth;

typedef struct {
    int end_label;
    int dispatch_label;
    int default_label;
    int temp_offset;
    int case_count;
    int case_values[MAX_CASE];
    int case_labels[MAX_CASE];
} SwitchCtx;
static SwitchCtx switch_stack[8];
static int switch_depth;

static Field *find_field(StructDef *s, const char *name);

/* Overflowing here used to drop bytes silently, which produced an object
 * whose relocations pointed past its own text -- a corrupt binary rather than
 * an error. */
static void emit8(uint8_t b) {
    if (code_len >= MAX_CODE) { puts("code ovf\n"); sys_exit(1); }
    code[code_len++] = b;
}
static void emit32(uint32_t v) {
    emit8((uint8_t)(v & 0xFF));
    emit8((uint8_t)((v >> 8) & 0xFF));
    emit8((uint8_t)((v >> 16) & 0xFF));
    emit8((uint8_t)((v >> 24) & 0xFF));
}

static void emit_mov_eax_imm(uint32_t v) { emit8(0xB8); emit32(v); }
static void emit_mov_ebx_imm(uint32_t v) { emit8(0xBB); emit32(v); }
static void emit_mov_ecx_imm(uint32_t v) { emit8(0xB9); emit32(v); }
static void emit_mov_edx_imm(uint32_t v) { emit8(0xBA); emit32(v); }

static void emit_push_eax(void) { emit8(0x50); }
static void emit_pop_eax(void) { emit8(0x58); }

static void print_dec(int n) {
    char buf[12];
    int bi = 0;
    if (n == 0) { putc('0'); return; }
    if (n < 0) { putc('-'); n = -n; }
    while (n > 0) { int q = n / 10; int d = n - q * 10; buf[bi++] = (char)('0' + d); n = q; }
    while (bi > 0) { bi--; putc(buf[bi]); }
}

static int append_dec(char *out, int n) {
    char buf[12];
    int bi = 0;
    if (n == 0) { out[0] = '0'; return 1; }
    if (n < 0) { out[0] = '-'; n = -n; out++; }
    while (n > 0) { int q = n / 10; int d = n - q * 10; buf[bi++] = (char)('0' + d); n = q; }
    int len = bi;
    while (bi > 0) { bi--; *out++ = buf[bi]; }
    return len;
}

static void make_static_link_name(char *out) {
    int oi = 0;
    out[oi++] = '_'; out[oi++] = '_'; out[oi++] = 's';
    oi += append_dec(out + oi, current_unit_id);
    out[oi++] = '_';
    oi += append_dec(out + oi, static_sym_id++);
    out[oi] = 0;
}

static void lm_add(const char *fname, int line) {
    if (lm_count >= MAX_LM_LINES) return;
    int fi = 0;
    while (fi < lm_files_len && strcmp(lm_files[fi], fname)) fi++;
    if (fi == lm_files_len) {
        if (lm_files_len >= MAX_LM_FILES) {
            fi = 0;
        } else {
            strncpy(lm_files[lm_files_len], fname, 63);
            lm_files[lm_files_len][63] = 0;
            lm_files_len++;
        }
    }
    lm_file[lm_count] = (uint8_t)fi;
    lm_line[lm_count] = line;
    lm_count++;
}

static void compute_line_col_at(int at, int *out_line, int *out_col) {
    int line = 1;
    int col = 1;
    int i;
    if (at < 0) at = 0;
    for (i = 0; i < at && src[i]; i++) {
        if (src[i] == '\n') { line++; col = 1; }
        else col++;
    }
    *out_line = line;
    *out_col = col;
}

static void diag_near_span(int at, int end) {
    int line = 1;
    int col = 1;
    compute_line_col_at(at, &line, &col);
    const char *fname = cur_file;
    int shown = line;
    if (line <= lm_count) {
        fname = lm_files[lm_file[line - 1]];
        shown = lm_line[line - 1];
    }
    if (fname[0]) { puts(fname); putc(':'); }
    print_dec(shown); putc(':'); print_dec(col); puts(": ");
    /* print line content */
    int start = at;
    while (start > 0 && src[start - 1] != '\n') start--;
    int line_end = at;
    while (src[line_end] && src[line_end] != '\n') line_end++;
    for (int i = start; i < line_end; i++) {
        char c = src[i];
        if ((unsigned char)c < 32 || (unsigned char)c > 126) putc('.');
        else putc(c);
    }
    putc('\n');
    if (col < 1) col = 1;
    for (int i = 1; i < col; i++) putc(' ');
    int span = end - at;
    if (span < 1) span = 1;
    if (at + span > line_end) span = line_end - at;
    if (span < 1) span = 1;
    putc('^');
    for (int i = 1; i < span; i++) putc('~');
    putc('\n');
}

static void diag_near_at(int at) {
    diag_near_span(at, at + 1);
}

static void diag_near(void) {
    if (diag_ovr_end > 0) { diag_near_span(diag_ovr_pos, diag_ovr_end); return; }
    diag_near_span(tok.pos, tok.end);
}

static void print_msg(const char *msg) {
    int i = 0;
    char last = 0;
    while (msg[i]) { last = msg[i]; putc(msg[i]); i++; }
    if (i == 0 || last != '\n') putc('\n');
}

static void error_here(const char *msg) {
    puts("error: ");
    print_msg(msg);
    diag_near();
    sys_exit(1);
}

static void error_name(const char *msg, const char *name) {
    puts("error: ");
    puts(msg);
    if (name && name[0]) {
        puts(": ");
        puts(name);
    }
    putc('\n');
    diag_near();
    sys_exit(1);
}

static void warn_here(const char *msg) {
    puts("warning: ");
    print_msg(msg);
    diag_near();
    warn_count++;
}

static void warn_at(int at, const char *msg) {
    puts("warning: ");
    print_msg(msg);
    diag_near_at(at);
    warn_count++;
}

static const char *tok_name(int k) {
    switch (k) {
    case TOK_EOF: return "end of file";
    case TOK_ID: return "identifier";
    case TOK_NUM: return "number";
    case TOK_FNUM: return "float";
    case TOK_STR: return "string";
    case TOK_CHAR: return "char";
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_LBRACE: return "'{'";
    case TOK_RBRACE: return "'}'";
    case TOK_LBRACK: return "'['";
    case TOK_RBRACK: return "']'";
    case TOK_SEMI: return "';'";
    case TOK_COMMA: return "','";
    case TOK_PLUS: return "'+'";
    case TOK_MINUS: return "'-'";
    case TOK_MUL: return "'*'";
    case TOK_DIV: return "'/'";
    case TOK_MOD: return "'%'";
    case TOK_ASSIGN: return "'='";
    case TOK_EQ: return "'=='";
    case TOK_NE: return "'!='";
    case TOK_LT: return "'<'";
    case TOK_LE: return "'<='";
    case TOK_GT: return "'>'";
    case TOK_GE: return "'>='";
    case TOK_DOT: return "'.'";
    case TOK_ARROW: return "'->'";
    case TOK_AMP: return "'&'";
    case TOK_OR: return "'|'";
    case TOK_XOR: return "'^'";
    case TOK_TILDE: return "'~'";
    case TOK_SHL: return "'<<'";
    case TOK_SHR: return "'>>'";
    case TOK_INC: return "'++'";
    case TOK_DEC: return "'--'";
    case TOK_OPASSIGN: return "compound assignment";
    case TOK_LAND: return "'&&'";
    case TOK_LOR: return "'||'";
    case TOK_LOGNOT: return "'!'";
    case TOK_COLON: return "':'";
    case TOK_QUESTION: return "'?'";
    case TOK_ELLIPSIS: return "'...'";
    case TOK_RETURN: return "return";
    case TOK_IF: return "if";
    case TOK_ELSE: return "else";
    case TOK_WHILE: return "while";
    case TOK_FOR: return "for";
    case TOK_DO: return "do";
    case TOK_GOTO: return "goto";
    case TOK_SWITCH: return "switch";
    case TOK_CASE: return "case";
    case TOK_DEFAULT: return "default";
    case TOK_BREAK: return "break";
    case TOK_CONTINUE: return "continue";
    case TOK_TYPEDEF: return "typedef";
    case TOK_STRUCT: return "struct";
    case TOK_UNION: return "union";
    case TOK_EXTERN: return "extern";
    case TOK_ENUM: return "enum";
    case TOK_INT: return "int";
    case TOK_FLOAT: return "float";
    case TOK_KW_CHAR: return "char";
    case TOK_BOOL: return "bool";
    case TOK_VOID: return "void";
    case TOK_SIZEOF: return "sizeof";
    case TOK_UNSIGNED: return "unsigned";
    case TOK_SIGNED: return "signed";
    case TOK_CONST: return "const";
    case TOK_VOLATILE: return "volatile";
    case TOK_STATIC: return "static";
    case TOK_AUTO: return "auto";
    case TOK_REGISTER: return "register";
    case TOK_SHORT: return "short";
    case TOK_LONG: return "long";
    default: return "token";
    }
}

static void print_tok_desc(void) {
    if (tok.kind == TOK_ID) {
        puts("identifier '"); puts(tok.text); putc('\'');
        return;
    }
    if (tok.kind == TOK_NUM) {
        puts("number ");
        print_dec(tok.ival);
        return;
    }
    if (tok.kind == TOK_CHAR) {
        puts("char ");
        print_dec(tok.ival);
        return;
    }
    puts(tok_name(tok.kind));
}

static void error_expected(int k) {
    puts("error: expected ");
    puts(tok_name(k));
    puts(", got ");
    print_tok_desc();
    putc('\n');
    diag_near();
    sys_exit(1);
}

static void debug_for_header(void) {
    int save_pos = pos;
    Token save_tok = tok;
    puts("for-header: ");
    int count = 0;
    while (tok.kind != TOK_EOF && count < 64) {
        print_tok_desc();
        putc(' ');
        if (tok.kind == TOK_RPAREN) break;
        next_token();
        count++;
    }
    putc('\n');
    pos = save_pos;
    tok = save_tok;
}
static void emit_pop_ebx(void) { emit8(0x5B); }

static void emit_add_esp(int v) { emit8(0x81); emit8(0xC4); emit32((uint32_t)v); }

static void emit_mov_eax_membp(int disp) { emit8(0x8B); emit8(0x85); emit32((uint32_t)disp); }
static void emit_mov_membp_eax(int disp) { emit8(0x89); emit8(0x85); emit32((uint32_t)disp); }

static void emit_mov_eax_memabs(uint32_t addr) { emit8(0xA1); emit32(addr); }
static void emit_mov_memabs_eax(uint32_t addr) { emit8(0xA3); emit32(addr); }

static int new_label(void) {
    if (label_count >= MAX_LABEL) { puts("label ovf\n"); sys_exit(1); }
    return label_count++;
}

static void set_label(int id) { labels_pos[id] = code_len; labels_def[id] = 1; }

static void emit_jmp_label(int id) {
    emit8(0xE9);
    int pos = code_len;
    emit32(0);
    fixups[fixup_len].pos = pos; fixups[fixup_len].label = id; fixup_len++;
}

static void emit_jcc_label(int cc, int id) {
    emit8(0x0F); emit8((uint8_t)cc);
    int pos = code_len;
    emit32(0);
    fixups[fixup_len].pos = pos; fixups[fixup_len].label = id; fixup_len++;
}

static void emit_call_label(int id) {
    emit8(0xE8);
    int pos = code_len;
    emit32(0);
    fixups[fixup_len].pos = pos; fixups[fixup_len].label = id; fixup_len++;
}

static void add_addr_fix(int pos, int kind, int index) {
    addr_fix[addr_fix_len].pos = pos; addr_fix[addr_fix_len].kind = kind;
    addr_fix[addr_fix_len].index = index; addr_fix_len++;
}

static void add_sym_fix(int pos, int type, const char *name) {
    if (sym_fix_len >= MAX_FIX) { puts("reloc ovf\n"); sys_exit(1); }
    SymFix *f = &sym_fix[sym_fix_len++];
    f->pos = pos;
    f->type = type;
    strncpy(f->name, name, 31);
    f->name[31] = 0;
}

static void skip_ws(void) {
    for (;;) {
        char c = src[pos];
        if (c==' '||c=='\t'||c=='\n'||c=='\r') { pos++; continue; }
        if (c=='/' && src[pos+1]=='/') { while (src[pos] && src[pos] != '\n') pos++; continue; }
        if (c=='/' && src[pos+1]=='*') { pos+=2; while (src[pos] && !(src[pos]=='*'&&src[pos+1]=='/')) pos++; if (src[pos]) pos+=2; continue; }
        break;
    }
}

static int is_alpha(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_alnum(char c) { return is_alpha(c)||(c>='0'&&c<='9'); }

/* Decode one escape sequence, with `pos` just past the backslash.
 *
 * The old code understood only \n and \t and fell through to "keep the
 * character", which silently turned "\0" into the digit 0 and "\r" into the
 * letter r.  Unknown escapes now warn rather than passing quietly. */
static int lex_escape(void) {
    char e = src[pos++];
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case 'e': return 27;          /* common extension */
    case '\\': return '\\';
    case '\'': return '\'';
    case '"': return '"';
    case '?': return '?';
    case 'x': {
        int v = 0, n = 0;
        for (;;) {
            char h = src[pos];
            int d = -1;
            if (h >= '0' && h <= '9') d = h - '0';
            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
            if (d < 0) break;
            v = v * 16 + d;
            pos++;
            n++;
        }
        if (!n) warn_here("\\x with no hex digits");
        return v & 0xFF;
    }
    default:
        if (e >= '0' && e <= '7') {          /* octal, up to three digits */
            int v = e - '0';
            for (int n = 1; n < 3; n++) {
                char o = src[pos];
                if (o < '0' || o > '7') break;
                v = v * 8 + (o - '0');
                pos++;
            }
            return v & 0xFF;
        }
        warn_here("unknown escape sequence");
        return e;
    }
}

static void next_token(void) {
    skip_ws();
    int start = pos;
    tok.pos = start;
    tok.end = start;
    char c = src[pos];
    tok.text[0] = 0;
    tok.ival = 0;
    tok.fval = 0.0f;
    if (!c) { tok.kind = TOK_EOF; tok.end = pos; return; }

    if (is_alpha(c)) {
        int i = 0;
        while (is_alnum(src[pos]) && i < (TOK_TEXT_MAX - 1)) tok.text[i++] = src[pos++];
        tok.text[i] = 0;
        if (!strcmp(tok.text, "return")) tok.kind = TOK_RETURN;
        else if (!strcmp(tok.text, "if")) tok.kind = TOK_IF;
        else if (!strcmp(tok.text, "else")) tok.kind = TOK_ELSE;
        else if (!strcmp(tok.text, "while")) tok.kind = TOK_WHILE;
        else if (!strcmp(tok.text, "for")) tok.kind = TOK_FOR;
        else if (!strcmp(tok.text, "do")) tok.kind = TOK_DO;
        else if (!strcmp(tok.text, "goto")) tok.kind = TOK_GOTO;
        else if (!strcmp(tok.text, "switch")) tok.kind = TOK_SWITCH;
        else if (!strcmp(tok.text, "case")) tok.kind = TOK_CASE;
        else if (!strcmp(tok.text, "default")) tok.kind = TOK_DEFAULT;
        else if (!strcmp(tok.text, "break")) tok.kind = TOK_BREAK;
        else if (!strcmp(tok.text, "continue")) tok.kind = TOK_CONTINUE;
        else if (!strcmp(tok.text, "typedef")) tok.kind = TOK_TYPEDEF;
        else if (!strcmp(tok.text, "struct")) tok.kind = TOK_STRUCT;
        else if (!strcmp(tok.text, "union")) tok.kind = TOK_UNION;
        else if (!strcmp(tok.text, "extern")) tok.kind = TOK_EXTERN;
        else if (!strcmp(tok.text, "enum")) tok.kind = TOK_ENUM;
        else if (!strcmp(tok.text, "unsigned")) tok.kind = TOK_UNSIGNED;
        else if (!strcmp(tok.text, "signed")) tok.kind = TOK_SIGNED;
        else if (!strcmp(tok.text, "int")) tok.kind = TOK_INT;
        else if (!strcmp(tok.text, "float")) tok.kind = TOK_FLOAT;
        else if (!strcmp(tok.text, "char")) tok.kind = TOK_KW_CHAR;
        else if (!strcmp(tok.text, "bool") || !strcmp(tok.text, "_Bool")) tok.kind = TOK_BOOL;
        else if (!strcmp(tok.text, "void")) tok.kind = TOK_VOID;
        else if (!strcmp(tok.text, "sizeof")) tok.kind = TOK_SIZEOF;
        else if (!strcmp(tok.text, "const")) tok.kind = TOK_CONST;
        else if (!strcmp(tok.text, "volatile")) tok.kind = TOK_VOLATILE;
        else if (!strcmp(tok.text, "static")) tok.kind = TOK_STATIC;
        else if (!strcmp(tok.text, "auto")) tok.kind = TOK_AUTO;
        else if (!strcmp(tok.text, "register")) tok.kind = TOK_REGISTER;
        else if (!strcmp(tok.text, "short")) tok.kind = TOK_SHORT;
        else if (!strcmp(tok.text, "long")) tok.kind = TOK_LONG;
        else tok.kind = TOK_ID;
        tok.end = pos;
        return;
    }

    if (c>='0' && c<='9') {
        int start = pos;
        int has_dot = 0;
        if (src[pos] == '0' && (src[pos+1] == 'x' || src[pos+1] == 'X')) {
            pos += 2;
            int v = 0;
            while (1) {
                char h = src[pos];
                int hv = -1;
                if (h >= '0' && h <= '9') hv = h - '0';
                else if (h >= 'a' && h <= 'f') hv = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') hv = h - 'A' + 10;
                if (hv < 0) break;
                v = (v << 4) + hv;
                pos++;
            }
            tok.kind = TOK_NUM;
            tok.ival = v;
            while (src[pos]=='u'||src[pos]=='U'||src[pos]=='l'||src[pos]=='L') pos++;
            tok.end = pos;
            return;
        }
        while ((src[pos]>='0' && src[pos]<='9') || src[pos]=='.') {
            if (src[pos]=='.') has_dot = 1;
            pos++;
        }
        int len = pos - start;   /* mantissa only -- the exponent is scanned below */

        /* Exponent form: 1.5e3, 1e-5, 2E+8.  An 'e' only starts one when at
         * least one digit follows, so `3.0extra` still lexes as a number and
         * then an identifier rather than swallowing the name. */
        int has_exp = 0, exp_neg = 0, exp_val = 0;
        if (src[pos]=='e' || src[pos]=='E') {
            int q = pos + 1;
            int sgn = 0;
            if (src[q]=='+' || src[q]=='-') { sgn = (src[q]=='-'); q++; }
            if (src[q] >= '0' && src[q] <= '9') {
                has_exp = 1;
                exp_neg = sgn;
                while (src[q] >= '0' && src[q] <= '9') { exp_val = exp_val*10 + (src[q]-'0'); q++; }
                pos = q;
            }
        }

        if (!has_dot && !has_exp) {
            /* Accumulate integers as integers.  The old path ran every decimal
             * literal through a float, whose 24-bit mantissa silently mangles
             * anything past 16777216 -- so 4000000000 never survived. */
            unsigned int uv = 0;
            for (int k = 0; k < len; k++) uv = uv * 10u + (unsigned int)(src[start+k] - '0');
            tok.kind = TOK_NUM;
            tok.ival = (int)uv;
            while (src[pos]=='u'||src[pos]=='U'||src[pos]=='l'||src[pos]=='L') pos++;
            tok.end = pos;
            return;
        }
        /* Gather every digit into one integer mantissa and scale it once.
         * Scaling a float by 0.1 per digit rounds at every step and puts 0.1
         * and friends in the wrong place; a single multiply or divide by an
         * exact power of ten rounds once, which is the best a float can do.
         *
         * No double here on purpose: cc has no such type, and cc.c has to stay
         * inside the language cc accepts for it to compile itself. */
        unsigned int mant = 0;
        int mdigits = 0;
        int frac_digits = 0;
        int seen_dot = 0;
        int i = 0;
        for (i = 0; i < len; i++) {
            char d = src[start+i];
            if (d == '.') { seen_dot = 1; continue; }
            /* A float carries ~7 digits; past 9 the rest only overflow mant.
             * Digits dropped before the point still have to shift the value. */
            if (mdigits < 9) {
                mant = mant * 10u + (unsigned int)(d - '0');
                mdigits++;
                if (seen_dot) frac_digits++;
            } else if (!seen_dot) {
                frac_digits--;
            }
        }

        int e10 = (has_exp ? (exp_neg ? -exp_val : exp_val) : 0) - frac_digits;
        tok.kind = TOK_FNUM;
        tok.fval = scale_pow10((float)mant, e10);
        if (src[pos]=='f'||src[pos]=='F') pos++;
        tok.end = pos;
        return;
    }

    if (c=='"') {
        pos++;
        int i=0;
        while (src[pos] && src[pos] != '"' && i < (TOK_TEXT_MAX - 1)) {
            int ch = src[pos++];
            if (ch=='\\') ch = lex_escape();
            tok.text[i++] = (char)ch;
        }
        tok.text[i]=0;
        if (src[pos]=='"') pos++;
        tok.kind = TOK_STR;
        tok.end = pos;
        return;
    }

    if (c=='\'') {
        pos++;
        int ch = src[pos++];
        if (ch=='\\') ch = lex_escape();
        if (src[pos]=='\'') pos++;
        tok.kind = TOK_CHAR;
        tok.ival = (int)(unsigned char)ch;
        tok.end = pos;
        return;
    }

    pos++;
    switch (c) {
    case '(': tok.kind = TOK_LPAREN; tok.end = pos; return;
    case ')': tok.kind = TOK_RPAREN; tok.end = pos; return;
    case '{': tok.kind = TOK_LBRACE; tok.end = pos; return;
    case '}': tok.kind = TOK_RBRACE; tok.end = pos; return;
    case '[': tok.kind = TOK_LBRACK; tok.end = pos; return;
    case ']': tok.kind = TOK_RBRACK; tok.end = pos; return;
    case ';': tok.kind = TOK_SEMI; tok.end = pos; return;
    case ',': tok.kind = TOK_COMMA; tok.end = pos; return;
    case ':': tok.kind = TOK_COLON; tok.end = pos; return;
    case '?': tok.kind = TOK_QUESTION; tok.end = pos; return;
    case '.':
        if (src[pos] == '.' && src[pos+1] == '.') { pos += 2; tok.kind = TOK_ELLIPSIS; tok.end = pos; return; }
        tok.kind = TOK_DOT; tok.end = pos; return;
    case '+':
        if (src[pos] == '+') { pos++; tok.kind = TOK_INC; tok.end = pos; return; }
        if (src[pos] == '=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = '+'; tok.end = pos; return; }
        tok.kind = TOK_PLUS; tok.end = pos; return;
    case '-':
        if (src[pos] == '>') { pos++; tok.kind = TOK_ARROW; tok.end = pos; return; }
        if (src[pos] == '-') { pos++; tok.kind = TOK_DEC; tok.end = pos; return; }
        if (src[pos] == '=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = '-'; tok.end = pos; return; }
        tok.kind = TOK_MINUS; tok.end = pos; return;
    case '*':
        if (src[pos] == '=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = '*'; tok.end = pos; return; }
        tok.kind = TOK_MUL; tok.end = pos; return;
    case '/':
        if (src[pos] == '=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = '/'; tok.end = pos; return; }
        tok.kind = TOK_DIV; tok.end = pos; return;
    case '%':
        if (src[pos] == '=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = '%'; tok.end = pos; return; }
        tok.kind = TOK_MOD; tok.end = pos; return;
    case '&':
        if (src[pos] == '&') { pos++; tok.kind = TOK_LAND; tok.end = pos; return; }
        if (src[pos] == '=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = '&'; tok.end = pos; return; }
        tok.kind = TOK_AMP; tok.end = pos; return;
    case '|':
        if (src[pos] == '|') { pos++; tok.kind = TOK_LOR; tok.end = pos; return; }
        if (src[pos] == '=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = '|'; tok.end = pos; return; }
        tok.kind = TOK_OR; tok.end = pos; return;
    case '^':
        if (src[pos] == '=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = '^'; tok.end = pos; return; }
        tok.kind = TOK_XOR; tok.end = pos; return;
    case '~': tok.kind = TOK_TILDE; tok.end = pos; return;
    case '=':
        if (src[pos]=='=') { pos++; tok.kind = TOK_EQ; }
        else tok.kind = TOK_ASSIGN;
        tok.end = pos;
        return;
    case '!':
        if (src[pos]=='=') { pos++; tok.kind = TOK_NE; tok.end = pos; return; }
        tok.kind = TOK_LOGNOT; tok.end = pos; return;
    case '<':
        if (src[pos]=='<') {
            pos++;
            if (src[pos]=='=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = 'L'; tok.end = pos; return; }
            tok.kind = TOK_SHL;
        }
        else if (src[pos]=='=') { pos++; tok.kind = TOK_LE; }
        else tok.kind = TOK_LT;
        tok.end = pos;
        return;
    case '>':
        if (src[pos]=='>') {
            pos++;
            if (src[pos]=='=') { pos++; tok.kind = TOK_OPASSIGN; tok.ival = 'R'; tok.end = pos; return; }
            tok.kind = TOK_SHR;
        }
        else if (src[pos]=='=') { pos++; tok.kind = TOK_GE; }
        else tok.kind = TOK_GT;
        tok.end = pos;
        return;
    }
    pos--;
    error_here("invalid character");
}

static int consume(int k) { if (tok.kind == k) { next_token(); return 1; } return 0; }
static void expect(int k) { if (tok.kind != k) { error_expected(k); } next_token(); }

static Node *new_node(int kind) { if (node_len >= MAX_NODE) { puts("node ovf\n"); sys_exit(1); } Node *n = &nodes[node_len++]; memset(n,0,sizeof(*n)); n->kind = kind; return n; }

static int align_to(int n, int a) { return (n + a - 1) & ~(a - 1); }

static Type *new_type(TypeKind kind, Type *base, int array_len, StructDef *sdef) {
    if (type_pool_len >= MAX_TYPE) { puts("type ovf\n"); sys_exit(1); }
    Type *t = &type_pool[type_pool_len++];
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    t->base = base;
    t->array_len = array_len;
    t->sdef = sdef;
    t->is_unsigned = 0;
    if (kind == TY_PTR) { t->size = 4; t->align = 4; }
    else if (kind == TY_ARRAY) { t->size = base->size * array_len; t->align = base->align; }
    else if (kind == TY_STRUCT || kind == TY_UNION) { t->size = sdef->size; t->align = sdef->align; }
    return t;
}

static Type *type_ptr(Type *base) { return new_type(TY_PTR, base, 0, 0); }
static Type *type_array(Type *base, int len) { return new_type(TY_ARRAY, base, len, 0); }
static Type *type_struct(StructDef *sdef) {
    if (sdef->type) return sdef->type;
    sdef->type = new_type(TY_STRUCT, 0, 0, sdef);
    return sdef->type;
}
static Type *type_union(StructDef *sdef) {
    if (sdef->type) return sdef->type;
    sdef->type = new_type(TY_UNION, 0, 0, sdef);
    return sdef->type;
}

static int type_size(Type *t) { return t ? t->size : 0; }
static int type_align(Type *t) { return t ? t->align : 1; }
static int is_int(Type *t) { return t && t->kind == TY_INT; }
static int is_short(Type *t) { return t && t->kind == TY_SHORT; }
static int is_float(Type *t) { return t && t->kind == TY_FLOAT; }
static int is_char(Type *t) { return t && t->kind == TY_CHAR; }
static int is_bool(Type *t) { return t && t->kind == TY_BOOL; }
static int is_uchar(Type *t) { return t && t->kind == TY_CHAR && t->is_unsigned; }
static int is_uint(Type *t) { return t && t->kind == TY_INT && t->is_unsigned; }
static int is_void(Type *t) { return t && t->kind == TY_VOID; }
static int is_ptr(Type *t) { return t && t->kind == TY_PTR; }
static int is_array(Type *t) { return t && t->kind == TY_ARRAY; }
static int is_struct(Type *t) { return t && t->kind == TY_STRUCT; }
static int is_union(Type *t) { return t && t->kind == TY_UNION; }
static int is_record(Type *t) { return is_struct(t) || is_union(t); }
static int is_numeric(Type *t) { return is_int(t) || is_short(t) || is_float(t) || is_char(t) || is_bool(t); }
static int is_unsigned_type(Type *t) { return t && t->is_unsigned; }
static int is_byte(Type *t) { return is_char(t) || is_bool(t); }

static Type *decay_array(Type *t) { return is_array(t) ? type_ptr(t->base) : t; }
static Type *promote(Type *t) { return (is_char(t) || is_bool(t)) ? &type_int : t; }

static StructDef *find_struct(const char *name) {
    for (int i = 0; i < structs_len; i++) if (!strcmp(structs[i].name, name)) return &structs[i];
    return 0;
}

static StructDef *add_struct(const char *name, int is_union) {
    StructDef *s = 0;
    if (name && name[0]) {
        s = find_struct(name);
        if (s) {
            if (s->is_union != is_union) { puts("struct/union mismatch\n"); sys_exit(1); }
            return s;
        }
    }
    if (structs_len >= MAX_STRUCT) { puts("struct ovf\n"); sys_exit(1); }
    s = &structs[structs_len++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 31);
    s->align = 1;
    s->size = 0;
    s->type = 0;
    s->is_union = is_union;
    return s;
}

static void struct_layout(StructDef *s) {
    int off = 0;
    int maxa = 1;
    if (s->is_union) {
        int maxsz = 0;
        for (int i = 0; i < s->field_count; i++) {
            int a = type_align(s->fields[i].type);
            if (a < 1) a = 1;
            s->fields[i].offset = 0;
            int sz = type_size(s->fields[i].type);
            if (sz > maxsz) maxsz = sz;
            if (a > maxa) maxa = a;
        }
        s->align = maxa;
        s->size = align_to(maxsz, maxa);
    } else {
        for (int i = 0; i < s->field_count; i++) {
            int a = type_align(s->fields[i].type);
            if (a < 1) a = 1;
            off = align_to(off, a);
            s->fields[i].offset = off;
            off += type_size(s->fields[i].type);
            if (a > maxa) maxa = a;
        }
        s->align = maxa;
        s->size = align_to(off, maxa);
    }
    if (s->type) { s->type->size = s->size; s->type->align = s->align; }
}

static Typedef *find_typedef(const char *name) {
    for (int i = 0; i < typedefs_len; i++) if (!strcmp(typedefs[i].name, name)) return &typedefs[i];
    return 0;
}

static int find_enum_const(const char *name, int *out) {
    for (int i = 0; i < enum_const_len; i++) {
        if (!strcmp(enum_consts[i].name, name)) { if (out) *out = enum_consts[i].val; return 1; }
    }
    return 0;
}

static void add_enum_const(const char *name, int val) {
    if (enum_const_len >= 256) { puts("enum ovf\n"); sys_exit(1); }
    for (int i = 0; i < enum_const_len; i++) {
        if (!strcmp(enum_consts[i].name, name)) { enum_consts[i].val = val; return; }
    }
    EnumConst *e = &enum_consts[enum_const_len++];
    strncpy(e->name, name, 31);
    e->val = val;
}

static void add_typedef(const char *name, Type *type) {
    if (typedefs_len >= MAX_TYPEDEF) { puts("typedef ovf\n"); sys_exit(1); }
    Typedef *t = find_typedef(name);
    if (t) { t->type = type; return; }
    t = &typedefs[typedefs_len++];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, 31);
    t->type = type;
}

static void init_types(void) {
    memset(&type_int, 0, sizeof(type_int));
    type_int.kind = TY_INT; type_int.size = 4; type_int.align = 4; type_int.is_unsigned = 0;
    memset(&type_uint, 0, sizeof(type_uint));
    type_uint.kind = TY_INT; type_uint.size = 4; type_uint.align = 4; type_uint.is_unsigned = 1;
    memset(&type_float, 0, sizeof(type_float));
    type_float.kind = TY_FLOAT; type_float.size = 4; type_float.align = 4;
    memset(&type_char, 0, sizeof(type_char));
    type_char.kind = TY_CHAR; type_char.size = 1; type_char.align = 1; type_char.is_unsigned = 0;
    memset(&type_bool, 0, sizeof(type_bool));
    type_bool.kind = TY_BOOL; type_bool.size = 1; type_bool.align = 1; type_bool.is_unsigned = 1;
    memset(&type_uchar, 0, sizeof(type_uchar));
    type_uchar.kind = TY_CHAR; type_uchar.size = 1; type_uchar.align = 1; type_uchar.is_unsigned = 1;
    memset(&type_short, 0, sizeof(type_short));
    type_short.kind = TY_SHORT; type_short.size = 2; type_short.align = 2; type_short.is_unsigned = 0;
    memset(&type_ushort, 0, sizeof(type_ushort));
    type_ushort.kind = TY_SHORT; type_ushort.size = 2; type_ushort.align = 2; type_ushort.is_unsigned = 1;
    memset(&type_void, 0, sizeof(type_void));
    type_void.kind = TY_VOID; type_void.size = 0; type_void.align = 1;

    /* builtin typedef: va_list -> char* */
    add_typedef("va_list", type_ptr(&type_char));
}

/* Backwards, so the innermost declaration of a name shadows an outer one.
 * Searching forwards meant a `Field *f` in one block was masked by a `Func *f`
 * declared earlier in a sibling block, and the member lookup then failed
 * against the wrong struct. */
static Sym *find_local(const char *name) {
    for (int i=locals_len-1;i>=0;i--) if (!strcmp(locals[i].name,name)) return &locals[i];
    return 0;
}

static Sym *find_global(const char *name) {
    for (int i=0;i<globals_len;i++) if (!strcmp(globals[i].name,name)) return &globals[i];
    return 0;
}

static Func *find_func(const char *name) {
    for (int i=0;i<funcs_len;i++) if (!strcmp(funcs[i].name,name)) return &funcs[i];
    return 0;
}

static void sym_init_name(Sym *s, const char *name) {
    strncpy(s->name, name, 31);
    s->name[31] = 0;
    s->link_name[0] = 0;
    s->is_static = 0;
}

static void sym_set_link_name(Sym *s, int is_static) {
    s->is_static = is_static;
    if (is_static) {
        make_static_link_name(s->link_name);
    } else {
        strncpy(s->link_name, s->name, 31);
        s->link_name[31] = 0;
    }
}

static void func_init_name(Func *f, const char *name) {
    strncpy(f->name, name, 31);
    f->name[31] = 0;
    f->link_name[0] = 0;
    f->is_static = 0;
}

static void func_set_link_name(Func *f, int is_static) {
    f->is_static = is_static;
    if (is_static) {
        make_static_link_name(f->link_name);
    } else {
        strncpy(f->link_name, f->name, 31);
        f->link_name[31] = 0;
    }
}

static void func_make_static(Func *f) {
    if (f->is_static) return;
    char old[32];
    if (f->link_name[0]) strncpy(old, f->link_name, 31);
    else strncpy(old, f->name, 31);
    old[31] = 0;
    func_set_link_name(f, 1);
    for (int i = 0; i < sym_fix_len; i++) {
        if (!strcmp(sym_fix[i].name, old)) {
            strncpy(sym_fix[i].name, f->link_name, 31);
        }
    }
}

static Sym *add_local(const char *name, Type *ty) {
    if (locals_len >= MAX_SYM) { puts("local ovf\n"); sys_exit(1); }
    int size = type_size(ty);
    int align = type_align(ty);
    if (align < 4) align = 4;
    local_offset -= size;
    local_offset &= ~(align - 1);
    Sym *s = &locals[locals_len++];
    memset(s,0,sizeof(*s));
    sym_init_name(s, name);
    s->type = ty;
    s->offset = local_offset;
    s->is_global = 0;
    s->is_extern = 0;
    s->alias_global = -1;
    return s;
}

static Sym *add_param(const char *name, Type *ty, int offset) {
    if (locals_len >= MAX_SYM) { puts("param ovf\n"); sys_exit(1); }
    Sym *s = &locals[locals_len++];
    memset(s,0,sizeof(*s));
    sym_init_name(s, name);
    s->type = ty;
    s->offset = offset;
    s->is_global = 0;
    s->is_extern = 0;
    s->alias_global = -1;
    return s;
}

static Sym *add_local_alias(const char *name, Type *ty, int global_index) {
    if (locals_len >= MAX_SYM) { puts("local ovf\n"); sys_exit(1); }
    Sym *s = &locals[locals_len++];
    memset(s,0,sizeof(*s));
    sym_init_name(s, name);
    s->type = ty;
    s->offset = 0;
    s->is_global = 0;
    s->is_extern = 0;
    s->alias_global = global_index;
    return s;
}

static Sym *add_global_data(const char *name, Type *ty) {
    if (globals_len >= MAX_SYM) { puts("global ovf\n"); sys_exit(1); }
    int align = type_align(ty);
    if (align < 1) align = 1;
    int off = align_to(data_len, align);
    if (off > data_len) {
        for (int i = data_len; i < off && i < MAX_DATA; i++) data_seg[i] = 0;
    }
    int size = type_size(ty);
    if (off + size > MAX_DATA) { puts("data ovf\n"); sys_exit(1); }
    Sym *s = &globals[globals_len++];
    memset(s,0,sizeof(*s));
    sym_init_name(s, name);
    s->type = ty;
    s->offset = off;
    s->is_global = 1;
    s->is_extern = 0;
    s->section = SEC_DATA;
    s->alias_global = -1;
    sym_set_link_name(s, 0);
    if (off + size > MAX_DATA) { puts("data ovf\n"); sys_exit(1); }
    for (int i = 0; i < size; i++) data_seg[off + i] = 0;
    data_len = off + size;
    return s;
}

static Sym *define_global_data_existing(Sym *s, Type *ty) {
    int align = type_align(ty);
    if (align < 1) align = 1;
    int off = align_to(data_len, align);
    if (off > data_len) {
        for (int i = data_len; i < off && i < MAX_DATA; i++) data_seg[i] = 0;
    }
    int size = type_size(ty);
    if (off + size > MAX_DATA) { puts("data ovf\n"); sys_exit(1); }
    s->type = ty;
    s->offset = off;
    s->is_global = 1;
    s->is_extern = 0;
    s->section = SEC_DATA;
    s->alias_global = -1;
    if (off + size > MAX_DATA) { puts("data ovf\n"); sys_exit(1); }
    for (int i = 0; i < size; i++) data_seg[off + i] = 0;
    data_len = off + size;
    return s;
}

static Sym *add_global_bss(const char *name, Type *ty) {
    if (globals_len >= MAX_SYM) { puts("global ovf\n"); sys_exit(1); }
    int align = type_align(ty);
    if (align < 1) align = 1;
    int off = align_to(bss_len, align);
    int size = type_size(ty);
    if (off + size > MAX_BSS) { puts("bss ovf\n"); sys_exit(1); }
    Sym *s = &globals[globals_len++];
    memset(s,0,sizeof(*s));
    sym_init_name(s, name);
    s->type = ty;
    s->offset = off;
    s->is_global = 1;
    s->is_extern = 0;
    s->section = SEC_BSS;
    s->alias_global = -1;
    sym_set_link_name(s, 0);
    bss_len = off + size;
    return s;
}

static Sym *define_global_bss_existing(Sym *s, Type *ty) {
    int align = type_align(ty);
    if (align < 1) align = 1;
    int off = align_to(bss_len, align);
    int size = type_size(ty);
    if (off + size > MAX_BSS) { puts("bss ovf\n"); sys_exit(1); }
    s->type = ty;
    s->offset = off;
    s->is_global = 1;
    s->is_extern = 0;
    s->section = SEC_BSS;
    s->alias_global = -1;
    bss_len = off + size;
    return s;
}

static Sym *add_global_decl(const char *name, Type *ty) {
    if (globals_len >= MAX_SYM) { puts("global ovf\n"); sys_exit(1); }
    Sym *s = find_global(name);
    if (s) return s;
    s = &globals[globals_len++];
    memset(s,0,sizeof(*s));
    sym_init_name(s, name);
    s->type = ty;
    s->offset = 0;
    s->is_global = 1;
    s->is_extern = 1;
    s->section = 0;
    s->alias_global = -1;
    sym_set_link_name(s, 0);
    return s;
}

static Func *add_func(const char *name, Type *ret) {
    Func *f = find_func(name);
    if (!f) {
        f = &funcs[funcs_len++];
        memset(f,0,sizeof(*f));
        func_init_name(f, name);
        f->label = new_label();
        f->declared = 0;
        func_set_link_name(f, 0);
    }
    f->ret = ret;
    return f;
}

static void emit_start_stub(void) {
    /* entry: call main, exit with its return code */
    Func *m = add_func("main", &type_int);
    emit_call_label(m->label);
    emit8(0x89); emit8(0xC3); /* mov ebx, eax */
    emit_mov_eax_imm(6); /* SYS_EXIT */
    emit8(0xCD); emit8(0x80);
    emit8(0xEB); emit8(0xFE); /* jmp $ */
}

enum { ND_NUM, ND_FNUM, ND_VAR, ND_BIN, ND_ASSIGN, ND_CALL, ND_CAST, ND_UNARY, ND_ADDR, ND_DEREF, ND_INDEX, ND_MEMBER, ND_VAARG, ND_STR, ND_TERN };

static Node *parse_expr(void);

static Type *parse_struct_spec(int is_union);
static Node *parse_shift(void);
static Node *parse_cond(void);
static int eval_const(Node *n, int *out);
static Node *parse_bitand(void);
static Node *parse_bitxor(void);
static Node *parse_bitor(void);
static Node *parse_land(void);
static Node *parse_lor(void);

/* Consume any run of cv-qualifiers.  C lets these appear before the type,
 * after it, and after each '*' in a declarator; the parser previously only
 * handled the leading position, so "int const *p" and "char * const p" were
 * both rejected. */
static void skip_cv(void) {
    while (consume(TOK_CONST) || consume(TOK_VOLATILE)) { }
}

static Type *parse_type_spec(void) {
    int uns = 0;
    int saw_spec = 0;
    int longc = 0;
    int shortc = 0;
    for (;;) {
        if (consume(TOK_CONST) || consume(TOK_VOLATILE)) { continue; }
        if (consume(TOK_UNSIGNED)) { uns = 1; saw_spec = 1; continue; }
        if (consume(TOK_SIGNED)) { uns = 0; saw_spec = 1; continue; }
        if (consume(TOK_LONG)) { longc++; saw_spec = 1; continue; }
        if (consume(TOK_SHORT)) { shortc++; saw_spec = 1; continue; }
        break;
    }
    /* "short" and "short int" are a real 16-bit type; "long" stays 32-bit,
     * which is correct for this ILP32 target. */
    if (shortc) { consume(TOK_INT); skip_cv(); return uns ? &type_ushort : &type_short; }
    if (consume(TOK_INT)) { skip_cv(); return uns ? &type_uint : &type_int; }
    if (consume(TOK_FLOAT)) { skip_cv(); return &type_float; }
    if (consume(TOK_KW_CHAR)) { skip_cv(); return uns ? &type_uchar : &type_char; }
    if (consume(TOK_BOOL)) { skip_cv(); return &type_bool; }
    if (consume(TOK_VOID)) { skip_cv(); return &type_void; }
    if (consume(TOK_STRUCT)) return parse_struct_spec(0);
    if (consume(TOK_UNION)) return parse_struct_spec(1);
    if (consume(TOK_ENUM)) {
        if (tok.kind == TOK_ID) { next_token(); }
        if (consume(TOK_LBRACE)) {
            int val = 0;
            for (;;) {
                if (tok.kind != TOK_ID) { error_here("enum name\n"); }
                char ename[32]; strncpy(ename, tok.text, 31);
                next_token();
                if (consume(TOK_ASSIGN)) {
                    int neg = 0;
                    if (consume(TOK_MINUS)) neg = 1;
                    if (tok.kind != TOK_NUM && tok.kind != TOK_CHAR) { error_here("enum value\n"); }
                    val = tok.ival;
                    if (neg) val = -val;
                    next_token();
                }
                add_enum_const(ename, val);
                val++;
                if (consume(TOK_COMMA)) {
                    if (consume(TOK_RBRACE)) break;
                    continue;
                }
                expect(TOK_RBRACE);
                break;
            }
        }
        return &type_int;
    }
    if (uns || longc || saw_spec) { skip_cv(); return uns ? &type_uint : &type_int; }
    if (tok.kind == TOK_ID) {
        Typedef *td = find_typedef(tok.text);
        if (td) { next_token(); skip_cv(); return td->type; }
    }
    return 0;
}

static int is_type_start(void) {
    if (tok.kind == TOK_INT || tok.kind == TOK_FLOAT || tok.kind == TOK_KW_CHAR || tok.kind == TOK_BOOL || tok.kind == TOK_VOID || tok.kind == TOK_STRUCT || tok.kind == TOK_UNION || tok.kind == TOK_ENUM) return 1;
    if (tok.kind == TOK_UNSIGNED || tok.kind == TOK_SIGNED || tok.kind == TOK_SHORT || tok.kind == TOK_LONG) return 1;
    if (tok.kind == TOK_CONST || tok.kind == TOK_VOLATILE || tok.kind == TOK_STATIC || tok.kind == TOK_EXTERN || tok.kind == TOK_TYPEDEF || tok.kind == TOK_REGISTER || tok.kind == TOK_AUTO) return 1;
    if (tok.kind == TOK_ID && find_typedef(tok.text)) return 1;
    return 0;
}

static Type *parse_declarator(Type *base, char *out_name) {
    while (consume(TOK_MUL)) { base = type_ptr(base); skip_cv(); }
    if (consume(TOK_LPAREN)) {
        int inner_ptr = 0;
        while (consume(TOK_MUL)) { inner_ptr++; skip_cv(); }
        if (tok.kind != TOK_ID) { error_here("decl name\n"); }
        strncpy(out_name, tok.text, 31);
        next_token();
        expect(TOK_RPAREN);
        while (inner_ptr--) base = type_ptr(base);
        if (consume(TOK_LPAREN)) {
            int depth = 1;
            while (tok.kind != TOK_EOF && depth > 0) {
                if (consume(TOK_LPAREN)) { depth++; continue; }
                if (consume(TOK_RPAREN)) { depth--; continue; }
                next_token();
            }
        }
    } else {
        if (tok.kind != TOK_ID) { error_here("decl name\n"); }
        strncpy(out_name, tok.text, 31);
        next_token();
    }
    /* Suffixes are collected first and applied right-to-left: in
     * `char x[2][3]` the FIRST suffix is the OUTERmost dimension, so the
     * last one must wrap `base` first.  Applying them as they are read
     * inverts the nesting -- x[i] then strides by 2 instead of 3, and a
     * `char names[][64]` parameter decays to a pointer whose element size
     * is 0, collapsing every row onto row 0. */
    int dims[8];
    int ndims = 0;
    while (consume(TOK_LBRACK)) {
        /* "[]" leaves the length to be filled in from the initialiser; the
         * declaration sites replace the type once they have seen it. */
        int len = 0;
        if (tok.kind == TOK_RBRACK) {
            next_token();
        } else {
            /* A constant expression, not just a literal: `buf[MAX + 1]` and
             * `t[sizeof(x)]` are ordinary C and cc.c is full of them. */
            if (!eval_const(parse_cond(), &len)) { error_here("array size not constant\n"); }
            if (len < 0) { error_here("negative array size\n"); }
            expect(TOK_RBRACK);
        }
        if (ndims >= 8) { error_here("array dims\n"); }
        dims[ndims++] = len;
    }
    for (int i = ndims - 1; i >= 0; i--) base = type_array(base, dims[i]);
    return base;
}

static Type *parse_struct_spec(int is_union) {
    char sname[32]; sname[0] = 0;
    if (tok.kind == TOK_ID) { strncpy(sname, tok.text, 31); next_token(); }
    StructDef *sd = 0;
    if (consume(TOK_LBRACE)) {
        sd = add_struct(sname, is_union);
        sd->field_count = 0;
        while (!consume(TOK_RBRACE)) {
            Type *ft = parse_type_spec();
            if (!ft) { error_here("field type\n"); }
            for (;;) {
                if (sd->field_count >= MAX_FIELD) { puts("field ovf\n"); sys_exit(1); }
                char fname[32];
                Type *fty = parse_declarator(ft, fname);
                Field *f = &sd->fields[sd->field_count++];
                memset(f, 0, sizeof(*f));
                strncpy(f->name, fname, 31);
                f->type = fty;
                if (consume(TOK_COMMA)) continue;
                expect(TOK_SEMI);
                break;
            }
        }
        struct_layout(sd);
    } else {
        if (!sname[0]) { error_here("struct name\n"); }
        /* A tag with no body is a forward declaration: create it incomplete
         * (size 0) and let the later definition fill it in.  struct_layout
         * refreshes the cached Type, so pointers taken before the body is
         * seen get the right size once it arrives.  This is what makes the
         * self-referential `typedef struct Node Node;` idiom work, and cc.c
         * is written that way. */
        sd = add_struct(sname, is_union);
    }
    return is_union ? type_union(sd) : type_struct(sd);
}

static Type *parse_type_name(void) {
    Type *base = parse_type_spec();
    if (!base) { puts("type?\n"); sys_exit(1); }
    while (consume(TOK_MUL)) { base = type_ptr(base); skip_cv(); }
    return base;
}

static Type *infer_type(Node *n) {
    if (!n) return &type_int;
    switch (n->kind) {
    case ND_NUM: return &type_int;
    case ND_FNUM: return &type_float;
    case ND_STR: return type_ptr(&type_char);
    case ND_VAR: {
        Sym *s = find_local(n->name);
        if (!s) s = find_global(n->name);
        if (!s) {
            Func *f = find_func(n->name);
            if (f) return type_ptr(&type_void);
            error_name("unknown var", n->name);
        }
        return s->type;
    }
    case ND_ADDR: {
        if (n->lhs && n->lhs->kind == ND_VAR) {
            Func *f = find_func(n->lhs->name);
            if (f) return type_ptr(&type_void);
        }
        Type *t = infer_type(n->lhs);
        return type_ptr(t);
    }
    case ND_DEREF: {
        Type *t = infer_type(n->lhs);
        if (!is_ptr(t)) { puts("deref ptr\n"); sys_exit(1); }
        return t->base;
    }
    case ND_INDEX: {
        Type *t = infer_type(n->lhs);
        t = decay_array(t);
        if (!is_ptr(t)) { puts("index ptr\n"); sys_exit(1); }
        return t->base;
    }
    case ND_MEMBER: {
        Type *t = infer_type(n->lhs);
        if (is_ptr(t)) t = t->base;
        if (!is_record(t)) { puts("member struct\n"); sys_exit(1); }
        Field *f = find_field(t->sdef, n->name);
        if (!f) { puts("no field: "); puts(n->name); putc('\n'); error_here("unknown member\n"); }
        return f->type;
    }
    case ND_CAST:
        return n->type ? n->type : &type_int;
    case ND_CALL: {
        Func *f = find_func(n->name);
        if (f) return f->ret;
        return &type_int;
    }
    case ND_BIN:
    case ND_ASSIGN:
    case ND_UNARY:
        if (n->lhs) {
            Type *t = infer_type(n->lhs);
            if (is_unsigned_type(t)) return &type_uint;
        }
        return &type_int;
    case ND_VAARG:
        return n->type ? n->type : &type_int;
    case ND_TERN: {
        if (n->argc >= 3) {
            Type *ta = decay_array(infer_type(n->args[1]));
            Type *tb = decay_array(infer_type(n->args[2]));
            if (is_ptr(ta) && is_ptr(tb)) return ta;
            if (is_float(ta) || is_float(tb)) return &type_float;
            if (is_unsigned_type(ta) || is_unsigned_type(tb)) return &type_uint;
            return &type_int;
        }
        return &type_int;
    }
    default:
        return &type_int;
    }
}

static Node *parse_primary(void) {
    if (consume(TOK_LPAREN)) {
        Node *e = parse_expr();
        expect(TOK_RPAREN);
        return e;
    }
    if (tok.kind == TOK_NUM) {
        Node *n = new_node(ND_NUM);
        n->ival = tok.ival;
        next_token();
        return n;
    }
    if (tok.kind == TOK_FNUM) {
        Node *n = new_node(ND_FNUM);
        n->fval = tok.fval;
        next_token();
        return n;
    }
    if (tok.kind == TOK_CHAR) {
        Node *n = new_node(ND_NUM);
        n->ival = tok.ival;
        next_token();
        return n;
    }
    if (tok.kind == TOK_STR) {
        Node *n = new_node(ND_STR);
        strcpy(n->name, tok.text);
        next_token();
        return n;
    }
    if (tok.kind == TOK_ID) {
        int ev = 0;
        if (find_enum_const(tok.text, &ev)) {
            Node *n = new_node(ND_NUM);
            n->ival = ev;
            next_token();
            return n;
        }
        Node *v = new_node(ND_VAR);
        strncpy(v->name, tok.text, 31);
        next_token();
        return v;
    }
    error_here("expected expression");
    return 0;
}

/* ++, -- and "op=" are desugared here rather than given their own codegen.
 *
 *     ++x        ->  (x = x + 1)
 *     x++        ->  (x = x + 1) - 1      yields the old value
 *     x op= e    ->  (x = x op e)
 *
 * The postfix form works for pointers too, since pointer arithmetic scales
 * both the += and the compensating -=.  The one caveat is that the lvalue is
 * evaluated twice, so a side-effecting subscript such as a[f()]++ would call
 * f() twice; plain variables, members and constant indices are unaffected.
 */
static Node *node_int(int v) {
    Node *n = new_node(ND_NUM);
    n->ival = v;
    return n;
}

static Node *node_bin(int op, Node *a, Node *b) {
    Node *n = new_node(ND_BIN);
    n->op = op;
    n->lhs = a;
    n->rhs = b;
    return n;
}

static Node *node_assign(Node *lhs, Node *rhs) {
    Node *n = new_node(ND_ASSIGN);
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

/* x = x <op> 1 */
static Node *node_step(Node *lv, int op) {
    return node_assign(lv, node_bin(op, lv, node_int(1)));
}

static Node *parse_postfix(void) {
    Node *n = parse_primary();
    for (;;) {
        if (consume(TOK_LPAREN)) {
            Node *call = new_node(ND_CALL);
            if (n->kind == ND_VAR) strncpy(call->name, n->name, 31);
            call->callee = n;
            call->argc = 0;
            if (!consume(TOK_RPAREN)) {
                if (!strcmp(call->name, "va_arg")) {
                    Node *ap = parse_expr();
                    expect(TOK_COMMA);
                    Type *ty = parse_type_name();
                    expect(TOK_RPAREN);
                    Node *va = new_node(ND_VAARG);
                    va->lhs = ap;
                    va->type = ty;
                    n = va;
                    continue;
                } else {
                    do {
                        if (call->argc >= MAX_PARAM) { puts("too many args\n"); sys_exit(1); }
                        call->args[call->argc++] = parse_expr();
                    } while (consume(TOK_COMMA));
                    expect(TOK_RPAREN);
                }
            }
            n = call;
            continue;
        }
        if (consume(TOK_LBRACK)) {
            Node *idx = parse_expr();
            expect(TOK_RBRACK);
            Node *in = new_node(ND_INDEX);
            in->lhs = n;
            in->rhs = idx;
            n = in;
            continue;
        }
        if (consume(TOK_DOT)) {
            if (tok.kind != TOK_ID) { error_here("member\n"); }
            Node *m = new_node(ND_MEMBER);
            m->lhs = n;
            m->op = '.';
            strncpy(m->name, tok.text, 31);
            next_token();
            n = m;
            continue;
        }
        if (consume(TOK_ARROW)) {
            if (tok.kind != TOK_ID) { error_here("member\n"); }
            Node *m = new_node(ND_MEMBER);
            m->lhs = n;
            m->op = '>';
            strncpy(m->name, tok.text, 31);
            next_token();
            n = m;
            continue;
        }
        if (consume(TOK_INC)) { n = node_bin('-', node_step(n, '+'), node_int(1)); continue; }
        if (consume(TOK_DEC)) { n = node_bin('+', node_step(n, '-'), node_int(1)); continue; }
        break;
    }
    return n;
}

static Node *parse_unary(void) {
    if (consume(TOK_INC)) return node_step(parse_unary(), '+');
    if (consume(TOK_DEC)) return node_step(parse_unary(), '-');
    if (consume(TOK_SIZEOF)) {
        if (consume(TOK_LPAREN)) {
            int save_pos = pos;
            Token save_tok = tok;
            Type *ct = parse_type_spec();
            if (ct) {
                while (consume(TOK_MUL)) ct = type_ptr(ct);
                expect(TOK_RPAREN);
                Node *n = new_node(ND_NUM);
                n->ival = type_size(ct);
                return n;
            }
            pos = save_pos;
            tok = save_tok;
            Node *e = parse_expr();
            expect(TOK_RPAREN);
            Type *t = infer_type(e);
            Node *n = new_node(ND_NUM);
            n->ival = type_size(t);
            return n;
        } else {
            Node *e = parse_unary();
            Type *t = infer_type(e);
            Node *n = new_node(ND_NUM);
            n->ival = type_size(t);
            return n;
        }
    }
    if (consume(TOK_MINUS)) {
        Node *n = new_node(ND_UNARY);
        n->op = '-';
        n->lhs = parse_unary();
        return n;
    }
    if (consume(TOK_PLUS)) return parse_unary();
    if (consume(TOK_LOGNOT)) {
        Node *n = new_node(ND_UNARY);
        n->op = '!';
        n->lhs = parse_unary();
        return n;
    }
    if (consume(TOK_TILDE)) {
        Node *n = new_node(ND_UNARY);
        n->op = '~';
        n->lhs = parse_unary();
        return n;
    }
    if (consume(TOK_AMP)) {
        Node *n = new_node(ND_ADDR);
        n->lhs = parse_unary();
        return n;
    }
    if (consume(TOK_MUL)) {
        Node *n = new_node(ND_DEREF);
        n->lhs = parse_unary();
        return n;
    }
    if (tok.kind == TOK_LPAREN) {
        int save_pos = pos;
        Token save_tok = tok;
        next_token(); /* consume '(' */
        Type *ct = parse_type_spec();
        if (ct) {
            while (consume(TOK_MUL)) ct = type_ptr(ct);
            if (consume(TOK_RPAREN)) {
            Node *n = new_node(ND_CAST);
            n->type = ct;
            n->lhs = parse_unary();
            return n;
            }
        }
        pos = save_pos;
        tok = save_tok;
    }
    return parse_postfix();
}

static Node *parse_mul(void) {
    Node *n = parse_unary();
    for (;;) {
        if (consume(TOK_MUL)) { Node *b = new_node(ND_BIN); b->op='*'; b->lhs=n; b->rhs=parse_unary(); n=b; }
        else if (consume(TOK_DIV)) { Node *b = new_node(ND_BIN); b->op='/'; b->lhs=n; b->rhs=parse_unary(); n=b; }
        else if (consume(TOK_MOD)) { Node *b = new_node(ND_BIN); b->op='%'; b->lhs=n; b->rhs=parse_unary(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_add(void) {
    Node *n = parse_mul();
    for (;;) {
        if (consume(TOK_PLUS)) { Node *b = new_node(ND_BIN); b->op='+'; b->lhs=n; b->rhs=parse_mul(); n=b; }
        else if (consume(TOK_MINUS)) { Node *b = new_node(ND_BIN); b->op='-'; b->lhs=n; b->rhs=parse_mul(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_shift(void) {
    Node *n = parse_add();
    for (;;) {
        if (consume(TOK_SHL)) { Node *b=new_node(ND_BIN); b->op='L'; b->lhs=n; b->rhs=parse_add(); n=b; }
        else if (consume(TOK_SHR)) { Node *b=new_node(ND_BIN); b->op='R'; b->lhs=n; b->rhs=parse_add(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_rel(void) {
    Node *n = parse_shift();
    for (;;) {
        if (consume(TOK_LT)) { Node *b=new_node(ND_BIN); b->op='<'; b->lhs=n; b->rhs=parse_shift(); n=b; }
        else if (consume(TOK_LE)) { Node *b=new_node(ND_BIN); b->op='l'; b->lhs=n; b->rhs=parse_shift(); n=b; }
        else if (consume(TOK_GT)) { Node *b=new_node(ND_BIN); b->op='>'; b->lhs=n; b->rhs=parse_shift(); n=b; }
        else if (consume(TOK_GE)) { Node *b=new_node(ND_BIN); b->op='g'; b->lhs=n; b->rhs=parse_shift(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_eq(void) {
    Node *n = parse_rel();
    for (;;) {
        if (consume(TOK_EQ)) { Node *b=new_node(ND_BIN); b->op='='; b->lhs=n; b->rhs=parse_rel(); n=b; }
        else if (consume(TOK_NE)) { Node *b=new_node(ND_BIN); b->op='!'; b->lhs=n; b->rhs=parse_rel(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_bitand(void) {
    Node *n = parse_eq();
    for (;;) {
        if (consume(TOK_AMP)) { Node *b=new_node(ND_BIN); b->op='&'; b->lhs=n; b->rhs=parse_eq(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_bitxor(void) {
    Node *n = parse_bitand();
    for (;;) {
        if (consume(TOK_XOR)) { Node *b=new_node(ND_BIN); b->op='^'; b->lhs=n; b->rhs=parse_bitand(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_bitor(void) {
    Node *n = parse_bitxor();
    for (;;) {
        if (consume(TOK_OR)) { Node *b=new_node(ND_BIN); b->op='|'; b->lhs=n; b->rhs=parse_bitxor(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_land(void) {
    Node *n = parse_bitor();
    for (;;) {
        if (consume(TOK_LAND)) { Node *b=new_node(ND_BIN); b->op='A'; b->lhs=n; b->rhs=parse_bitor(); n=b; }
        else break;
    }
    return n;
}

static Node *parse_lor(void) {
    Node *n = parse_land();
    for (;;) {
        if (consume(TOK_LOR)) { Node *b=new_node(ND_BIN); b->op='O'; b->lhs=n; b->rhs=parse_land(); n=b; }
        else break;
    }
    return n;
}

/* Fold an integer constant expression.  Returns 1 and writes *out when the
 * whole tree is literals and operators; 0 if anything else turns up, so the
 * caller can report a proper error rather than emit a wrong size.
 *
 * `sizeof` needs no case: parse_unary already turns it into ND_NUM. */
static int eval_const(Node *n, int *out) {
    if (!n) return 0;
    if (n->kind == ND_NUM) { *out = n->ival; return 1; }
    if (n->kind == ND_CAST) return eval_const(n->lhs, out);
    if (n->kind == ND_UNARY) {
        int a;
        if (!eval_const(n->lhs, &a)) return 0;
        if (n->op == '-') { *out = -a; return 1; }
        if (n->op == '!') { *out = !a; return 1; }
        if (n->op == '~') { *out = ~a; return 1; }
        return 0;
    }
    if (n->kind == ND_TERN) {
        int c;
        if (!eval_const(n->args[0], &c)) return 0;
        return eval_const(c ? n->args[1] : n->args[2], out);
    }
    if (n->kind != ND_BIN) return 0;
    int a, b;
    if (!eval_const(n->lhs, &a)) return 0;
    /* && and || short-circuit: the right side need not be constant. */
    if (n->op == 'A' && !a) { *out = 0; return 1; }
    if (n->op == 'O' && a)  { *out = 1; return 1; }
    if (!eval_const(n->rhs, &b)) return 0;
    switch (n->op) {
    case '+': *out = a + b; return 1;
    case '-': *out = a - b; return 1;
    case '*': *out = a * b; return 1;
    case '/': if (!b) return 0; *out = a / b; return 1;
    case '%': if (!b) return 0; *out = a % b; return 1;
    case 'L': *out = a << b; return 1;
    case 'R': *out = a >> b; return 1;
    case '&': *out = a & b; return 1;
    case '|': *out = a | b; return 1;
    case '^': *out = a ^ b; return 1;
    case '<': *out = a < b; return 1;
    case 'l': *out = a <= b; return 1;
    case '>': *out = a > b; return 1;
    case 'g': *out = a >= b; return 1;
    case '=': *out = a == b; return 1;
    case '!': *out = a != b; return 1;
    case 'A': *out = a && b; return 1;
    case 'O': *out = a || b; return 1;
    default: return 0;
    }
}

static Node *parse_cond(void) {
    Node *n = parse_lor();
    if (consume(TOK_QUESTION)) {
        Node *t = parse_expr();
        expect(TOK_COLON);
        Node *f = parse_cond();
        Node *c = new_node(ND_TERN);
        c->args[0] = n;
        c->args[1] = t;
        c->args[2] = f;
        c->argc = 3;
        return c;
    }
    return n;
}

static Node *parse_assign(void) {
    Node *n = parse_cond();
    if (consume(TOK_ASSIGN)) {
        Node *a = new_node(ND_ASSIGN);
        a->lhs = n;
        a->rhs = parse_assign();
        return a;
    }
    if (tok.kind == TOK_OPASSIGN) {
        int op = tok.ival;
        next_token();
        return node_assign(n, node_bin(op, n, parse_assign()));
    }
    return n;
}

static Node *parse_expr(void) { return parse_assign(); }

static void emit_int_to_float(int disp) {
    emit8(0xDB); emit8(0x85); emit32((uint32_t)disp); /* fild dword [ebp+disp] */
    emit8(0xD9); emit8(0x9D); emit32((uint32_t)disp); /* fstp dword [ebp+disp] */
}

static void conv_stack_to_float(int disp) {
    emit8(0xDB); emit8(0x84); emit8(0x24); emit32((uint32_t)disp); /* fild dword [esp+disp] */
    emit8(0xD9); emit8(0x9C); emit8(0x24); emit32((uint32_t)disp); /* fstp dword [esp+disp] */
}

/* A C cast to int truncates, but the FPU rounds to nearest -- so switch the
 * rounding mode to chop for just this one instruction and put it back.  The
 * startup stub used to set chop once and leave it, which quietly truncated
 * every arithmetic result in the program, not only the casts. */
static void conv_stack_to_int(int disp) {
    emit8(0xD9); emit8(0x84); emit8(0x24); emit32((uint32_t)disp); /* fld dword [esp+disp] */
    emit8(0x83); emit8(0xEC); emit8(0x08);                         /* sub esp, 8 */
    emit8(0xD9); emit8(0x3C); emit8(0x24);                         /* fnstcw [esp]     (saved) */
    emit8(0xD9); emit8(0x7C); emit8(0x24); emit8(0x04);            /* fnstcw [esp+4]   (scratch) */
    emit8(0x66); emit8(0x81); emit8(0x4C); emit8(0x24); emit8(0x04);
    emit8(0x00); emit8(0x0C);                                      /* or word [esp+4], 0x0C00 */
    emit8(0xD9); emit8(0x6C); emit8(0x24); emit8(0x04);            /* fldcw [esp+4]  -> chop */
    /* esp moved by 8, so the destination slot moved with it */
    emit8(0xDB); emit8(0x9C); emit8(0x24); emit32((uint32_t)(disp + 8)); /* fistp dword [esp+disp+8] */
    emit8(0xD9); emit8(0x2C); emit8(0x24);                         /* fldcw [esp]    -> restore */
    emit8(0x83); emit8(0xC4); emit8(0x08);                         /* add esp, 8 */
}

static Type *gen_expr(Node *n);
static void gen_logical(Node *e, int label_true, int label_false);
static void gen_cond(Node *e, int label_false);

static void emit_boolify_from_int(void) {
    emit_pop_eax();
    emit8(0x83); emit8(0xF8); emit8(0x00); /* cmp eax, 0 */
    emit8(0x0F); emit8(0x95); emit8(0xC0); /* setne al */
    emit8(0x0F); emit8(0xB6); emit8(0xC0);
    emit_push_eax();
}

static void emit_boolify_from_float(void) {
    emit8(0xD9); emit8(0x04); emit8(0x24); /* fld [esp] */
    emit8(0xD9); emit8(0xE4); /* ftst */
    emit8(0x9B); emit8(0xDF); emit8(0xE0); /* fstsw ax */
    emit8(0x9E); /* sahf */
    emit_add_esp(4);
    emit8(0x0F); emit8(0x95); emit8(0xC0); /* setne al */
    emit8(0x0F); emit8(0xB6); emit8(0xC0);
    emit_push_eax();
}

static void emit_boolify(Type *t) {
    if (is_float(t)) emit_boolify_from_float();
    else emit_boolify_from_int();
}

static Type *ternary_common_type(Type *ta, Type *tb) {
    ta = decay_array(ta);
    tb = decay_array(tb);
    if (is_ptr(ta) && is_ptr(tb)) return ta;
    if (is_float(ta) || is_float(tb)) return &type_float;
    if (is_unsigned_type(ta) || is_unsigned_type(tb)) return &type_uint;
    return &type_int;
}

static void emit_lea_eax_membp(int disp) { emit8(0x8D); emit8(0x85); emit32((uint32_t)disp); }

static void emit_load_addr(Sym *s) {
    if (s->alias_global >= 0) {
        emit_mov_eax_imm(0);
        add_addr_fix(code_len - 4, AF_GLOB, s->alias_global);
    } else if (s->is_global) {
        emit_mov_eax_imm(0);
        add_addr_fix(code_len - 4, AF_GLOB, s - globals);
    } else {
        emit_lea_eax_membp(s->offset);
    }
    emit_push_eax();
}

static void emit_func_addr(const char *name) {
    emit_mov_eax_imm(0);
    add_sym_fix(code_len - 4, RELOC_ABS32, name);
    emit_push_eax();
}

static void emit_load_from_addr(Type *ty) {
    emit_pop_eax();
    if (is_char(ty)) {
        if (is_unsigned_type(ty)) { emit8(0x0F); emit8(0xB6); emit8(0x00); }
        else { emit8(0x0F); emit8(0xBE); emit8(0x00); } /* movsx eax, byte [eax] */
    } else if (is_bool(ty)) {
        emit8(0x0F); emit8(0xB6); emit8(0x00); /* movzx eax, byte [eax] */
    } else if (is_short(ty)) {
        if (is_unsigned_type(ty)) { emit8(0x0F); emit8(0xB7); emit8(0x00); } /* movzx eax, word [eax] */
        else { emit8(0x0F); emit8(0xBF); emit8(0x00); }                      /* movsx eax, word [eax] */
    } else {
        emit8(0x8B); emit8(0x00); /* mov eax, [eax] */
    }
    emit_push_eax();
}

static void emit_store_to_addr(Type *ty) {
    emit_pop_eax(); /* value */
    emit_pop_ebx(); /* addr */
    if (is_byte(ty)) {
        emit8(0x88); emit8(0x03); /* mov [ebx], al */
    } else if (is_short(ty)) {
        emit8(0x66); emit8(0x89); emit8(0x03); /* mov [ebx], ax */
    } else {
        emit8(0x89); emit8(0x03); /* mov [ebx], eax */
    }
    emit_push_eax();
}

static Field *find_field(StructDef *s, const char *name) {
    for (int i = 0; i < s->field_count; i++) if (!strcmp(s->fields[i].name, name)) return &s->fields[i];
    return 0;
}

static Type *emit_syscall_builtin(int sysno, int argc) {
    if (argc > 4) { puts("syscall args\n"); sys_exit(1); }
    if (argc >= 1) { emit8(0x5B); } /* pop ebx */
    if (argc >= 2) { emit8(0x59); } /* pop ecx */
    if (argc >= 3) { emit8(0x5A); } /* pop edx */
    if (argc >= 4) { emit8(0x5E); } /* pop esi */
    emit_mov_eax_imm((uint32_t)sysno);
    emit8(0xCD); emit8(0x80);
    emit_push_eax();
    return &type_int;
}

static Type *emit_syscall_runtime(int argc) {
    if (argc < 1 || argc > 4) { puts("syscall argc\n"); sys_exit(1); }
    emit8(0x58); /* pop eax (sysno) */
    if (argc >= 2) emit8(0x5B); /* pop ebx */
    if (argc >= 3) emit8(0x59); /* pop ecx */
    if (argc >= 4) emit8(0x5A); /* pop edx */
    emit8(0xCD); emit8(0x80);
    emit_push_eax();
    return &type_int;
}

static void gen_args_rev(Node *n) {
    for (int i = n->argc - 1; i >= 0; i--) gen_expr(n->args[i]);
}

/* Structs by value.
 *
 * The convention is that a struct-valued expression leaves its ADDRESS on the
 * stack rather than its contents; only the points that actually need the bytes
 * (assignment and argument passing) copy them.  That keeps the existing
 * stack-machine codegen intact -- every other expression form still deals in
 * 4-byte stack slots.
 *
 * Copies use rep movsb, which clobbers esi/edi/ecx.  Safe here because values
 * live on the stack between sub-expressions, never in registers. */
static void emit_struct_copy(int size) {
    /* stack: [dst][src], top is src.  Leaves [dst] as the result. */
    emit8(0x5E);                                  /* pop esi  <- src */
    emit8(0x5F);                                  /* pop edi  <- dst */
    emit8(0x57);                                  /* push edi -> result */
    emit8(0xB9); emit32((uint32_t)size);          /* mov ecx, size */
    emit8(0xFC);                                  /* cld */
    emit8(0xF3); emit8(0xA4);                     /* rep movsb */
}

/* Expand a struct argument: its address is on the stack, and the callee wants
 * the bytes themselves. */
static void emit_struct_push(int size) {
    int slot = (size + 3) & ~3;
    emit8(0x5E);                                  /* pop esi <- src */
    emit_add_esp(-slot);                          /* make room */
    emit8(0x89); emit8(0xE7);                     /* mov edi, esp */
    emit8(0xB9); emit32((uint32_t)size);          /* mov ecx, size */
    emit8(0xFC);                                  /* cld */
    emit8(0xF3); emit8(0xA4);                     /* rep movsb */
}

static int arg_slot_bytes(Type *t) {
    if (is_record(t)) return (type_size(t) + 3) & ~3;
    return 4;
}

static Type *gen_call(Node *n) {
    if (!strcmp(n->name, "syscall")) {
        if (n->argc < 1 || n->argc > 4) { puts("syscall args\n"); sys_exit(1); }
        for (int i = n->argc - 1; i >= 0; i--) gen_expr(n->args[i]);
        return emit_syscall_runtime(n->argc);
    }
    /* syscall builtins */
    if (!strcmp(n->name, "sys_write")) { if (n->argc != 3) { puts("sys_write args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(0, 3); }
    if (!strcmp(n->name, "sys_read")) { if (n->argc != 3) { puts("sys_read args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(1, 3); }
    if (!strcmp(n->name, "sys_list")) { if (n->argc != 2) { puts("sys_list args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(2, 2); }
    if (!strcmp(n->name, "sys_load")) { if (n->argc != 3) { puts("sys_load args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(3, 3); }
    if (!strcmp(n->name, "sys_save")) { if (n->argc != 3) { puts("sys_save args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(4, 3); }
    if (!strcmp(n->name, "sys_exec")) { if (n->argc != 1) { puts("sys_exec args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(5, 1); }
    if (!strcmp(n->name, "sys_exit")) { if (n->argc != 1) { puts("sys_exit args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(6, 1); }
    if (!strcmp(n->name, "sys_sbrk")) { if (n->argc != 1) { puts("sys_sbrk args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(7, 1); }
    if (!strcmp(n->name, "sys_rename")) { if (n->argc != 2) { puts("sys_rename args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(8, 2); }
    if (!strcmp(n->name, "sys_delete")) { if (n->argc != 1) { puts("sys_delete args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(9, 1); }
    if (!strcmp(n->name, "sys_getkey")) { if (n->argc != 0) { puts("sys_getkey args\n"); sys_exit(1); } return emit_syscall_builtin(10, 0); }
    if (!strcmp(n->name, "sys_getkey_nb")) { if (n->argc != 0) { puts("sys_getkey_nb args\n"); sys_exit(1); } return emit_syscall_builtin(23, 0); }
    if (!strcmp(n->name, "sys_ticks")) { if (n->argc != 0) { puts("sys_ticks args\n"); sys_exit(1); } return emit_syscall_builtin(24, 0); }
    if (!strcmp(n->name, "sys_thread_create")) { if (n->argc != 2) { puts("sys_thread_create args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(25, 2); }
    if (!strcmp(n->name, "sys_thread_exit")) { if (n->argc != 0) { puts("sys_thread_exit args\n"); sys_exit(1); } return emit_syscall_builtin(26, 0); }
    if (!strcmp(n->name, "sys_sleep")) { if (n->argc != 1) { puts("sys_sleep args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(27, 1); }
    if (!strcmp(n->name, "sys_sleepf")) { if (n->argc != 1) { puts("sys_sleepf args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(28, 1); }
    if (!strcmp(n->name, "sys_cls")) { if (n->argc != 0) { puts("sys_cls args\n"); sys_exit(1); } return emit_syscall_builtin(11, 0); }
    if (!strcmp(n->name, "sys_setcursor")) { if (n->argc != 2) { puts("sys_setcursor args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(12, 2); }
    if (!strcmp(n->name, "sys_vmode")) { if (n->argc != 1) { puts("sys_vmode args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(13, 1); }
    if (!strcmp(n->name, "sys_blit")) { if (n->argc != 1) { puts("sys_blit args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(14, 1); }
    if (!strcmp(n->name, "sys_palette")) { if (n->argc != 2) { puts("sys_palette args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(15, 2); }
    if (!strcmp(n->name, "sys_gfxinfo")) { if (n->argc != 2) { puts("sys_gfxinfo args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(21, 2); }
    if (!strcmp(n->name, "sys_gfx_fbinfo")) { if (n->argc != 2) { puts("sys_gfx_fbinfo args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(29, 2); }
    if (!strcmp(n->name, "sys_vbemodes")) { if (n->argc != 2) { puts("sys_vbemodes args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(22, 2); }
    if (!strcmp(n->name, "sys_keystate")) { if (n->argc != 1) { puts("sys_keystate args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(30, 1); }
    if (!strcmp(n->name, "sys_getcwd")) { if (n->argc != 2) { puts("sys_getcwd args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(35, 2); }
    if (!strcmp(n->name, "sys_setcwd")) { if (n->argc != 1) { puts("sys_setcwd args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(36, 1); }
    if (!strcmp(n->name, "sys_poweroff")) { if (n->argc != 0) { puts("sys_poweroff args\n"); sys_exit(1); } return emit_syscall_builtin(37, 0); }
    if (!strcmp(n->name, "sys_meminfo")) { if (n->argc != 2) { puts("sys_meminfo args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(38, 2); }
    if (!strcmp(n->name, "sys_storage")) { if (n->argc != 2) { puts("sys_storage args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(39, 2); }
    if (!strcmp(n->name, "sys_sync")) { if (n->argc != 0) { puts("sys_sync args\n"); sys_exit(1); } return emit_syscall_builtin(40, 0); }
    if (!strcmp(n->name, "sys_io_in")) { if (n->argc != 2) { puts("sys_io_in args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(41, 2); }
    if (!strcmp(n->name, "sys_io_out")) { if (n->argc != 3) { puts("sys_io_out args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(42, 3); }
    if (!strcmp(n->name, "sys_pci_read")) { if (n->argc != 2) { puts("sys_pci_read args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(43, 2); }
    if (!strcmp(n->name, "sys_pci_write")) { if (n->argc != 3) { puts("sys_pci_write args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(44, 3); }
    if (!strcmp(n->name, "sys_map_phys")) { if (n->argc != 3) { puts("sys_map_phys args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(45, 3); }
    if (!strcmp(n->name, "sys_dma_alloc")) { if (n->argc != 2) { puts("sys_dma_alloc args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(46, 2); }
    if (!strcmp(n->name, "sys_irq_wait")) { if (n->argc != 2) { puts("sys_irq_wait args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(47, 2); }
    if (!strcmp(n->name, "va_start")) {
        if (n->argc != 2) { puts("va_start args\n"); sys_exit(1); }
        if (n->args[0]->kind != ND_VAR || n->args[1]->kind != ND_VAR) { puts("va_start lvalue\n"); sys_exit(1); }
        Sym *ap = find_local(n->args[0]->name);
        if (!ap) ap = find_global(n->args[0]->name);
        Sym *last = find_local(n->args[1]->name);
        if (!last) last = find_global(n->args[1]->name);
        if (!ap || !last) { puts("va_start var\n"); sys_exit(1); }
        int step = type_size(last->type);
        if (step < 4) step = 4;
        if (step & 3) step = (step + 3) & ~3;

        emit_load_addr(ap);   /* push &ap */
        emit_load_addr(last); /* push &last */
        emit_pop_eax();       /* eax = &last */
        emit8(0x05); emit32((uint32_t)step); /* add eax, step */
        emit8(0x5B);          /* pop ebx = &ap */
        emit8(0x89); emit8(0x03); /* mov [ebx], eax */
        emit_mov_eax_imm(0);
        emit_push_eax();
        return &type_int;
    }
    if (!strcmp(n->name, "va_end")) {
        emit_mov_eax_imm(0);
        emit_push_eax();
        return &type_int;
    }

    if (n->callee) {
        int indirect = 0;
        if (n->callee->kind != ND_VAR) {
            indirect = 1;
        } else {
            Sym *sv = find_local(n->name);
            if (!sv) sv = find_global(n->name);
            if (sv) indirect = 1;
        }
        if (indirect) {
            for (int i = n->argc - 1; i >= 0; i--) gen_expr(n->args[i]);
            gen_expr(n->callee);
            emit_pop_eax();
            emit8(0xFF); emit8(0xD0); /* call eax */
            /* indirect calls have no parameter types, so every argument is a
             * plain 4-byte slot -- structs by value need a known prototype */
            if (n->argc > 0) emit_add_esp(4 * n->argc);
            emit_push_eax();
            return &type_int;
        }
    }

    Func *f = find_func(n->name);
    if (!f) {
        warn_here("call to undeclared function");
        f = add_func(n->name, &type_int);
    } else if (!f->declared && !f->defined) {
        warn_here("call to undeclared function");
    }
    if (f->declared) {
        if (f->is_varargs) {
            if (n->argc < f->param_count) { error_here("not enough arguments"); }
        } else {
            if (n->argc != f->param_count) { error_here("wrong number of arguments"); }
        }
    }
    int argbytes = 0;
    for (int i = n->argc - 1; i >= 0; i--) {
        Type *t = gen_expr(n->args[i]);
        /* Arguments past the prototype are varargs: pass them through
         * untouched.  They used to be forced to int, which silently destroyed
         * a float before printf could ever see its bits. */
        int vararg = (i >= f->param_count);
        Type *pt = vararg ? &type_int : f->params[i];
        if (is_array(pt)) { puts("param struct/array\n"); sys_exit(1); }
        if (is_record(pt)) {
            if (!is_record(t) || type_size(t) != type_size(pt)) {
                puts("struct arg type\n"); sys_exit(1);
            }
            emit_struct_push(type_size(pt));
            argbytes += arg_slot_bytes(pt);
            continue;
        }
        if (!vararg) {
            if (is_float(pt) && !is_float(t)) conv_stack_to_float(0);
            if (!is_float(pt) && is_float(t)) conv_stack_to_int(0);
        }
        argbytes += 4;
    }
    emit8(0xE8);
    int pos = code_len;
    emit32(0);
    const char *lname = f->link_name[0] ? f->link_name : n->name;
    add_sym_fix(pos, RELOC_REL32, lname);
    if (argbytes > 0) emit_add_esp(argbytes);
    if (is_float(f->ret)) {
        emit_add_esp(-4);
        emit8(0xD9); emit8(0x1C); emit8(0x24); /* fstp dword [esp] */
        return &type_float;
    }
    emit_push_eax();
    return is_void(f->ret) ? &type_int : f->ret;
}

static Type *gen_addr(Node *n) {
    switch (n->kind) {
    case ND_VAR: {
        Sym *s = find_local(n->name);
        if (!s) s = find_global(n->name);
        if (!s) {
            Func *f = find_func(n->name);
            if (!f) { error_name("unknown var", n->name); }
            const char *lname = f->link_name[0] ? f->link_name : n->name;
            emit_func_addr(lname);
            return &type_void;
        }
        emit_load_addr(s);
        return s->type;
    }
    case ND_DEREF: {
        Type *pt = gen_expr(n->lhs);
        pt = decay_array(pt);
        if (!is_ptr(pt)) { puts("deref ptr\n"); sys_exit(1); }
        return pt->base;
    }
    case ND_INDEX: {
        Type *bt = gen_expr(n->lhs);
        bt = decay_array(bt);
        if (!is_ptr(bt)) { puts("index ptr\n"); sys_exit(1); }
        Type *it = gen_expr(n->rhs);
        if (is_float(it)) conv_stack_to_int(0);
        int sz = type_size(bt->base);
        if (sz > 1) {
            emit_pop_eax();
            emit8(0x69); emit8(0xC0); emit32((uint32_t)sz); /* imul eax, eax, imm */
            emit_push_eax();
        }
        emit_pop_ebx(); /* index */
        emit_pop_eax(); /* base */
        emit8(0x01); emit8(0xD8); /* add eax, ebx */
        emit_push_eax();
        return bt->base;
    }
    case ND_MEMBER: {
        Type *bt = 0;
        if (n->op == '.') {
            bt = gen_addr(n->lhs);
        } else {
            Type *pt = gen_expr(n->lhs);
            pt = decay_array(pt);
            if (!is_ptr(pt)) { puts("arrow ptr\n"); sys_exit(1); }
            bt = pt->base;
        }
        if (!is_record(bt)) { puts("member struct\n"); sys_exit(1); }
        Field *f = find_field(bt->sdef, n->name);
        if (!f) { puts("no field: "); puts(n->name); putc('\n'); error_here("unknown member\n"); }
        emit_pop_eax();
        if (f->offset) { emit8(0x05); emit32((uint32_t)f->offset); }
        emit_push_eax();
        return f->type;
    }
    default:
        puts("lvalue\n"); sys_exit(1);
    }
    return &type_int;
}

static Type *gen_expr(Node *n) {
    if (!n) return &type_int;
    switch (n->kind) {
    case ND_NUM:
        emit8(0x68); emit32((uint32_t)n->ival);
        return &type_int;
    case ND_STR: {
        int slen = strlen(n->name);
        int roff = rodata_len;
        if (roff + slen + 1 > MAX_RODATA) { puts("rodata ovf\n"); sys_exit(1); }
        memcpy(rodata + roff, n->name, slen + 1);
        rodata_len += slen + 1;
        emit_mov_eax_imm(0);
        add_addr_fix(code_len - 4, AF_STR, roff);
        emit_push_eax();
        return type_ptr(&type_char);
    }
    case ND_FNUM: {
        union { float f; uint32_t u; } u;
        u.f = n->fval;
        emit8(0x68); emit32(u.u);
        return &type_float;
    }
    case ND_VAR: {
        Sym *s = find_local(n->name);
        if (!s) s = find_global(n->name);
        if (!s) {
            Func *f = find_func(n->name);
            if (!f) { error_name("unknown var", n->name); }
            const char *lname = f->link_name[0] ? f->link_name : n->name;
            emit_func_addr(lname);
            return type_ptr(&type_void);
        }
        if (is_array(s->type)) {
            emit_load_addr(s);
            return type_ptr(s->type->base);
        }
        if (is_record(s->type)) { emit_load_addr(s); return s->type; }
        emit_load_addr(s);
        emit_load_from_addr(s->type);
        return promote(s->type);
    }
    case ND_ADDR: {
        Type *t = gen_addr(n->lhs);
        return type_ptr(t);
    }
    case ND_DEREF: {
        Type *t = gen_addr(n);
        /* A record or array value is represented by its address on this
         * stack -- exactly what the ND_VAR case above leaves for a struct --
         * so `*p` needs no load, and `s = *p` or `f(*p)` then work. */
        if (is_record(t) || is_array(t)) return t;
        emit_load_from_addr(t);
        return promote(t);
    }
    case ND_INDEX:
    case ND_MEMBER: {
        Type *t = gen_addr(n);
        if (is_array(t)) {
            return type_ptr(t->base);
        }
        if (is_record(t)) return t;      /* address already on the stack */
        emit_load_from_addr(t);
        return promote(t);
    }
    case ND_ASSIGN: {
        Type *lt = gen_addr(n->lhs);
        if (is_record(lt)) {
            Type *rt = gen_expr(n->rhs);
            if (!is_record(rt) || type_size(rt) != type_size(lt)) {
                puts("struct assign type\n"); sys_exit(1);
            }
            emit_struct_copy(type_size(lt));
            return lt;
        }
        if (is_array(lt)) { puts("assign struct/array\n"); sys_exit(1); }
        Type *rt = gen_expr(n->rhs);
        if (is_bool(lt)) {
            emit_boolify(rt);
        } else {
            if (is_float(lt) && !is_float(rt)) conv_stack_to_float(0);
            if (!is_float(lt) && is_float(rt)) conv_stack_to_int(0);
        }
        emit_store_to_addr(lt);
        return promote(lt);
    }
    case ND_TERN: {
        if (n->argc < 3) { puts("ternary args\n"); sys_exit(1); }
        Node *cond = n->args[0];
        Node *t = n->args[1];
        Node *f = n->args[2];
        /* An array operand decays to a pointer, so `c ? "a" : "b"` is fine.
         * A record would need the result copied somewhere, which this
         * expression stack has no room for, so that stays rejected. */
        Type *tt = decay_array(infer_type(t));
        Type *tf = decay_array(infer_type(f));
        if (is_record(tt) || is_record(tf)) { puts("ternary type\n"); sys_exit(1); }
        Type *ct = ternary_common_type(tt, tf);
        int l_false = new_label();
        int l_end = new_label();
        gen_cond(cond, l_false);
        Type *rt = gen_expr(t);
        if (ct == &type_float && !is_float(rt)) conv_stack_to_float(0);
        if (ct != &type_float && is_float(rt)) conv_stack_to_int(0);
        emit_jmp_label(l_end);
        set_label(l_false);
        Type *rf = gen_expr(f);
        if (ct == &type_float && !is_float(rf)) conv_stack_to_float(0);
        if (ct != &type_float && is_float(rf)) conv_stack_to_int(0);
        set_label(l_end);
        return ct;
    }
    case ND_VAARG: {
        if (!n->lhs || n->lhs->kind != ND_VAR) { puts("va_arg lvalue\n"); sys_exit(1); }
        if (is_record(n->type) || is_array(n->type)) { puts("va_arg type\n"); sys_exit(1); }
        Sym *s = find_local(n->lhs->name);
        if (!s) s = find_global(n->lhs->name);
        if (!s) { error_name("unknown var", n->lhs->name); }
        int step = type_size(n->type);
        if (step < 4) step = 4;
        if (step & 3) step = (step + 3) & ~3;

        emit_load_addr(s);      /* push &ap */
        emit8(0x5B);            /* pop ebx */
        emit8(0x8B); emit8(0x03); /* mov eax, [ebx] (ap) */
        emit8(0x89); emit8(0xC1); /* mov ecx, eax (save ap) */
        if (is_byte(n->type)) {
            emit8(0x0F); emit8(0xB6); emit8(0x00); /* movzx eax, byte [eax] */
        } else {
            emit8(0x8B); emit8(0x00); /* mov eax, [eax] */
        }
        emit8(0x81); emit8(0xC1); emit32((uint32_t)step); /* add ecx, step */
        emit8(0x89); emit8(0x0B); /* mov [ebx], ecx */
        emit_push_eax();
        return n->type;
    }
    case ND_CALL:
        return gen_call(n);
    case ND_CAST: {
        Type *t = gen_expr(n->lhs);
        if (is_bool(n->type)) {
            emit_boolify(t);
        } else {
            if (is_float(n->type) && !is_float(t)) conv_stack_to_float(0);
            if (!is_float(n->type) && is_float(t)) conv_stack_to_int(0);
            /* A cast to a narrower integer has to truncate: values live in eax
             * as 32 bits, so without this (uint8_t)300 would stay 300. */
            if (is_short(n->type)) {
                emit_pop_eax();
                if (is_unsigned_type(n->type)) { emit8(0x0F); emit8(0xB7); emit8(0xC0); } /* movzx eax, ax */
                else { emit8(0x0F); emit8(0xBF); emit8(0xC0); }                           /* movsx eax, ax */
                emit_push_eax();
            } else if (is_char(n->type)) {
                emit_pop_eax();
                if (is_unsigned_type(n->type)) { emit8(0x0F); emit8(0xB6); emit8(0xC0); } /* movzx eax, al */
                else { emit8(0x0F); emit8(0xBE); emit8(0xC0); }                           /* movsx eax, al */
                emit_push_eax();
            }
        }
        return n->type;
    }
    case ND_UNARY: {
        Type *t = gen_expr(n->lhs);
        if (n->op == '-') {
            if (!is_float(t)) {
                emit_pop_eax(); emit8(0xF7); emit8(0xD8); emit_push_eax();
                return is_unsigned_type(t) ? &type_uint : &type_int;
            } else {
                emit8(0xD9); emit8(0x04); emit8(0x24); /* fld dword [esp] */
                emit8(0xD9); emit8(0xE0); /* fchs */
                emit8(0xD9); emit8(0x1C); emit8(0x24); /* fstp dword [esp] */
                return &type_float;
            }
        }
        if (n->op == '!') {
            if (is_float(t)) {
                emit8(0xD9); emit8(0x04); emit8(0x24); /* fld [esp] */
                emit8(0xD9); emit8(0xE4); /* ftst */
                emit8(0x9B); emit8(0xDF); emit8(0xE0); /* fstsw ax */
                emit8(0x9E); /* sahf */
                emit_add_esp(4);
                emit8(0x0F); emit8(0x94); emit8(0xC0); /* sete al */
                emit8(0x0F); emit8(0xB6); emit8(0xC0);
                emit_push_eax();
                return &type_int;
            }
            emit_pop_eax();
            emit8(0x83); emit8(0xF8); emit8(0x00); /* cmp eax, 0 */
            emit8(0x0F); emit8(0x94); emit8(0xC0); /* sete al */
            emit8(0x0F); emit8(0xB6); emit8(0xC0);
            emit_push_eax();
            return &type_int;
        }
        if (n->op == '~') {
            if (is_float(t)) { puts("bitnot float\n"); sys_exit(1); }
            emit_pop_eax(); emit8(0xF7); emit8(0xD0); emit_push_eax(); /* not eax */
            return is_unsigned_type(t) ? &type_uint : &type_int;
        }
        return t;
    }
    case ND_BIN: {
        /* && and || must come before the operand emission below:
         * gen_logical generates both operands itself (with short-circuit
         * jumps), so emitting them here too left two extra values on the
         * stack -- any `x = a && b` then stored through a garbage address. */
        if (n->op=='A' || n->op=='O') {
            int l_true = new_label();
            int l_false = new_label();
            gen_logical(n, l_true, l_false);
            set_label(l_true);
            emit8(0x68); emit32(1);
            int l_end = new_label();
            emit_jmp_label(l_end);
            set_label(l_false);
            emit8(0x68); emit32(0);
            set_label(l_end);
            return &type_int;
        }
        Type *lt = gen_expr(n->lhs);
        Type *rt = gen_expr(n->rhs);
        lt = promote(decay_array(lt));
        rt = promote(decay_array(rt));
        if ((n->op=='/' || n->op=='%') && n->rhs && n->rhs->kind == ND_NUM && n->rhs->ival == 0) {
            error_here("division by zero");
        }
        if ((n->op=='L' || n->op=='R') && n->rhs && n->rhs->kind == ND_NUM) {
            int sh = n->rhs->ival;
            if (sh < 0 || sh >= 32) warn_here("shift count out of range");
        }
        int is_cmp = (n->op=='<'||n->op=='>'||n->op=='l'||n->op=='g'||n->op=='='||n->op=='!');
        int is_u = is_unsigned_type(lt) || is_unsigned_type(rt);
        if (is_ptr(lt) || is_ptr(rt)) is_u = 1;

        if (n->op=='&' || n->op=='|' || n->op=='^' || n->op=='L' || n->op=='R' || n->op=='%') {
            if (is_float(lt) || is_float(rt) || is_ptr(lt) || is_ptr(rt)) { puts("bit op type\n"); sys_exit(1); }
            emit_pop_ebx(); emit_pop_eax();
            if (n->op=='&') { emit8(0x21); emit8(0xD8); }
            else if (n->op=='|') { emit8(0x09); emit8(0xD8); }
            else if (n->op=='^') { emit8(0x31); emit8(0xD8); }
            else if (n->op=='L') {
                emit8(0x89); emit8(0xD9); /* mov ecx, ebx */
                emit8(0xD3); emit8(0xE0); /* shl eax, cl */
            } else if (n->op=='R') {
                emit8(0x89); emit8(0xD9); /* mov ecx, ebx */
                if (is_u) { emit8(0xD3); emit8(0xE8); } /* shr eax, cl */
                else { emit8(0xD3); emit8(0xF8); }      /* sar eax, cl */
            } else if (n->op=='%') {
                if (is_u) {
                    emit8(0x31); emit8(0xD2); /* xor edx, edx */
                    emit8(0xF7); emit8(0xF3); /* div ebx */
                } else {
                    emit8(0x99); /* cdq */
                    emit8(0xF7); emit8(0xFB); /* idiv ebx */
                }
                emit8(0x89); emit8(0xD0); /* mov eax, edx */
            }
            emit_push_eax();
            return is_u ? &type_uint : &type_int;
        }
        if (is_cmp) {
            if (is_float(lt) || is_float(rt)) {
                if (is_ptr(lt) || is_ptr(rt)) { puts("ptr cmp float\n"); sys_exit(1); }
                if (!is_float(rt)) conv_stack_to_float(0);
                if (!is_float(lt)) conv_stack_to_float(4);
                /* fcompp compares st(0) against st(1), so the left operand
                 * must be loaded LAST.  Loading it first made every float
                 * <, >, <= and >= evaluate reversed; == and != survived only
                 * because equality is symmetric. */
                emit8(0xD9); emit8(0x04); emit8(0x24);                  /* fld [esp]   -> rhs */
                emit8(0xD9); emit8(0x44); emit8(0x24); emit8(0x04);     /* fld [esp+4] -> lhs */
                emit8(0xDE); emit8(0xD9); /* fcompp: lhs ? rhs */
                emit8(0x9B); emit8(0xDF); emit8(0xE0); /* fstsw ax */
                emit8(0x9E); /* sahf */
                emit8(0x0F);
                switch (n->op) {
                case '<': emit8(0x92); break; /* setb */
                case 'l': emit8(0x96); break; /* setbe */
                case '>': emit8(0x97); break; /* seta */
                case 'g': emit8(0x93); break; /* setae */
                case '=': emit8(0x94); break; /* sete */
                case '!': emit8(0x95); break; /* setne */
                }
                emit8(0xC0);
                emit8(0x0F); emit8(0xB6); emit8(0xC0); /* movzx eax, al */
                emit_add_esp(8);
                emit_push_eax();
                return &type_int;
            } else {
                emit_pop_ebx(); emit_pop_eax();
                emit8(0x39); emit8(0xD8); /* cmp eax, ebx */
                emit8(0x0F);
                if (is_u) {
                    switch (n->op) {
                    case '<': emit8(0x92); break; /* setb */
                    case 'l': emit8(0x96); break; /* setbe */
                    case '>': emit8(0x97); break; /* seta */
                    case 'g': emit8(0x93); break; /* setae */
                    case '=': emit8(0x94); break; /* sete */
                    case '!': emit8(0x95); break; /* setne */
                    }
                } else {
                    switch (n->op) {
                    case '<': emit8(0x9C); break; /* setl */
                    case 'l': emit8(0x9E); break; /* setle */
                    case '>': emit8(0x9F); break; /* setg */
                    case 'g': emit8(0x9D); break; /* setge */
                    case '=': emit8(0x94); break; /* sete */
                    case '!': emit8(0x95); break; /* setne */
                    }
                }
                emit8(0xC0);
                emit8(0x0F); emit8(0xB6); emit8(0xC0);
                emit_push_eax();
                return &type_int;
            }
        }

        if ((n->op == '+' || n->op == '-') && is_ptr(lt) && is_numeric(rt) && !is_float(rt)) {
            int sz = type_size(lt->base);
            if (sz > 1) {
                emit_pop_eax();
                emit8(0x69); emit8(0xC0); emit32((uint32_t)sz);
                emit_push_eax();
            }
            emit_pop_ebx(); /* scaled int */
            emit_pop_eax(); /* ptr */
            if (n->op == '+') { emit8(0x01); emit8(0xD8); }
            else { emit8(0x29); emit8(0xD8); }
            emit_push_eax();
            return lt;
        }
        if (n->op == '+' && is_ptr(rt) && is_numeric(lt) && !is_float(lt)) {
            int sz = type_size(rt->base);
            emit_pop_ebx(); /* ptr */
            emit_pop_eax(); /* int */
            if (sz > 1) {
                emit8(0x69); emit8(0xC0); emit32((uint32_t)sz);
            }
            emit8(0x01); emit8(0xD8); /* add eax, ebx */
            emit_push_eax();
            return rt;
        }
        if (n->op == '-' && is_ptr(lt) && is_ptr(rt)) {
            int sz = type_size(lt->base);
            emit_pop_ebx(); emit_pop_eax();
            emit8(0x29); emit8(0xD8); /* sub eax, ebx */
            if (sz > 1) {
                emit_mov_ecx_imm((uint32_t)sz);
                emit8(0x99); /* cdq */
                emit8(0xF7); emit8(0xF9); /* idiv ecx */
            }
            emit_push_eax();
            return &type_int;
        }

        if (is_float(lt) || is_float(rt)) {
            if (is_ptr(lt) || is_ptr(rt)) { puts("ptr arith\n"); sys_exit(1); }
            if (!is_float(rt)) conv_stack_to_float(0);
            if (!is_float(lt)) conv_stack_to_float(4);
            emit8(0xD9); emit8(0x44); emit8(0x24); emit8(0x04); /* fld [esp+4] */
            emit8(0xD9); emit8(0x04); emit8(0x24); /* fld [esp] */
            if (n->op=='+') { emit8(0xDE); emit8(0xC1); }
            else if (n->op=='-') { emit8(0xDE); emit8(0xE9); }
            else if (n->op=='*') { emit8(0xDE); emit8(0xC9); }
            else if (n->op=='/') { emit8(0xDE); emit8(0xF9); }
            emit8(0xD9); emit8(0x5C); emit8(0x24); emit8(0x04); /* fstp [esp+4] */
            emit_add_esp(4);
            return &type_float;
        } else {
            emit_pop_ebx(); emit_pop_eax();
            if (n->op=='+') { emit8(0x01); emit8(0xD8); }
            else if (n->op=='-') { emit8(0x29); emit8(0xD8); }
            else if (n->op=='*') { emit8(0x0F); emit8(0xAF); emit8(0xC3); }
            else if (n->op=='/') {
                if (is_u) { emit8(0x31); emit8(0xD2); emit8(0xF7); emit8(0xF3); }
                else { emit8(0x99); emit8(0xF7); emit8(0xFB); }
            }
            emit_push_eax();
            return is_u ? &type_uint : &type_int;
        }
    }
    }
    return &type_int;
}

static Type *current_ret;
static int current_epilogue_label = -1;

static void gen_cond(Node *e, int label_false) {
    if (e && e->kind == ND_BIN && (e->op == 'A' || e->op == 'O')) {
        int l_true = new_label();
        gen_logical(e, l_true, label_false);
        set_label(l_true);
        return;
    }
    Type *t = gen_expr(e);
    if (is_float(t)) {
        emit8(0xD9); emit8(0x04); emit8(0x24); /* fld [esp] */
        emit8(0xD9); emit8(0xE4); /* ftst */
        emit8(0x9B); emit8(0xDF); emit8(0xE0); /* fstsw ax */
        emit8(0x9E); /* sahf */
        emit_add_esp(4);
        emit_jcc_label(0x84, label_false); /* je */
    } else {
        emit_pop_eax();
        emit8(0x83); emit8(0xF8); emit8(0x00); /* cmp eax, 0 */
        emit_jcc_label(0x84, label_false);
    }
}

static void gen_stmt(void);

typedef struct {
    int is_typedef;
    int is_extern;
    int is_static;
} DeclSpec;

static void parse_storage_spec(DeclSpec *ds) {
    memset(ds, 0, sizeof(*ds));
    for (;;) {
        if (consume(TOK_TYPEDEF)) { ds->is_typedef = 1; continue; }
        if (consume(TOK_EXTERN)) { ds->is_extern = 1; continue; }
        if (consume(TOK_STATIC)) { ds->is_static = 1; continue; }
        if (consume(TOK_REGISTER)) { continue; }
        if (consume(TOK_AUTO)) { continue; }
        if (consume(TOK_CONST) || consume(TOK_VOLATILE)) { continue; }
        break;
    }
}

/* Copy a string literal into a stack array with immediate stores; the array
 * is zero-filled past the text, as C requires. */
static void emit_char_array_init(Sym *s, Type *ty, const char *text) {
    int slen = strlen(text);
    int total = type_size(ty);
    emit_load_addr(s);
    emit_pop_eax();               /* eax = &array */
    for (int i = 0; i < total; i++) {
        int b = (i < slen) ? (unsigned char)text[i] : 0;
        emit8(0xC6); emit8(0x80); emit32((uint32_t)i); emit8((uint8_t)b); /* mov byte [eax+i], b */
    }
}

/* ---- braced initialisers ---------------------------------------------
 *
 * The list is parsed once into (offset, type, expression) items, which lets
 * globals and locals share the walk: a global writes the constants straight
 * into .data, while a local emits a store per item.  Parsing before the
 * storage is reserved is also what makes "int a[] = {1,2,3}" work, since the
 * length is only known once the list has been counted.
 *
 * Nested aggregates need explicit braces -- brace elision, as in
 * "int m[2][2] = {1,2,3,4}", is not supported.
 */
#define MAX_INIT_ITEMS 1024
typedef struct { int off; Type *ty; Node *e; } InitItem;
static InitItem init_items[MAX_INIT_ITEMS];
static int init_item_len;
static int init_span;        /* bytes covered, for "[]" length inference */

static void parse_braced(Type *ty, int base);

static void note_span(int end) { if (end > init_span) init_span = end; }

static void parse_init_item(Type *ty, int off) {
    if (tok.kind == TOK_LBRACE) { parse_braced(ty, off); return; }
    Node *e = parse_assign();          /* not parse_expr: ',' separates items */
    if (init_item_len >= MAX_INIT_ITEMS) { puts("init ovf\n"); sys_exit(1); }
    init_items[init_item_len].off = off;
    init_items[init_item_len].ty = ty;
    init_items[init_item_len].e = e;
    init_item_len++;
    note_span(off + type_size(ty));
}

static void parse_braced(Type *ty, int base) {
    expect(TOK_LBRACE);
    if (consume(TOK_RBRACE)) { note_span(base + type_size(ty)); return; }

    if (is_array(ty)) {
        Type *et = ty->base;
        int esz = type_size(et);
        int i = 0;
        for (;;) {
            parse_init_item(et, base + i * esz);
            i++;
            if (consume(TOK_COMMA)) { if (tok.kind == TOK_RBRACE) break; continue; }
            break;
        }
        expect(TOK_RBRACE);
        if (ty->array_len > 0) note_span(base + type_size(ty));
        return;
    }

    if (is_record(ty)) {
        StructDef *sd = ty->sdef;
        int i = 0;
        for (;;) {
            if (i >= sd->field_count) { puts("too many init\n"); sys_exit(1); }
            Field *f = &sd->fields[i];
            parse_init_item(f->type, base + f->offset);
            i++;
            if (consume(TOK_COMMA)) { if (tok.kind == TOK_RBRACE) break; continue; }
            break;
        }
        expect(TOK_RBRACE);
        note_span(base + type_size(ty));
        return;
    }

    /* a scalar wrapped in braces: int x = { 5 } */
    parse_init_item(ty, base);
    expect(TOK_RBRACE);
}

/* Parse a whole initialiser and, for "[]", return the type with its length
 * filled in from what the list covered. */
static Type *parse_init_for(Type *ty) {
    init_item_len = 0;
    init_span = 0;
    parse_braced(ty, 0);
    if (is_array(ty) && ty->array_len == 0) {
        int esz = type_size(ty->base);
        if (esz <= 0) { puts("init elem size\n"); sys_exit(1); }
        ty = type_array(ty->base, (init_span + esz - 1) / esz);
    }
    return ty;
}

/* Zero a stack slot: values are written over the top afterwards, so partial
 * initialisers leave the remainder zeroed the way C requires. */
static void emit_zero_slot(Sym *s, int size) {
    if (size <= 0) return;
    emit_load_addr(s);
    emit_pop_eax();                                 /* eax = &slot */
    int words = size / 4;
    if (words > 0) {
        emit8(0xB9); emit32((uint32_t)words);       /* mov ecx, words */
        emit8(0x31); emit8(0xD2);                   /* xor edx, edx */
        int top = code_len;
        emit8(0x89); emit8(0x10);                   /* mov [eax], edx */
        emit8(0x83); emit8(0xC0); emit8(0x04);      /* add eax, 4 */
        emit8(0x49);                                /* dec ecx */
        emit8(0x75);                                /* jnz top */
        int dpos = code_len;
        emit8((uint8_t)(top - (dpos + 1)));
    }
    for (int i = 0; i < size % 4; i++) {            /* trailing bytes */
        emit8(0xC6); emit8(0x00); emit8(0x00);      /* mov byte [eax], 0 */
        emit8(0x40);                                /* inc eax */
    }
}

/* Emit the recorded items into a local. */
static void emit_local_init_items(Sym *s) {
    for (int i = 0; i < init_item_len; i++) {
        InitItem *it = &init_items[i];
        emit_load_addr(s);
        if (it->off) {
            emit_pop_eax();
            emit8(0x05); emit32((uint32_t)it->off);  /* add eax, off */
            emit_push_eax();
        }
        Type *rt = gen_expr(it->e);
        if (is_bool(it->ty)) {
            emit_boolify(rt);
        } else {
            if (is_float(it->ty) && !is_float(rt)) conv_stack_to_float(0);
            if (!is_float(it->ty) && is_float(rt)) conv_stack_to_int(0);
        }
        emit_store_to_addr(it->ty);
        emit_add_esp(4);
    }
}

/* Named labels for goto.  Labels may be used before they are defined, so an
 * entry is created on first mention and the existing label machinery resolves
 * the forward jump. */
#define MAX_GOTO_LABELS 64
typedef struct { char name[32]; int label; int defined; int used; } GotoLabel;
static GotoLabel goto_labels[MAX_GOTO_LABELS];
static int goto_label_len;

static GotoLabel *goto_label_find(const char *name) {
    for (int i = 0; i < goto_label_len; i++)
        if (!strcmp(goto_labels[i].name, name)) return &goto_labels[i];
    if (goto_label_len >= MAX_GOTO_LABELS) { puts("label ovf\n"); sys_exit(1); }
    GotoLabel *g = &goto_labels[goto_label_len++];
    strncpy(g->name, name, 31);
    g->name[31] = 0;
    g->label = new_label();
    g->defined = 0;
    g->used = 0;
    return g;
}

static void goto_labels_reset(void) { goto_label_len = 0; }

static void goto_labels_check(void) {
    for (int i = 0; i < goto_label_len; i++)
        if (goto_labels[i].used && !goto_labels[i].defined) {
            puts("undefined label: ");
            puts(goto_labels[i].name);
            putc('\n');
            sys_exit(1);
        }
}

static void parse_decl_stmt(int expect_semi) {
    DeclSpec ds;
    parse_storage_spec(&ds);
    Type *base = parse_type_spec();
    if (!base) { error_here("type?\n"); }
    if (tok.kind == TOK_SEMI) {
        if (expect_semi) next_token();
        return;
    }
    for (;;) {
        char name[32];
        Type *ty = parse_declarator(base, name);
        if (ds.is_typedef) {
            add_typedef(name, ty);
        } else if (ds.is_extern) {
            add_global_decl(name, ty);
        } else if (ds.is_static && in_func) {
            char gname[64];
            int id = static_local_id++;
            int gi = 0;
            gname[gi++] = '_'; gname[gi++] = '_'; gname[gi++] = 's'; gname[gi++] = 't'; gname[gi++] = '_';
            for (int i = 0; current_func_name[i] && gi < 48; i++) gname[gi++] = current_func_name[i];
            gname[gi++] = '_';
            for (int i = 0; name[i] && gi < 58; i++) gname[gi++] = name[i];
            gname[gi++] = '_';
            gname[gi++] = (char)('0' + (id % 10));
            gname[gi] = 0;
            Sym *gs = 0;
            if (consume(TOK_ASSIGN)) {
                gs = add_global_data(gname, ty);
                if (is_array(ty)) {
                    if (!is_char(ty->base) || tok.kind != TOK_STR) { puts("array init\n"); sys_exit(1); }
                    int slen = strlen(tok.text);
                    if (slen > type_size(ty)) slen = type_size(ty);
                    memcpy(data_seg + gs->offset, tok.text, slen);
                    next_token();
                } else {
                    Node *e = parse_expr();
                    if (e->kind == ND_NUM) {
                        int v = e->ival;
                        data_seg[gs->offset+0] = (uint8_t)(v & 0xFF);
                        data_seg[gs->offset+1] = (uint8_t)((v>>8) & 0xFF);
                        data_seg[gs->offset+2] = (uint8_t)((v>>16) & 0xFF);
                        data_seg[gs->offset+3] = (uint8_t)((v>>24) & 0xFF);
                    } else if (e->kind == ND_FNUM) {
                        union { float f; uint32_t u; } u; u.f = e->fval;
                        data_seg[gs->offset+0] = (uint8_t)(u.u & 0xFF);
                        data_seg[gs->offset+1] = (uint8_t)((u.u>>8) & 0xFF);
                        data_seg[gs->offset+2] = (uint8_t)((u.u>>16) & 0xFF);
                        data_seg[gs->offset+3] = (uint8_t)((u.u>>24) & 0xFF);
                    } else {
                        puts("global init\n"); sys_exit(1);
                    }
                }
            } else {
                gs = add_global_bss(gname, ty);
            }
            int gidx = (int)(gs - globals);
            add_local_alias(name, ty, gidx);
        } else {
            /* Consume '=' first so the initialiser can decide the array's
             * length before the stack slot is reserved -- no lookahead, which
             * would have to save and restore all of the lexer's state. */
            int has_init = (tok.kind == TOK_ASSIGN);
            if (has_init) next_token();

            if (has_init && is_array(ty) && is_char(ty->base) && tok.kind == TOK_STR) {
                if (ty->array_len == 0)                      /* char buf[] = "..." */
                    ty = type_array(ty->base, strlen(tok.text) + 1);
                Sym *sa = add_local(name, ty);
                emit_char_array_init(sa, ty, tok.text);
                next_token();
                if (consume(TOK_COMMA)) continue;
                if (expect_semi) expect(TOK_SEMI);
                break;
            }

            if (has_init && tok.kind == TOK_LBRACE) {
                ty = parse_init_for(ty);
                Sym *sb = add_local(name, ty);
                emit_zero_slot(sb, type_size(ty));
                emit_local_init_items(sb);
                if (consume(TOK_COMMA)) continue;
                if (expect_semi) expect(TOK_SEMI);
                break;
            }

            Sym *s = add_local(name, ty);
            if (has_init) {
                if (is_array(ty)) { puts("init array\n"); sys_exit(1); }
                Node *e = parse_expr();
                emit_load_addr(s);
                Type *t = gen_expr(e);
                if (is_record(ty)) {
                    /* `Token save = tok;` -- the same copy `save = tok;` does.
                     * Initialising a struct from an expression was rejected
                     * even though assigning one was already supported. */
                    if (!is_record(t) || type_size(t) != type_size(ty)) {
                        puts("struct init type\n"); sys_exit(1);
                    }
                    emit_struct_copy(type_size(ty));
                } else {
                    if (is_bool(ty)) {
                        emit_boolify(t);
                    } else {
                        if (is_float(ty) && !is_float(t)) conv_stack_to_float(0);
                        if (!is_float(ty) && is_float(t)) conv_stack_to_int(0);
                    }
                    emit_store_to_addr(ty);
                }
                emit_add_esp(4);
            }
            if (consume(TOK_COMMA)) continue;
            if (expect_semi) expect(TOK_SEMI);
            break;
        }
        if (consume(TOK_COMMA)) continue;
        if (expect_semi) expect(TOK_SEMI);
        break;
    }
}

static void gen_logical(Node *e, int label_true, int label_false) {
    if (!e) { emit_jmp_label(label_false); return; }
    if (e->kind == ND_BIN && e->op == 'A') {
        int mid = new_label();
        gen_logical(e->lhs, mid, label_false);
        set_label(mid);
        gen_logical(e->rhs, label_true, label_false);
        return;
    }
    if (e->kind == ND_BIN && e->op == 'O') {
        int mid = new_label();
        gen_logical(e->lhs, label_true, mid);
        set_label(mid);
        gen_logical(e->rhs, label_true, label_false);
        return;
    }
    Type *t = gen_expr(e);
    if (is_float(t)) {
        emit8(0xD9); emit8(0x04); emit8(0x24); /* fld [esp] */
        emit8(0xD9); emit8(0xE4); /* ftst */
        emit8(0x9B); emit8(0xDF); emit8(0xE0); /* fstsw ax */
        emit8(0x9E); /* sahf */
        emit_add_esp(4);
        emit_jcc_label(0x84, label_false); /* je */
        emit_jmp_label(label_true);
    } else {
        emit_pop_eax();
        emit8(0x83); emit8(0xF8); emit8(0x00); /* cmp eax, 0 */
        emit_jcc_label(0x84, label_false);
        emit_jmp_label(label_true);
    }
}

static void gen_return(Node *e) {
    func_has_return = 1;
    if (!e) {
        if (current_ret == &type_int) emit_mov_eax_imm(0);
        emit_jmp_label(current_epilogue_label);
        return;
    }
    Type *t = gen_expr(e);
    if (is_float(current_ret)) {
        if (!is_float(t)) conv_stack_to_float(0);
        emit8(0xD9); emit8(0x04); emit8(0x24); /* fld [esp] */
        emit_add_esp(4);
    } else {
        if (is_bool(current_ret)) {
            emit_boolify(t);
        } else if (is_float(t)) {
            conv_stack_to_int(0);
        }
        emit_pop_eax();
    }
    emit_jmp_label(current_epilogue_label);
}

static void gen_stmt(void) {
    if (consume(TOK_LBRACE)) {
        /* Names declared in this block go out of scope at its end.  Only the
         * count is restored, not local_offset, so the slots are never reused
         * and the frame size (-local_offset at the end of the function) stays
         * right -- reusing them would need liveness analysis this compiler
         * does not do. */
        int saved = locals_len;
        while (!consume(TOK_RBRACE)) gen_stmt();
        locals_len = saved;
        return;
    }
    if (consume(TOK_RETURN)) {
        if (consume(TOK_SEMI)) { gen_return(0); return; }
        Node *e = parse_expr(); expect(TOK_SEMI); gen_return(e); return;
    }
    if (consume(TOK_BREAK)) {
        if (break_depth == 0) { puts("break?\n"); sys_exit(1); }
        emit_jmp_label(break_stack[break_depth - 1].break_label);
        expect(TOK_SEMI);
        return;
    }
    if (consume(TOK_CONTINUE)) {
        for (int i = break_depth - 1; i >= 0; i--) {
            if (break_stack[i].continue_label >= 0) {
                emit_jmp_label(break_stack[i].continue_label);
                expect(TOK_SEMI);
                return;
            }
        }
        puts("continue?\n"); sys_exit(1);
    }
    if (consume(TOK_IF)) {
        expect(TOK_LPAREN);
        Node *cond = parse_expr();
        expect(TOK_RPAREN);
        int l_false = new_label();
        int l_end = new_label();
        gen_cond(cond, l_false);
        gen_stmt();
        emit_jmp_label(l_end);
        set_label(l_false);
        if (consume(TOK_ELSE)) gen_stmt();
        set_label(l_end);
        return;
    }
    if (consume(TOK_DO)) {
        int l_top = new_label();
        int l_cont = new_label();   /* continue lands on the condition test */
        int l_end = new_label();
        set_label(l_top);
        break_stack[break_depth].break_label = l_end;
        break_stack[break_depth].continue_label = l_cont;
        break_depth++;
        gen_stmt();
        break_depth--;
        set_label(l_cont);
        expect(TOK_WHILE);
        expect(TOK_LPAREN);
        int cond_pos = tok.pos;
        Node *cond = parse_expr();
        int cond_end = tok.pos;
        expect(TOK_RPAREN);
        expect(TOK_SEMI);
        int l_exit = new_label();
        diag_ovr_pos = cond_pos; diag_ovr_end = cond_end;
        gen_cond(cond, l_exit);     /* falls through when true */
        diag_ovr_end = 0;
        emit_jmp_label(l_top);
        set_label(l_exit);
        set_label(l_end);
        return;
    }
    if (consume(TOK_GOTO)) {
        if (tok.kind != TOK_ID) { error_here("goto label\n"); }
        GotoLabel *g = goto_label_find(tok.text);
        g->used = 1;
        next_token();
        expect(TOK_SEMI);
        emit_jmp_label(g->label);
        return;
    }
    /* "name:" is a label.  A statement can only start with an identifier
     * followed by ':' in that one case, so one token of lookahead settles it. */
    if (tok.kind == TOK_ID) {
        int save_pos = pos;
        Token save_tok = tok;
        char lname[32];
        strncpy(lname, tok.text, 31);
        lname[31] = 0;
        next_token();
        if (tok.kind == TOK_COLON) {
            next_token();
            GotoLabel *g = goto_label_find(lname);
            if (g->defined) { puts("duplicate label\n"); sys_exit(1); }
            g->defined = 1;
            set_label(g->label);
            gen_stmt();
            return;
        }
        pos = save_pos;
        tok = save_tok;
    }
    if (consume(TOK_WHILE)) {
        int l_start = new_label();
        int l_end = new_label();
        set_label(l_start);
        expect(TOK_LPAREN);
        Node *cond = parse_expr();
        expect(TOK_RPAREN);
        break_stack[break_depth].break_label = l_end;
        break_stack[break_depth].continue_label = l_start;
        break_depth++;
        gen_cond(cond, l_end);
        gen_stmt();
        break_depth--;
        emit_jmp_label(l_start);
        set_label(l_end);
        return;
    }
    if (consume(TOK_FOR)) {
        /* A declaration in the init clause belongs to the loop, not to the
         * enclosing function. */
        int for_scope = locals_len;
        expect(TOK_LPAREN);
        if (dbg_for) debug_for_header();
        if (!consume(TOK_SEMI)) {
            if (is_type_start()) {
                parse_decl_stmt(1);
            } else {
                Node *init = parse_expr();
                expect(TOK_SEMI);
                gen_expr(init);
                emit_add_esp(4);
            }
        }
        if (dbg_for) { puts("for-after-init: "); print_tok_desc(); putc('\n'); }
        Node *cond = 0;
        int cond_pos = 0;
        int cond_end = 0;
        if (!consume(TOK_SEMI)) { cond_pos = tok.pos; cond = parse_expr(); cond_end = tok.pos; expect(TOK_SEMI); }
        if (dbg_for) { puts("for-after-cond: "); print_tok_desc(); putc('\n'); }
        Node *post = 0;
        int post_pos = 0;
        int post_end = 0;
        if (!consume(TOK_RPAREN)) { post_pos = tok.pos; post = parse_expr(); post_end = tok.pos; expect(TOK_RPAREN); }

        int l_start = new_label();
        int l_end = new_label();
        int l_post = new_label();
        set_label(l_start);
        if (cond) {
            diag_ovr_pos = cond_pos; diag_ovr_end = cond_end;
            gen_cond(cond, l_end);
            diag_ovr_end = 0;
        }
        break_stack[break_depth].break_label = l_end;
        break_stack[break_depth].continue_label = l_post;
        break_depth++;
        gen_stmt();
        break_depth--;
        set_label(l_post);
        if (post) {
            diag_ovr_pos = post_pos; diag_ovr_end = post_end;
            gen_expr(post);
            diag_ovr_end = 0;
            emit_add_esp(4);
        }
        emit_jmp_label(l_start);
        set_label(l_end);
        locals_len = for_scope;
        return;
    }
    if (consume(TOK_SWITCH)) {
        expect(TOK_LPAREN);
        Node *cond = parse_expr();
        expect(TOK_RPAREN);
        int temp_off = local_offset - 4;
        local_offset -= 4;
        local_offset &= ~3;
        temp_off = local_offset;
        Type *ct = gen_expr(cond);
        if (is_float(ct)) conv_stack_to_int(0);
        emit_pop_eax();
        emit_mov_membp_eax(temp_off);
        int dispatch = new_label();
        int end = new_label();
        break_stack[break_depth].break_label = end;
        break_stack[break_depth].continue_label = -1;
        break_depth++;
        if (switch_depth >= 8) { puts("switch nest\n"); sys_exit(1); }
        SwitchCtx *sw = &switch_stack[switch_depth++];
        sw->end_label = end;
        sw->dispatch_label = dispatch;
        sw->default_label = -1;
        sw->temp_offset = temp_off;
        sw->case_count = 0;
        emit_jmp_label(dispatch);
        expect(TOK_LBRACE);
        while (!consume(TOK_RBRACE)) {
            if (consume(TOK_CASE)) {
                /* Any integer constant expression, so `case ND_UNARY:` and
                 * `case BASE + 1:` work, not just literals. */
                int val = 0;
                if (!eval_const(parse_cond(), &val)) {
                    error_here("case label not constant\n");
                }
                expect(TOK_COLON);
                if (sw->case_count >= MAX_CASE) { puts("case ovf\n"); sys_exit(1); }
                int lbl = new_label();
                set_label(lbl);
                sw->case_values[sw->case_count] = val;
                sw->case_labels[sw->case_count] = lbl;
                sw->case_count++;
                continue;
            }
            if (consume(TOK_DEFAULT)) {
                expect(TOK_COLON);
                int lbl = new_label();
                set_label(lbl);
                sw->default_label = lbl;
                continue;
            }
            gen_stmt();
        }
        emit_jmp_label(end);
        set_label(dispatch);
        for (int i = 0; i < sw->case_count; i++) {
            emit_mov_eax_membp(sw->temp_offset);
            emit8(0x3D); emit32((uint32_t)sw->case_values[i]); /* cmp eax, imm */
            emit_jcc_label(0x84, sw->case_labels[i]); /* je */
        }
        if (sw->default_label >= 0) emit_jmp_label(sw->default_label);
        else emit_jmp_label(sw->end_label);
        set_label(sw->end_label);
        switch_depth--;
        break_depth--;
        return;
    }

    if (is_type_start()) { parse_decl_stmt(1); return; }

    Node *e = parse_expr();
    expect(TOK_SEMI);
    gen_expr(e);
    emit_add_esp(4);
}

static void parse_function(Type *ret, const char *name) {
    /* deprecated */
}

/* Fills *out rather than returning a ParamInfo: cc has no hidden-pointer ABI
 * for returning a struct by value, and cc.c stays inside the language cc
 * accepts so that it can compile itself. */
static void parse_param_list(ParamInfo *out) {
    ParamInfo pi;
    memset(&pi, 0, sizeof(pi));
    expect(TOK_LPAREN);
    if (!consume(TOK_RPAREN)) {
        int void_only = 0;
        if (tok.kind == TOK_VOID) {
            int save_pos = pos;
            Token save_tok = tok;
            next_token();
            if (tok.kind == TOK_RPAREN) { next_token(); void_only = 1; }
            else { pos = save_pos; tok = save_tok; }
        }
        if (!void_only) {
            if (consume(TOK_ELLIPSIS)) {
                pi.is_varargs = 1;
                expect(TOK_RPAREN);
            } else {
                do {
                    DeclSpec ds;
                    parse_storage_spec(&ds);
                    if (ds.is_typedef || ds.is_extern) { error_here("param type\n"); }
                    Type *pt = parse_type_spec();
                    if (!pt) { error_here("param type\n"); }
                    char pname[32];
                    Type *pty = parse_declarator(pt, pname);
                    if (is_array(pty)) pty = type_ptr(pty->base);
                    /* struct parameters are passed by value: the caller copies
                     * the bytes onto the stack and the callee gets its own */
                    if (pi.count >= MAX_PARAM) { puts("param ovf\n"); sys_exit(1); }
                    strncpy(pi.names[pi.count], pname, 31);
                    pi.types[pi.count] = pty;
                    pi.count++;
                    if (consume(TOK_COMMA)) {
                        if (consume(TOK_ELLIPSIS)) { pi.is_varargs = 1; expect(TOK_RPAREN); break; }
                        continue;
                    }
                    expect(TOK_RPAREN);
                    break;
                } while (1);
            }
        }
    }
    *out = pi;
}

static void add_func_decl(Type *ret, const char *name, ParamInfo *pi, int is_static) {
    Func *f = add_func(name, ret);
    if (is_static && !f->is_static) func_make_static(f);
    if (f->defined) return;
    f->ret = ret;
    f->param_count = pi->count;
    for (int i = 0; i < pi->count; i++) f->params[i] = pi->types[i];
    f->is_varargs = pi->is_varargs;
    f->declared = 1;
}

static void parse_function_def(Type *ret, const char *name, ParamInfo *pi, int is_static) {
    Func *f = add_func(name, ret);
    if (is_static && !f->is_static) func_make_static(f);
    f->defined = 1;
    f->ret = ret;
    f->param_count = pi->count;
    for (int i = 0; i < pi->count; i++) f->params[i] = pi->types[i];
    f->is_varargs = pi->is_varargs;
    f->declared = 1;
    locals_len = 0;
    local_offset = 0;
    func_has_return = 0;
    in_func = 1;
    strncpy(current_func_name, name, 31);
    current_func_name[31] = 0;

    int poff = 8;
    for (int i = 0; i < pi->count; i++) {
        add_param(pi->names[i], pi->types[i], poff);
        poff += arg_slot_bytes(pi->types[i]);   /* a struct spans several slots */
    }

    set_label(f->label);
    /* prologue */
    emit8(0x55); /* push ebp */
    emit8(0x89); emit8(0xE5); /* mov ebp, esp */
    emit8(0x81); emit8(0xEC); int patch = code_len; emit32(0); /* sub esp, imm */

    /* Returning a struct by value would need a hidden-pointer ABI, which is
     * not implemented.  Reject it explicitly: struct expressions now evaluate
     * to an address, so without this check "return s;" would quietly hand the
     * caller a pointer where it expects the struct. */
    if (is_record(ret)) { puts("ret struct unsupported\n"); sys_exit(1); }
    goto_labels_reset();
    current_ret = ret;
    int epilogue = new_label();
    current_epilogue_label = epilogue;

    int func_pos = pos;
    gen_stmt();

    if (current_ret != &type_void && func_has_return == 0 && strcmp(name, "main")) {
        warn_at(func_pos, "no return in non-void function");
    }

    /* default return */
    if (current_ret == &type_int) emit_mov_eax_imm(0);
    emit_jmp_label(epilogue);

    set_label(epilogue);
    emit8(0xC9); /* leave */
    emit8(0xC3); /* ret */

    int stack_size = -local_offset;
    if (stack_size < 0) stack_size = 0;
    code[patch+0] = (uint8_t)(stack_size & 0xFF);
    code[patch+1] = (uint8_t)((stack_size >> 8) & 0xFF);
    code[patch+2] = (uint8_t)((stack_size >> 16) & 0xFF);
    code[patch+3] = (uint8_t)((stack_size >> 24) & 0xFF);
    current_epilogue_label = -1;
    goto_labels_check();   /* every 'goto' must name a label that exists */
    in_func = 0;
}

/* Write the recorded items into .data.  Globals must be constants, so this
 * accepts only literals -- and a string literal becomes a relocation, the same
 * mechanism a plain "const char *p = ..." global uses. */
static void store_global_init_items(Sym *s) {
    for (int i = 0; i < init_item_len; i++) {
        InitItem *it = &init_items[i];
        int off = s->offset + it->off;
        int sz = type_size(it->ty);
        Node *e = it->e;
        if (e->kind == ND_NUM) {
            unsigned int v = (unsigned int)e->ival;
            for (int b = 0; b < sz && b < 4; b++) data_seg[off + b] = (uint8_t)((v >> (8 * b)) & 0xFF);
        } else if (e->kind == ND_FNUM) {
            union { float f; uint32_t u; } u; u.f = e->fval;
            for (int b = 0; b < 4; b++) data_seg[off + b] = (uint8_t)((u.u >> (8 * b)) & 0xFF);
        } else if (e->kind == ND_STR) {
            int slen = strlen(e->name);
            int roff = rodata_len;
            memcpy(rodata + roff, e->name, slen + 1);
            rodata_len += slen + 1;
            data_seg[off+0] = 0; data_seg[off+1] = 0;
            data_seg[off+2] = 0; data_seg[off+3] = 0;
            add_addr_fix(off, AF_STR_DATA, roff);
        } else {
            puts("global init\n"); sys_exit(1);
        }
    }
}

/* A declaration may list several declarators sharing one base type:
 * `static Type type_int, type_float, type_char;`.  Returns 1 when a comma
 * followed, meaning the caller should parse another declarator. */
static int end_of_declarator(void) {
    if (consume(TOK_COMMA)) return 1;
    expect(TOK_SEMI);
    return 0;
}

static int parse_global(Type *ty, const char *name, int is_static) {
    Sym *exist = find_global(name);
    if (exist && !exist->is_extern) { puts("dup global\n"); sys_exit(1); }
    if (exist && is_static) { puts("extern/static\n"); sys_exit(1); }
    if (consume(TOK_ASSIGN)) {
        /* braced initialiser: parse it first, since "[]" takes its length
         * from what the list covers */
        if (tok.kind == TOK_LBRACE) {
            ty = parse_init_for(ty);
            Sym *sb = exist ? define_global_data_existing(exist, ty) : add_global_data(name, ty);
            if (is_static) sym_set_link_name(sb, 1);
            store_global_init_items(sb);
            return end_of_declarator();
        }

        /* char x[] = "..."  -- size comes from the literal, NUL included */
        if (is_array(ty) && ty->array_len == 0 && is_char(ty->base) && tok.kind == TOK_STR)
            ty = type_array(ty->base, strlen(tok.text) + 1);
        Sym *s = exist ? define_global_data_existing(exist, ty) : add_global_data(name, ty);
        if (is_static) sym_set_link_name(s, 1);
        if (is_array(ty)) {
            if (!is_char(ty->base) || tok.kind != TOK_STR) { puts("array init\n"); sys_exit(1); }
            int slen = strlen(tok.text);
            if (slen > type_size(ty)) slen = type_size(ty);
            memcpy(data_seg + s->offset, tok.text, slen);
            next_token();
        } else {
            Node *e = parse_expr();
            if (e->kind == ND_STR) {
                /* const char *p = "..." : park the text in rodata and leave a
                 * relocation so the linker writes its address into .data. */
                int slen2 = strlen(e->name);
                int roff = rodata_len;
                if (roff + slen2 + 1 > MAX_RODATA) { puts("rodata ovf\n"); sys_exit(1); }
                memcpy(rodata + roff, e->name, slen2 + 1);
                rodata_len += slen2 + 1;
                data_seg[s->offset+0] = 0; data_seg[s->offset+1] = 0;
                data_seg[s->offset+2] = 0; data_seg[s->offset+3] = 0;
                add_addr_fix(s->offset, AF_STR_DATA, roff);
            } else if (e->kind == ND_NUM) {
                int v = e->ival;
                data_seg[s->offset+0] = (uint8_t)(v & 0xFF);
                data_seg[s->offset+1] = (uint8_t)((v>>8) & 0xFF);
                data_seg[s->offset+2] = (uint8_t)((v>>16) & 0xFF);
                data_seg[s->offset+3] = (uint8_t)((v>>24) & 0xFF);
            } else if (e->kind == ND_FNUM) {
                union { float f; uint32_t u; } u; u.f = e->fval;
                data_seg[s->offset+0] = (uint8_t)(u.u & 0xFF);
                data_seg[s->offset+1] = (uint8_t)((u.u>>8) & 0xFF);
                data_seg[s->offset+2] = (uint8_t)((u.u>>16) & 0xFF);
                data_seg[s->offset+3] = (uint8_t)((u.u>>24) & 0xFF);
            } else {
                /* Anything the constant folder can reduce: `= MAX / 2`,
                 * `= sizeof(T)`, `= A | B`.  A global has to be initialised
                 * before main runs, so nothing else is allowed here. */
                int cv = 0;
                if (!eval_const(e, &cv)) { error_here("global init not constant\n"); }
                data_seg[s->offset+0] = (uint8_t)(cv & 0xFF);
                data_seg[s->offset+1] = (uint8_t)((cv>>8) & 0xFF);
                data_seg[s->offset+2] = (uint8_t)((cv>>16) & 0xFF);
                data_seg[s->offset+3] = (uint8_t)((cv>>24) & 0xFF);
            }
        }
        return end_of_declarator();
    }
    Sym *s = exist ? define_global_bss_existing(exist, ty) : add_global_bss(name, ty);
    if (is_static) sym_set_link_name(s, 1);
    return end_of_declarator();
}

static void parse_program(void) {
    next_token();
    while (tok.kind != TOK_EOF) {
        /* Codegen is single-pass, and neither Sym nor Func keeps a Node*, so
         * every node made for the previous construct is dead by now.  Reusing
         * the pool per top-level declaration is what lets MAX_NODE be sized
         * for the largest single function instead of the whole file -- a 4500
         * line source needs tens of thousands of nodes otherwise, at 260
         * bytes each. */
        node_len = 0;
        DeclSpec ds;
        parse_storage_spec(&ds);
        if (ds.is_static && ds.is_extern) { error_here("static/extern\n"); }
        if (ds.is_typedef) {
            Type *base = parse_type_spec();
            if (!base) { error_here("typedef type\n"); }
            for (;;) {
                char name[32];
                Type *ty = parse_declarator(base, name);
                add_typedef(name, ty);
                if (consume(TOK_COMMA)) continue;
                expect(TOK_SEMI);
                break;
            }
            continue;
        }
        int is_extern = ds.is_extern;
        Type *base = parse_type_spec();
        if (!base) { error_here("type?\n"); }
        if (tok.kind == TOK_SEMI) { next_token(); continue; }

        for (;;) {
            char name[32];
            Type *ty = parse_declarator(base, name);
            if (tok.kind == TOK_LPAREN) {
                if (is_array(ty) || is_record(ty)) { error_here("func return\n"); }
                ParamInfo pi;
                parse_param_list(&pi);
                if (tok.kind == TOK_SEMI || tok.kind == TOK_COMMA) {
                    add_func_decl(ty, name, &pi, ds.is_static);
                    if (end_of_declarator()) continue;
                    break;
                }
                /* A body ends the declaration; no comma list can follow. */
                parse_function_def(ty, name, &pi, ds.is_static);
                break;
            }
            if (is_extern) {
                if (consume(TOK_ASSIGN)) { puts("extern init\n"); sys_exit(1); }
                add_global_decl(name, ty);
                if (end_of_declarator()) continue;
                break;
            }
            if (parse_global(ty, name, ds.is_static)) continue;
            break;
        }
    }
}

static void apply_fixups(void) {
    for (int i=0;i<fixup_len;i++) {
        int pos = fixups[i].pos;
        int label = fixups[i].label;
        int target = labels_pos[label];
        int rel = target - (pos + 4);
        code[pos+0] = (uint8_t)(rel & 0xFF);
        code[pos+1] = (uint8_t)((rel >> 8) & 0xFF);
        code[pos+2] = (uint8_t)((rel >> 16) & 0xFF);
        code[pos+3] = (uint8_t)((rel >> 24) & 0xFF);
    }
}

static int build_elf(uint8_t *out, int out_max,
                     uint8_t *text, int text_len,
                     uint8_t *rodata, int rodata_len,
                     uint8_t *data, int data_len,
                     int bss_len) {
    int ehdr = 52, phdr = 32;
    int header = ehdr + phdr;
    int file_sz = header + text_len + rodata_len + data_len;
    if (file_sz > out_max) return -1;

    uint8_t *p = out;
    p[0]=0x7F; p[1]='E'; p[2]='L'; p[3]='F';
    p[4]=1; p[5]=1; p[6]=1; p[7]=0;
    for (int i=8;i<16;i++) p[i]=0;
    *(uint16_t*)(p+16)=2;
    *(uint16_t*)(p+18)=3;
    *(uint32_t*)(p+20)=1;
    *(uint32_t*)(p+24)=LINK_BASE + header; /* entry */
    *(uint32_t*)(p+28)=ehdr;
    *(uint32_t*)(p+32)=0;
    *(uint32_t*)(p+36)=0;
    *(uint16_t*)(p+40)=ehdr;
    *(uint16_t*)(p+42)=phdr;
    *(uint16_t*)(p+44)=1;
    *(uint16_t*)(p+46)=0;
    *(uint16_t*)(p+48)=0;
    *(uint16_t*)(p+50)=0;

    p += ehdr;
    *(uint32_t*)(p+0)=1;
    *(uint32_t*)(p+4)=0;
    *(uint32_t*)(p+8)=LINK_BASE;
    *(uint32_t*)(p+12)=LINK_BASE;
    *(uint32_t*)(p+16)=file_sz;
    *(uint32_t*)(p+20)=file_sz + bss_len;
    *(uint32_t*)(p+24)=7;
    *(uint32_t*)(p+28)=0x1000;

    p = out + header;
    memcpy(p, text, text_len); p += text_len;
    memcpy(p, rodata, rodata_len); p += rodata_len;
    memcpy(p, data, data_len);

    return file_sz;
}

#define MAX_MACRO 256
#define MAX_MACRO_BODY 256
#define MAX_MACRO_PARAMS 8

typedef struct {
    char name[32];
    int is_func;
    int param_count;
    char params[MAX_MACRO_PARAMS][32];
    char body[MAX_MACRO_BODY];
} Macro;

typedef struct {
    Macro macros[MAX_MACRO];
    int macro_len;
} Preproc;

static int pp_is_space(char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f'; }
static int pp_is_alpha(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int pp_is_alnum(char c) { return pp_is_alpha(c)||(c>='0'&&c<='9'); }

static int pp_find_macro(Preproc *pp, const char *name) {
    for (int i = 0; i < pp->macro_len; i++) if (!strcmp(pp->macros[i].name, name)) return i;
    return -1;
}

static void pp_define(Preproc *pp, Macro *m) {
    int idx = pp_find_macro(pp, m->name);
    if (idx >= 0) { pp->macros[idx] = *m; return; }
    if (pp->macro_len >= MAX_MACRO) return;
    pp->macros[pp->macro_len++] = *m;
}

static void pp_undef(Preproc *pp, const char *name) {
    int idx = pp_find_macro(pp, name);
    if (idx < 0) return;
    for (int i = idx; i + 1 < pp->macro_len; i++) pp->macros[i] = pp->macros[i + 1];
    pp->macro_len--;
}

typedef struct {
    const char *p;
    Preproc *pp;
} PPExpr;

static void pp_expr_skip(PPExpr *e) { while (pp_is_space(*e->p)) e->p++; }

static int pp_expr_match(PPExpr *e, const char *op) {
    pp_expr_skip(e);
    int n = 0;
    while (op[n]) n++;
    if (!strncmp(e->p, op, n)) { e->p += n; return 1; }
    return 0;
}

static int pp_expr_number(PPExpr *e, int *out) {
    pp_expr_skip(e);
    const char *p = e->p;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        int v = 0;
        int hv = 0;
        while (1) {
            char c = *p;
            if (c >= '0' && c <= '9') hv = c - '0';
            else if (c >= 'a' && c <= 'f') hv = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') hv = c - 'A' + 10;
            else break;
            v = (v << 4) + hv;
            p++;
        }
        *out = neg ? -v : v;
        e->p = p;
        return 1;
    }
    if (*p < '0' || *p > '9') return 0;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *out = neg ? -v : v;
    e->p = p;
    return 1;
}

static int pp_macro_value(Preproc *pp, const char *name) {
    int idx = pp_find_macro(pp, name);
    if (idx < 0) return 0;
    Macro *m = &pp->macros[idx];
    const char *b = m->body;
    int v = 0;
    PPExpr e;
    e.p = b;
    e.pp = pp;
    if (pp_expr_number(&e, &v)) return v;
    return 1;
}

static int pp_parse_lor(PPExpr *e);

static int pp_parse_primary(PPExpr *e) {
    pp_expr_skip(e);
    if (*e->p == '(') {
        e->p++;
        int v = pp_parse_lor(e);
        pp_expr_skip(e);
        if (*e->p == ')') e->p++;
        return v;
    }
    if (pp_is_alpha(*e->p)) {
        char name[32]; int ni = 0;
        while (pp_is_alnum(*e->p) && ni < 31) name[ni++] = *e->p++;
        name[ni] = 0;
        if (!strcmp(name, "defined")) {
            pp_expr_skip(e);
            if (*e->p == '(') {
                e->p++;
                pp_expr_skip(e);
                char dn[32]; int di = 0;
                while (pp_is_alnum(*e->p) && di < 31) dn[di++] = *e->p++;
                dn[di] = 0;
                pp_expr_skip(e);
                if (*e->p == ')') e->p++;
                return pp_find_macro(e->pp, dn) >= 0;
            } else {
                char dn[32]; int di = 0;
                while (pp_is_alnum(*e->p) && di < 31) dn[di++] = *e->p++;
                dn[di] = 0;
                return pp_find_macro(e->pp, dn) >= 0;
            }
        }
        return pp_macro_value(e->pp, name);
    }
    int v = 0;
    if (pp_expr_number(e, &v)) return v;
    return 0;
}

static int pp_parse_unary(PPExpr *e) {
    if (pp_expr_match(e, "!")) return !pp_parse_unary(e);
    if (pp_expr_match(e, "~")) return ~pp_parse_unary(e);
    if (pp_expr_match(e, "+")) return pp_parse_unary(e);
    if (pp_expr_match(e, "-")) return -pp_parse_unary(e);
    return pp_parse_primary(e);
}

static int pp_parse_mul(PPExpr *e) {
    int v = pp_parse_unary(e);
    for (;;) {
        if (pp_expr_match(e, "*")) v = v * pp_parse_unary(e);
        else if (pp_expr_match(e, "/")) { int r = pp_parse_unary(e); if (r) v = v / r; }
        else if (pp_expr_match(e, "%")) { int r = pp_parse_unary(e); if (r) v = v % r; }
        else break;
    }
    return v;
}

static int pp_parse_add(PPExpr *e) {
    int v = pp_parse_mul(e);
    for (;;) {
        if (pp_expr_match(e, "+")) v = v + pp_parse_mul(e);
        else if (pp_expr_match(e, "-")) v = v - pp_parse_mul(e);
        else break;
    }
    return v;
}

static int pp_parse_shift(PPExpr *e) {
    int v = pp_parse_add(e);
    for (;;) {
        if (pp_expr_match(e, "<<")) v = v << pp_parse_add(e);
        else if (pp_expr_match(e, ">>")) v = v >> pp_parse_add(e);
        else break;
    }
    return v;
}

static int pp_parse_rel(PPExpr *e) {
    int v = pp_parse_shift(e);
    for (;;) {
        if (pp_expr_match(e, "<=")) v = v <= pp_parse_shift(e);
        else if (pp_expr_match(e, ">=")) v = v >= pp_parse_shift(e);
        else if (pp_expr_match(e, "<")) v = v < pp_parse_shift(e);
        else if (pp_expr_match(e, ">")) v = v > pp_parse_shift(e);
        else break;
    }
    return v;
}

static int pp_parse_eq(PPExpr *e) {
    int v = pp_parse_rel(e);
    for (;;) {
        if (pp_expr_match(e, "==")) v = v == pp_parse_rel(e);
        else if (pp_expr_match(e, "!=")) v = v != pp_parse_rel(e);
        else break;
    }
    return v;
}

static int pp_parse_bitand(PPExpr *e) {
    int v = pp_parse_eq(e);
    while (pp_expr_match(e, "&")) v = v & pp_parse_eq(e);
    return v;
}

static int pp_parse_bitxor(PPExpr *e) {
    int v = pp_parse_bitand(e);
    while (pp_expr_match(e, "^")) v = v ^ pp_parse_bitand(e);
    return v;
}

static int pp_parse_bitor(PPExpr *e) {
    int v = pp_parse_bitxor(e);
    while (pp_expr_match(e, "|")) v = v | pp_parse_bitxor(e);
    return v;
}

static int pp_parse_land(PPExpr *e) {
    int v = pp_parse_bitor(e);
    while (pp_expr_match(e, "&&")) v = (v && pp_parse_bitor(e));
    return v;
}

static int pp_parse_lor(PPExpr *e) {
    int v = pp_parse_land(e);
    while (pp_expr_match(e, "||")) v = (v || pp_parse_land(e));
    return v;
}

static int pp_eval_if(Preproc *pp, const char *expr) {
    PPExpr e;
    e.p = expr;
    e.pp = pp;
    return pp_parse_lor(&e) != 0;
}

static int pp_expand_text(Preproc *pp, const char *in, char *out, int out_max, int depth, const char *file, int line) {
    int out_len = 0;
    int i = 0;
    int in_str = 0, in_chr = 0;
    if (depth <= 0) {
        while (in[i] && out_len + 1 < out_max) out[out_len++] = in[i++];
        out[out_len] = 0;
        return out_len;
    }
    while (in[i]) {
        char c = in[i];
        if (in_str) {
            if (out_len + 1 >= out_max) break;
            out[out_len++] = c;
            if (c == '\\' && in[i+1]) { out[out_len++] = in[i+1]; i += 2; continue; }
            if (c == '"') in_str = 0;
            i++;
            continue;
        }
        if (in_chr) {
            if (out_len + 1 >= out_max) break;
            out[out_len++] = c;
            if (c == '\\' && in[i+1]) { out[out_len++] = in[i+1]; i += 2; continue; }
            if (c == '\'') in_chr = 0;
            i++;
            continue;
        }
        if (c == '"') { in_str = 1; out[out_len++] = c; i++; continue; }
        if (c == '\'') { in_chr = 1; out[out_len++] = c; i++; continue; }
        if (pp_is_alpha(c)) {
            char name[32]; int ni = 0;
            while (pp_is_alnum(in[i]) && ni < 31) name[ni++] = in[i++];
            name[ni] = 0;
            if (!strcmp(name, "__LINE__")) {
                char nb[16]; int bi = 0; int n = line;
                if (n == 0) nb[bi++] = '0';
                else { int t = n; char tmp[16]; int ti=0; while (t>0){ int q=t/10; int d=t-q*10; tmp[ti++]=(char)('0'+d); t=q; } while (ti>0) nb[bi++]=tmp[--ti]; }
                for (int k=0;k<bi && out_len+1<out_max;k++) out[out_len++]=nb[k];
                continue;
            }
            if (!strcmp(name, "__FILE__")) {
                if (out_len + 1 < out_max) out[out_len++] = '"';
                for (int k=0; file[k] && out_len+1<out_max; k++) out[out_len++] = file[k];
                if (out_len + 1 < out_max) out[out_len++] = '"';
                continue;
            }
            int midx = pp_find_macro(pp, name);
            if (midx < 0) {
                for (int k = 0; k < ni && out_len + 1 < out_max; k++) out[out_len++] = name[k];
                continue;
            }
            Macro *m = &pp->macros[midx];
            if (m->is_func) {
                int j = i;
                if (in[j] != '(') {
                    for (int k = 0; k < ni && out_len + 1 < out_max; k++) out[out_len++] = name[k];
                    i = j;
                    continue;
                }
                j++;
                char args[MAX_MACRO_PARAMS][128];
                int ac = 0;
                int depthp = 1;
                int ai = 0;
                while (in[j] && depthp > 0) {
                    char ch = in[j];
                    if (ch == '(') { depthp++; args[ac][ai++] = ch; j++; continue; }
                    if (ch == ')') {
                        depthp--;
                        if (depthp == 0) { j++; break; }
                        args[ac][ai++] = ch; j++; continue;
                    }
                    if (ch == ',' && depthp == 1) {
                        args[ac][ai] = 0;
                        if (ac + 1 < MAX_MACRO_PARAMS) ac++;
                        ai = 0;
                        j++;
                        continue;
                    }
                    args[ac][ai++] = ch; j++;
                    if (ai >= 120) { args[ac][ai] = 0; }
                }
                args[ac][ai] = 0;
                ac++;
                i = j;

                char expanded_args[MAX_MACRO_PARAMS][256];
                for (int a = 0; a < ac && a < MAX_MACRO_PARAMS; a++) {
                    pp_expand_text(pp, args[a], expanded_args[a], sizeof(expanded_args[a]), depth - 1, file, line);
                }

                char temp[512];
                int tlen = 0;
                int bi = 0;
                while (m->body[bi] && tlen + 1 < (int)sizeof(temp)) {
                    if (pp_is_alpha(m->body[bi])) {
                        char tn[32]; int ti = 0;
                        while (pp_is_alnum(m->body[bi]) && ti < 31) tn[ti++] = m->body[bi++];
                        tn[ti] = 0;
                        int replaced = 0;
                        for (int a = 0; a < m->param_count; a++) {
                            if (!strcmp(tn, m->params[a])) {
                                const char *rep = (a < ac) ? expanded_args[a] : "";
                                for (int k = 0; rep[k] && tlen + 1 < (int)sizeof(temp); k++) temp[tlen++] = rep[k];
                                replaced = 1;
                                break;
                            }
                        }
                        if (!replaced) {
                            for (int k = 0; k < ti && tlen + 1 < (int)sizeof(temp); k++) temp[tlen++] = tn[k];
                        }
                        continue;
                    }
                    temp[tlen++] = m->body[bi++];
                }
                temp[tlen] = 0;
                char expanded[512];
                int elen = pp_expand_text(pp, temp, expanded, sizeof(expanded), depth - 1, file, line);
                for (int k = 0; k < elen && out_len + 1 < out_max; k++) out[out_len++] = expanded[k];
                continue;
            } else {
                char expanded[512];
                int elen = pp_expand_text(pp, m->body, expanded, sizeof(expanded), depth - 1, file, line);
                for (int k = 0; k < elen && out_len + 1 < out_max; k++) out[out_len++] = expanded[k];
                continue;
            }
        }
        if (out_len + 1 >= out_max) break;
        out[out_len++] = c;
        i++;
    }
    out[out_len] = 0;
    return out_len;
}

/* Quoted includes search the including file's directory, then the working
 * directory, then the root.  Angle-bracket includes are root-only.  On
 * success `resolved` holds the path that worked, so nested includes inside
 * that file resolve against its directory rather than the top-level one. */
static int load_include(const char *name, const char *from_file, int quoted,
                        char *buf, int max, char *resolved, int resolved_sz) {
    if (quoted) {
        char dir[PATH_MAX_LEN];
        path_dir(from_file ? from_file : "", dir, sizeof(dir));
        if (dir[0] && path_fs(dir, name, resolved, resolved_sz)) {
            int n = sys_load(resolved, buf, max);
            if (n > 0) return n;
        }
        if (cwd_get()[0] && path_fs(cwd_get(), name, resolved, resolved_sz)) {
            int n = sys_load(resolved, buf, max);
            if (n > 0) return n;
        }
    }
    if (path_fs("", name, resolved, resolved_sz)) {
        int n = sys_load(resolved, buf, max);
        if (n > 0) return n;
    }
    /* system headers live in /lib, so <stdio.h> works from anywhere */
    if (!path_fs("lib", name, resolved, resolved_sz)) return -1;
    return sys_load(resolved, buf, max);
}

static int pp_process(Preproc *pp, const char *in, const char *filename, char *out, int out_max, int depth, int skip_stdio) {
    if (depth >= PP_MAX_DEPTH) { puts("include depth\n"); return -1; }
    int out_len = 0;
    int line = 1;
    const char *p = in;
    if (depth == 0 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    int cond_active[16];
    int cond_taken[16];
    int cond_depth = 0;
    while (*p) {
        char linebuf[LINEBUF_MAX];
        int li = 0;
        while (*p && *p != '\n' && li < (int)sizeof(linebuf) - 1) linebuf[li++] = *p++;
        if (*p == '\n') { p++; }
        linebuf[li] = 0;

        const char *q = linebuf;
        while (pp_is_space(*q)) q++;
        int active = 1;
        for (int i = 0; i < cond_depth; i++) if (!cond_active[i]) { active = 0; break; }

        if (*q == '#') {
            q++;
            while (pp_is_space(*q)) q++;
            if (!strncmp(q, "include", 7) && pp_is_space(q[7])) {
                q += 7;
                while (pp_is_space(*q)) q++;
                if (!active) { line++; continue; }
                char end = 0;
                if (*q == '\"') { end = '\"'; q++; }
                else if (*q == '<') { end = '>'; q++; }
                if (end) {
                    char name[64]; int ni = 0;
                    while (*q && *q != end && ni < 63) name[ni++] = *q++;
                    name[ni] = 0;
                    if (skip_stdio && !strcmp(name, "stdio.c")) { line++; continue; }
                    if (depth + 1 >= PP_MAX_DEPTH) { puts("include depth\n"); return -1; }
                    char *incbuf = incbuf_pool[depth + 1];
                    char incpath[PATH_MAX_LEN];
                    int n = load_include(name, filename, end == '\"', incbuf,
                                         INC_BUF_MAX - 1, incpath, sizeof(incpath));
                    if (n <= 0) { puts("include fail: "); puts(name); putc('\n'); return -1; }
                    incbuf[n] = 0;
                    int n2 = pp_process(pp, incbuf, incpath, out + out_len, out_max - out_len - 1, depth + 1, skip_stdio);
                    if (n2 < 0) return -1;
                    out_len += n2;
                }
                line++;
                continue;
            }
            if (!strncmp(q, "define", 6) && pp_is_space(q[6])) {
                q += 6;
                while (pp_is_space(*q)) q++;
                if (active) {
                    Macro m; memset(&m, 0, sizeof(m));
                    int ni = 0;
                    while (pp_is_alnum(*q) && ni < 31) m.name[ni++] = *q++;
                    m.name[ni] = 0;
                    if (*q == '(') {
                        m.is_func = 1;
                        q++;
                        while (pp_is_space(*q)) q++;
                        while (*q && *q != ')') {
                            char pn[32]; int pi = 0;
                            while (pp_is_alnum(*q) && pi < 31) pn[pi++] = *q++;
                            pn[pi] = 0;
                            if (m.param_count < MAX_MACRO_PARAMS) strncpy(m.params[m.param_count++], pn, 31);
                            while (pp_is_space(*q)) q++;
                            if (*q == ',') { q++; while (pp_is_space(*q)) q++; continue; }
                            else break;
                        }
                        if (*q == ')') q++;
                    }
                    while (pp_is_space(*q)) q++;
                    strncpy(m.body, q, MAX_MACRO_BODY - 1);
                    pp_define(pp, &m);
                }
                line++;
                continue;
            }
            if (!strncmp(q, "undef", 5) && pp_is_space(q[5])) {
                q += 5;
                while (pp_is_space(*q)) q++;
                if (active) {
                    char name[32]; int ni = 0;
                    while (pp_is_alnum(*q) && ni < 31) name[ni++] = *q++;
                    name[ni] = 0;
                    pp_undef(pp, name);
                }
                line++;
                continue;
            }
            if (!strncmp(q, "ifdef", 5) && pp_is_space(q[5])) {
                q += 5;
                while (pp_is_space(*q)) q++;
                char name[32]; int ni = 0;
                while (pp_is_alnum(*q) && ni < 31) name[ni++] = *q++;
                name[ni] = 0;
                int val = (pp_find_macro(pp, name) >= 0);
                cond_active[cond_depth] = active && val;
                cond_taken[cond_depth] = val;
                cond_depth++;
                line++;
                continue;
            }
            if (!strncmp(q, "ifndef", 6) && pp_is_space(q[6])) {
                q += 6;
                while (pp_is_space(*q)) q++;
                char name[32]; int ni = 0;
                while (pp_is_alnum(*q) && ni < 31) name[ni++] = *q++;
                name[ni] = 0;
                int val = (pp_find_macro(pp, name) < 0);
                cond_active[cond_depth] = active && val;
                cond_taken[cond_depth] = val;
                cond_depth++;
                line++;
                continue;
            }
            if (!strncmp(q, "if", 2) && pp_is_space(q[2])) {
                q += 2;
                int val = pp_eval_if(pp, q);
                cond_active[cond_depth] = active && val;
                cond_taken[cond_depth] = val;
                cond_depth++;
                line++;
                continue;
            }
            if (!strncmp(q, "elif", 4) && pp_is_space(q[4])) {
                if (cond_depth == 0) { line++; continue; }
                q += 4;
                int parent_active = 1;
                for (int i = 0; i < cond_depth - 1; i++) if (!cond_active[i]) { parent_active = 0; break; }
                if (cond_taken[cond_depth - 1]) {
                    cond_active[cond_depth - 1] = 0;
                } else {
                    int val = pp_eval_if(pp, q);
                    cond_active[cond_depth - 1] = parent_active && val;
                    if (val) cond_taken[cond_depth - 1] = 1;
                }
                line++;
                continue;
            }
            if (!strncmp(q, "else", 4)) {
                if (cond_depth == 0) { line++; continue; }
                int parent_active = 1;
                for (int i = 0; i < cond_depth - 1; i++) if (!cond_active[i]) { parent_active = 0; break; }
                if (cond_taken[cond_depth - 1]) cond_active[cond_depth - 1] = 0;
                else { cond_active[cond_depth - 1] = parent_active; cond_taken[cond_depth - 1] = 1; }
                line++;
                continue;
            }
            if (!strncmp(q, "endif", 5)) {
                if (cond_depth > 0) cond_depth--;
                line++;
                continue;
            }
            line++;
            continue;
        }

        if (active) {
            char expanded[1024];
            int elen = pp_expand_text(pp, linebuf, expanded, sizeof(expanded), 8, filename, line);
            if (out_len + elen + 2 >= out_max) { puts("pp ovf\n"); return -1; }
            memcpy(out + out_len, expanded, elen);
            out_len += elen;
            out[out_len++] = '\n';
            lm_add(filename, line);
        }
        line++;
    }
    out[out_len] = 0;
    return out_len;
}

static int split_sources(const char *line, char names[][64], int max) {
    int count = 0;
    int i = 0;
    while (line[i]) {
        while (line[i] && pp_is_space(line[i])) i++;
        if (!line[i]) break;
        if (count >= max) break;
        int ni = 0;
        while (line[i] && !pp_is_space(line[i]) && ni < 63) {
            names[count][ni++] = line[i++];
        }
        names[count][ni] = 0;
        count++;
    }
    return count;
}

static void obj_add_sym(Obj *obj, const char *name, int section, int value, int size, int defined, int is_func) {
    if (obj->sym_len >= MAX_SYM) { puts("sym ovf\n"); sys_exit(1); }
    ObjSym *s = &obj->syms[obj->sym_len++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 31);
    s->name[31] = 0;
    s->section = section;
    s->value = value;
    s->size = size;
    s->defined = defined;
    s->is_func = is_func;
}

static int compile_obj(const char *srcname, Obj *obj, int skip_stdio) {
    static char srcbuf[SRCBUF_MAX];
    static char ppbuf[PPBUF_MAX];

    int n = sys_load(srcname, srcbuf, sizeof(srcbuf) - 1);
    if (n <= 0 && strncmp(srcname, "lib/", 4)) {
        /* a missing source falls back to /lib by basename, so `gfx.c`
           finds lib/gfx.c from any working directory -- typing `gfx.c`
           inside /demo resolves to demo/gfx.c, which does not exist */
        static char libname[PATH_MAX_LEN];
        strcpy(libname, "lib/");
        strncpy(libname + 4, path_base(srcname), PATH_MAX_LEN - 5);
        libname[PATH_MAX_LEN - 1] = 0;
        n = sys_load(libname, srcbuf, sizeof(srcbuf) - 1);
        if (n > 0) srcname = libname;
    }
    if (n <= 0) { puts("read fail: "); puts(srcname); putc('\n'); return -1; }
    srcbuf[n] = 0;

    strncpy(cur_file, srcname, 63);
    cur_file[63] = 0;
    current_unit_id = unit_counter++;
    static_sym_id = 0;

    lm_count = 0;
    lm_files_len = 0;
    diag_ovr_end = 0;

    Preproc pp; memset(&pp, 0, sizeof(pp));
    int pn = pp_process(&pp, srcbuf, srcname, ppbuf, sizeof(ppbuf), 0, skip_stdio);
    if (pn < 0) return -1;

    src = ppbuf;
    pos = 0;
    code_len = rodata_len = data_len = 0;
    bss_len = 0;
    node_len = locals_len = globals_len = funcs_len = 0;
    fixup_len = addr_fix_len = 0;
    sym_fix_len = 0;
    label_count = 0;
    type_pool_len = 0;
    structs_len = 0;
    typedefs_len = 0;
    enum_const_len = 0;
    break_depth = 0;
    switch_depth = 0;
    init_types();

    parse_program();
    apply_fixups();

    memset(obj, 0, sizeof(*obj));
    memcpy(obj->text, code, code_len);
    obj->text_len = code_len;
    memcpy(obj->rodata, rodata, rodata_len);
    obj->rodata_len = rodata_len;
    memcpy(obj->data, data_seg, data_len);
    obj->data_len = data_len;
    obj->bss_len = bss_len;

    for (int i = 0; i < funcs_len; i++) {
        Func *f = &funcs[i];
        if (f->defined) {
            int off = labels_pos[f->label];
            const char *lname = f->link_name[0] ? f->link_name : f->name;
#ifdef HOST_DEBUG
            if (f->link_name[0]) { puts("FUNC "); puts(lname); putc(' '); puts(f->name); putc('\n'); }
            if (obj->text[off] != 0x55) {
                puts("BADSYM "); puts(f->name); putc(' ');
                print_dec(off); putc(' '); print_dec(f->label); putc('\n');
            }
#endif
            obj_add_sym(obj, lname, SEC_TEXT, off, 0, 1, 1);
        } else {
            const char *lname = f->link_name[0] ? f->link_name : f->name;
            obj_add_sym(obj, lname, 0, 0, 0, 0, 1);
        }
    }
    for (int i = 0; i < globals_len; i++) {
        Sym *s = &globals[i];
        if (s->is_extern) {
            const char *lname = s->link_name[0] ? s->link_name : s->name;
            obj_add_sym(obj, lname, 0, 0, 0, 0, 0);
        } else {
            int sz = type_size(s->type);
            const char *lname = s->link_name[0] ? s->link_name : s->name;
            obj_add_sym(obj, lname, s->section, s->offset, sz, 1, 0);
        }
    }

    for (int i = 0; i < addr_fix_len; i++) {
        int pos = addr_fix[i].pos;
        if (addr_fix[i].kind == AF_STR_DATA) {
            if (obj->lreloc_len >= MAX_RELOC) { puts("reloc ovf\n"); return -1; }
            ObjRelocLoc *r = &obj->lreloc[obj->lreloc_len++];
            r->section = SEC_DATA;          /* patch .data, not .text */
            r->offset = pos;
            r->type = RELOC_ABS32;
            r->target_section = SEC_RODATA;
            r->addend = addr_fix[i].index;
        } else if (addr_fix[i].kind == AF_STR) {
            if (obj->lreloc_len >= MAX_RELOC) { puts("reloc ovf\n"); return -1; }
            ObjRelocLoc *r = &obj->lreloc[obj->lreloc_len++];
            r->section = SEC_TEXT;
            r->offset = pos;
            r->type = RELOC_ABS32;
            r->target_section = SEC_RODATA;
            r->addend = addr_fix[i].index;
        } else if (addr_fix[i].kind == AF_GLOB) {
            if (obj->sreloc_len >= MAX_RELOC) { puts("reloc ovf\n"); return -1; }
            ObjRelocSym *r = &obj->sreloc[obj->sreloc_len++];
            r->section = SEC_TEXT;
            r->offset = pos;
            r->type = RELOC_ABS32;
            r->addend = 0;
            const char *lname = globals[addr_fix[i].index].link_name[0] ? globals[addr_fix[i].index].link_name : globals[addr_fix[i].index].name;
            strncpy(r->name, lname, 31);
        }
    }

    for (int i = 0; i < sym_fix_len; i++) {
        if (obj->sreloc_len >= MAX_RELOC) { puts("reloc ovf\n"); return -1; }
        ObjRelocSym *r = &obj->sreloc[obj->sreloc_len++];
        r->section = SEC_TEXT;
        r->offset = sym_fix[i].pos;
        r->type = sym_fix[i].type;
        r->addend = 0;
        strncpy(r->name, sym_fix[i].name, 31);
    }

    return 0;
}

static void build_start_obj(Obj *obj) {
    memset(obj, 0, sizeof(*obj));
    int p = 0;
    /* FPU control word 0x037F: round to nearest, all exceptions masked -- the
     * IEEE default, and what every other C implementation gives you.  Casts
     * chop by flipping the mode for the one instruction (conv_stack_to_int);
     * setting chop here instead truncated every float operation in the
     * program, so results came out up to an ulp low and overflow saturated to
     * FLT_MAX where it should have produced inf. */
    obj->text[p++] = 0x83; obj->text[p++] = 0xEC; obj->text[p++] = 0x04; /* sub esp, 4 */
    obj->text[p++] = 0x66; obj->text[p++] = 0xC7; obj->text[p++] = 0x04; obj->text[p++] = 0x24; /* mov word [esp], imm16 */
    obj->text[p++] = 0x7F; obj->text[p++] = 0x03; /* 0x037F */
    obj->text[p++] = 0xD9; obj->text[p++] = 0x2C; obj->text[p++] = 0x24; /* fldcw [esp] */
    obj->text[p++] = 0x83; obj->text[p++] = 0xC4; obj->text[p++] = 0x04; /* add esp, 4 */

    int call_off = p;
    obj->text[p++] = 0xE8; /* call rel32 */
    obj->text[p++] = 0x00;
    obj->text[p++] = 0x00;
    obj->text[p++] = 0x00;
    obj->text[p++] = 0x00;
    obj->text[p++] = 0x89; obj->text[p++] = 0xC3; /* mov ebx, eax */
    obj->text[p++] = 0xB8; /* mov eax, imm32 */
    obj->text[p++] = 0x06; obj->text[p++] = 0x00; obj->text[p++] = 0x00; obj->text[p++] = 0x00; /* SYS_EXIT */
    obj->text[p++] = 0xCD; obj->text[p++] = 0x80; /* int 0x80 */
    obj->text[p++] = 0xEB; obj->text[p++] = 0xFE; /* jmp $ */
    obj->text_len = p;
    obj->sreloc_len = 0;
    ObjRelocSym *r = &obj->sreloc[obj->sreloc_len++];
    r->section = SEC_TEXT;
    r->offset = call_off + 1;
    r->type = RELOC_REL32;
    r->addend = 0;
    strncpy(r->name, "main", 31);
}

typedef struct { char name[32]; int addr; int defined; } LinkSym;

static int link_find(LinkSym *syms, int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(syms[i].name, name)) return i;
    return -1;
}

static void write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int is_builtin_sym(const char *name) {
    if (!name) return 0;
    if (!strcmp(name, "syscall")) return 1;
    if (!strcmp(name, "va_start")) return 1;
    if (!strcmp(name, "va_end")) return 1;
    if (!strncmp(name, "sys_", 4)) return 1;
    return 0;
}

static int link_objects(Obj *objs, int obj_count,
                        uint8_t *out_text, int *out_text_len,
                        uint8_t *out_rodata, int *out_rodata_len,
                        uint8_t *out_data, int *out_data_len,
                        int *out_bss) {
    int text_total = 0, rodata_total = 0, data_total = 0, bss_total = 0;
    int text_off[MAX_OBJ], rodata_off[MAX_OBJ], data_off[MAX_OBJ], bss_off[MAX_OBJ];
    for (int i = 0; i < obj_count; i++) {
        text_off[i] = text_total; text_total += objs[i].text_len;
        rodata_off[i] = rodata_total; rodata_total += objs[i].rodata_len;
        data_off[i] = data_total; data_total += objs[i].data_len;
        bss_off[i] = bss_total; bss_total += objs[i].bss_len;
    }
    if (text_total > MAX_OUT_TEXT || rodata_total > MAX_OUT_RODATA || data_total > MAX_OUT_DATA) {
        puts("link ovf\n"); return -1;
    }
    for (int i = 0; i < obj_count; i++) {
        memcpy(out_text + text_off[i], objs[i].text, objs[i].text_len);
        memcpy(out_rodata + rodata_off[i], objs[i].rodata, objs[i].rodata_len);
        memcpy(out_data + data_off[i], objs[i].data, objs[i].data_len);
    }

    LinkSym syms[MAX_SYM * MAX_OBJ];
    int sym_len = 0;
    int header = 52 + 32;
    int text_base = LINK_BASE + header;
    int rodata_base = text_base + text_total;
    int data_base = rodata_base + rodata_total;
    int bss_base = data_base + data_total;

    for (int i = 0; i < obj_count; i++) {
        for (int s = 0; s < objs[i].sym_len; s++) {
            ObjSym *os = &objs[i].syms[s];
            if (os->defined) {
                int addr = 0;
                if (os->section == SEC_TEXT) addr = text_base + text_off[i] + os->value;
                else if (os->section == SEC_RODATA) addr = rodata_base + rodata_off[i] + os->value;
                else if (os->section == SEC_DATA) addr = data_base + data_off[i] + os->value;
                else if (os->section == SEC_BSS) addr = bss_base + bss_off[i] + os->value;
#ifdef HOST_DEBUG
                puts("SYM "); print_dec(addr); putc(' '); puts(os->name); putc('\n');
#endif
                int idx = link_find(syms, sym_len, os->name);
                if (idx >= 0) {
                    if (syms[idx].defined) { puts("dup sym "); puts(os->name); puts("\n"); return -1; }
                    syms[idx].defined = 1;
                    syms[idx].addr = addr;
                } else {
                    strncpy(syms[sym_len].name, os->name, 31);
                    syms[sym_len].defined = 1;
                    syms[sym_len].addr = addr;
                    sym_len++;
                }
            } else {
                int idx = link_find(syms, sym_len, os->name);
                if (idx < 0) {
                    strncpy(syms[sym_len].name, os->name, 31);
                    syms[sym_len].defined = 0;
                    syms[sym_len].addr = 0;
                    sym_len++;
                }
            }
        }
    }
    for (int i = 0; i < sym_len; i++) {
        if (!syms[i].defined) {
            if (is_builtin_sym(syms[i].name)) continue;
            /* An unused prototype is not an error -- a header may declare
               functions no linked source defines (libc.h declares atoi, which
               lives in stdlib.c).  Only a reference to a missing definition
               is fatal, and the reloc pass below also reports those. */
            int used = 0;
            for (int o = 0; o < obj_count && !used; o++)
                for (int r = 0; r < objs[o].sreloc_len; r++)
                    if (!strcmp(objs[o].sreloc[r].name, syms[i].name)) { used = 1; break; }
            if (!used) continue;
            puts("undef "); puts(syms[i].name); puts("\n"); return -1;
        }
    }

    for (int i = 0; i < obj_count; i++) {
        for (int r = 0; r < objs[i].lreloc_len; r++) {
            ObjRelocLoc *lr = &objs[i].lreloc[r];
            uint8_t *buf = 0;
            int base = 0;
            int patch_off = 0;
            if (lr->section == SEC_TEXT) { buf = out_text; base = text_base + text_off[i]; patch_off = text_off[i] + lr->offset; }
            else if (lr->section == SEC_RODATA) { buf = out_rodata; base = rodata_base + rodata_off[i]; patch_off = rodata_off[i] + lr->offset; }
            else if (lr->section == SEC_DATA) { buf = out_data; base = data_base + data_off[i]; patch_off = data_off[i] + lr->offset; }
            int target_base = 0;
            if (lr->target_section == SEC_TEXT) target_base = text_base + text_off[i];
            else if (lr->target_section == SEC_RODATA) target_base = rodata_base + rodata_off[i];
            else if (lr->target_section == SEC_DATA) target_base = data_base + data_off[i];
            else if (lr->target_section == SEC_BSS) target_base = bss_base + bss_off[i];
            int patch_addr = base + (lr->offset);
            int val = target_base + lr->addend;
            if (lr->type == RELOC_REL32) val = val - (patch_addr + 4);
            write32(buf + patch_off, (uint32_t)val);
        }
        for (int r = 0; r < objs[i].sreloc_len; r++) {
            ObjRelocSym *sr = &objs[i].sreloc[r];
            uint8_t *buf = 0;
            int base = 0;
            int patch_off = 0;
            if (sr->section == SEC_TEXT) { buf = out_text; base = text_base + text_off[i]; patch_off = text_off[i] + sr->offset; }
            else if (sr->section == SEC_RODATA) { buf = out_rodata; base = rodata_base + rodata_off[i]; patch_off = rodata_off[i] + sr->offset; }
            else if (sr->section == SEC_DATA) { buf = out_data; base = data_base + data_off[i]; patch_off = data_off[i] + sr->offset; }
            int idx = link_find(syms, sym_len, sr->name);
            if (idx < 0 || !syms[idx].defined) { puts("undef "); puts(sr->name); puts("\n"); return -1; }
            int sym_addr = syms[idx].addr + sr->addend;
            int patch_addr = base + sr->offset;
            int val = (sr->type == RELOC_REL32) ? (sym_addr - (patch_addr + 4)) : sym_addr;
            write32(buf + patch_off, (uint32_t)val);
        }
    }

    *out_text_len = text_total;
    *out_rodata_len = rodata_total;
    *out_data_len = data_total;
    *out_bss = bss_total;
    return 0;
}

int main(void) {
    char srcline[256];
    char outname[64];
    /* Sized to the kernel's FILE_BUF_MAX: an ELF bigger than this could be
       saved but never executed, so there is no point emitting one. */
    static uint8_t outbuf[1024*1024];
    static uint8_t text[MAX_OUT_TEXT];
    static uint8_t rodata_buf[MAX_OUT_RODATA];
    static uint8_t data_buf[MAX_OUT_DATA];

    puts("cc v8 (relative paths)\n");
    if (cwd_get()[0]) { puts("dir: /"); puts(cwd_get()); putc('\n'); }
    puts("source files: ");
    readline(srcline, sizeof(srcline));
    puts("output file: ");
    readline(outname, sizeof(outname));

    char files[MAX_OBJ][64];
    int file_count = split_sources(srcline, files, MAX_OBJ);
    if (file_count <= 0) { puts("no input\n"); return 1; }

    /* Names typed at the prompt are relative to the shell's cwd. */
    for (int i = 0; i < file_count; i++) {
        char full[PATH_MAX_LEN];
        if (!path_fs(cwd_get(), files[i], full, sizeof(full))) {
            puts("bad path: "); puts(files[i]); putc('\n');
            return 1;
        }
        strncpy(files[i], full, 63);
        files[i][63] = 0;
    }

    char outpath[PATH_MAX_LEN];
    if (!path_fs(cwd_get(), outname, outpath, sizeof(outpath))) {
        puts("bad output name\n");
        return 1;
    }

    warn_count = 0;
    cur_file[0] = 0;

    /* Compare basenames: "src/stdio.c" is still the user's own stdio.c. */
    int has_stdio = 0;
    for (int i = 0; i < file_count; i++) {
        if (!strcmp(path_base(files[i]), "stdio.c")) has_stdio = 1;
    }

    int obj_count = 0;

    build_start_obj(&objs[obj_count++]);

    if (!has_stdio) {
        if (compile_obj("lib/stdio.c", &objs[obj_count++], 0) != 0) return 1;
    }

    for (int i = 0; i < file_count; i++) {
        if (obj_count >= MAX_OBJ) { puts("too many files\n"); return 1; }
        int skip_stdio = 1;
        if (!strcmp(path_base(files[i]), "stdio.c")) skip_stdio = 0;
        if (compile_obj(files[i], &objs[obj_count++], skip_stdio) != 0) return 1;
    }

    int text_len = 0, rodata_len = 0, data_len2 = 0, bss_len = 0;
    if (link_objects(objs, obj_count, text, &text_len, rodata_buf, &rodata_len, data_buf, &data_len2, &bss_len) != 0) return 1;

    int outsz = build_elf(outbuf, sizeof(outbuf), text, text_len, rodata_buf, rodata_len, data_buf, data_len2, bss_len);
    if (outsz < 0) { puts("build fail\n"); return 1; }

    if (sys_save(outpath, outbuf, outsz) != 0) { puts("save fail\n"); return 1; }
    if (warn_count > 0) {
        puts("warnings: ");
        print_dec(warn_count);
        putc('\n');
    }
    puts("ok\n");
    return 0;
}
