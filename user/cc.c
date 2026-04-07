#include "libc.h"
#include <stdint.h>

#define MAX_CODE 65536
#define MAX_RODATA 98304
#define MAX_DATA 32768
#define MAX_BSS 1048576
#define MAX_TOK 4096
#define MAX_NODE 4096
#define MAX_SYM 256
#define MAX_FUNC 64
#define MAX_FIX 512
#define MAX_LABEL 512
#define MAX_TYPE 512
#define MAX_STRUCT 64
#define MAX_FIELD 64
#define MAX_TYPEDEF 128
#define MAX_OBJ 8
#define MAX_OUT_TEXT (MAX_CODE * MAX_OBJ)
#define MAX_OUT_RODATA (MAX_RODATA * MAX_OBJ)
#define MAX_OUT_DATA (MAX_DATA * MAX_OBJ)

typedef enum { TY_INT, TY_FLOAT, TY_CHAR, TY_BOOL, TY_VOID, TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNION } TypeKind;

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
#define SRCBUF_MAX 196608
#define PPBUF_MAX 393216
#define INC_BUF_MAX 196608
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
    TOK_RETURN, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR,
    TOK_SWITCH, TOK_CASE, TOK_DEFAULT, TOK_BREAK, TOK_CONTINUE,
    TOK_TYPEDEF, TOK_STRUCT, TOK_UNION, TOK_EXTERN, TOK_ENUM,
    TOK_INT, TOK_FLOAT, TOK_KW_CHAR, TOK_BOOL, TOK_VOID, TOK_SIZEOF, TOK_UNSIGNED, TOK_SIGNED,
    TOK_CONST, TOK_VOLATILE, TOK_STATIC, TOK_AUTO, TOK_REGISTER, TOK_SHORT, TOK_LONG,
    TOK_DOT, TOK_LBRACK, TOK_RBRACK, TOK_COLON, TOK_ARROW, TOK_AMP, TOK_ELLIPSIS,
    TOK_OR, TOK_XOR, TOK_TILDE, TOK_MOD, TOK_SHL, TOK_SHR,
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
    struct Node *args[8];
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
    Type *params[8];
    int is_varargs;
    int declared;
} Func;

static const char *src;
static int pos;
static Token tok;
static char cur_file[64];
static char incbuf_pool[PP_MAX_DEPTH][INC_BUF_MAX];
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
static EnumConst enum_consts[256];
static int enum_const_len;

static Type type_int, type_float, type_char, type_bool, type_void;
static Type type_uint, type_uchar;

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
    ObjRelocLoc lreloc[MAX_FIX];
    int lreloc_len;
    ObjRelocSym sreloc[MAX_FIX];
    int sreloc_len;
} Obj;

/* large object list: keep out of stack */
static Obj objs[MAX_OBJ + 1];

typedef struct {
    int count;
    Type *types[8];
    char names[8][32];
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

enum { AF_STR = 1, AF_GLOB = 2 };

typedef struct { int break_label; int continue_label; } BreakCtx;
static BreakCtx break_stack[16];
static int break_depth;

typedef struct {
    int end_label;
    int dispatch_label;
    int default_label;
    int temp_offset;
    int case_count;
    int case_values[64];
    int case_labels[64];
} SwitchCtx;
static SwitchCtx switch_stack[8];
static int switch_depth;

static Field *find_field(StructDef *s, const char *name);

static void emit8(uint8_t b) { if (code_len < MAX_CODE) code[code_len++] = b; }
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
    if (cur_file[0]) { puts(cur_file); putc(':'); }
    print_dec(line); putc(':'); print_dec(col); puts(": ");
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

static int new_label(void) { return label_count++; }

static void set_label(int id) { labels_pos[id] = code_len; labels_def[id] = 1; }

static void emit_jmp_label(int id) {
    emit8(0xE9);
    int pos = code_len;
    emit32(0);
    fixups[fixup_len++] = (Fix){ pos, id };
}

static void emit_jcc_label(int cc, int id) {
    emit8(0x0F); emit8((uint8_t)cc);
    int pos = code_len;
    emit32(0);
    fixups[fixup_len++] = (Fix){ pos, id };
}

static void emit_call_label(int id) {
    emit8(0xE8);
    int pos = code_len;
    emit32(0);
    fixups[fixup_len++] = (Fix){ pos, id };
}

static void add_addr_fix(int pos, int kind, int index) {
    addr_fix[addr_fix_len++] = (AddrFix){ pos, kind, index };
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
            tok.end = pos;
            return;
        }
        while ((src[pos]>='0' && src[pos]<='9') || src[pos]=='.') {
            if (src[pos]=='.') has_dot = 1;
            pos++;
        }
        int len = pos - start;
        float f = 0.0f;
        int i = 0;
        while (i < len && src[start+i] != '.') { f = f*10.0f + (src[start+i]-'0'); i++; }
        if (i < len && src[start+i]=='.') {
            i++;
            float place = 0.1f;
            while (i < len) { f += (src[start+i]-'0')*place; place *= 0.1f; i++; }
        }
        if (has_dot) { tok.kind = TOK_FNUM; tok.fval = f; }
        else { tok.kind = TOK_NUM; tok.ival = (int)f; }
        tok.end = pos;
        return;
    }

    if (c=='"') {
        pos++;
        int i=0;
        while (src[pos] && src[pos] != '"' && i < (TOK_TEXT_MAX - 1)) {
            char ch = src[pos++];
            if (ch=='\\') {
                ch = src[pos++];
                if (ch=='n') ch='\n';
                else if (ch=='t') ch='\t';
            }
            tok.text[i++] = ch;
        }
        tok.text[i]=0;
        if (src[pos]=='"') pos++;
        tok.kind = TOK_STR;
        tok.end = pos;
        return;
    }

    if (c=='\'') {
        pos++;
        char ch = src[pos++];
        if (ch=='\\') {
            ch = src[pos++];
            if (ch=='n') ch='\n';
            else if (ch=='t') ch='\t';
        }
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
    case '+': tok.kind = TOK_PLUS; tok.end = pos; return;
    case '-':
        if (src[pos] == '>') { pos++; tok.kind = TOK_ARROW; tok.end = pos; return; }
        tok.kind = TOK_MINUS; tok.end = pos; return;
    case '*': tok.kind = TOK_MUL; tok.end = pos; return;
    case '/': tok.kind = TOK_DIV; tok.end = pos; return;
    case '%': tok.kind = TOK_MOD; tok.end = pos; return;
    case '&':
        if (src[pos] == '&') { pos++; tok.kind = TOK_LAND; tok.end = pos; return; }
        tok.kind = TOK_AMP; tok.end = pos; return;
    case '|':
        if (src[pos] == '|') { pos++; tok.kind = TOK_LOR; tok.end = pos; return; }
        tok.kind = TOK_OR; tok.end = pos; return;
    case '^': tok.kind = TOK_XOR; tok.end = pos; return;
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
        if (src[pos]=='<') { pos++; tok.kind = TOK_SHL; }
        else if (src[pos]=='=') { pos++; tok.kind = TOK_LE; }
        else tok.kind = TOK_LT;
        tok.end = pos;
        return;
    case '>':
        if (src[pos]=='>') { pos++; tok.kind = TOK_SHR; }
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
static int is_numeric(Type *t) { return is_int(t) || is_float(t) || is_char(t) || is_bool(t); }
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
    memset(&type_void, 0, sizeof(type_void));
    type_void.kind = TY_VOID; type_void.size = 0; type_void.align = 1;

    /* builtin typedef: va_list -> char* */
    add_typedef("va_list", type_ptr(&type_char));
}

static Sym *find_local(const char *name) {
    for (int i=0;i<locals_len;i++) if (!strcmp(locals[i].name,name)) return &locals[i];
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
static Node *parse_bitand(void);
static Node *parse_bitxor(void);
static Node *parse_bitor(void);
static Node *parse_land(void);
static Node *parse_lor(void);

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
    if (consume(TOK_INT)) return uns ? &type_uint : &type_int;
    if (consume(TOK_FLOAT)) return &type_float;
    if (consume(TOK_KW_CHAR)) return uns ? &type_uchar : &type_char;
    if (consume(TOK_BOOL)) return &type_bool;
    if (consume(TOK_VOID)) return &type_void;
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
    if (uns || longc || shortc || saw_spec) return uns ? &type_uint : &type_int;
    if (tok.kind == TOK_ID) {
        Typedef *td = find_typedef(tok.text);
        if (td) { next_token(); return td->type; }
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
    while (consume(TOK_MUL)) base = type_ptr(base);
    if (consume(TOK_LPAREN)) {
        int inner_ptr = 0;
        while (consume(TOK_MUL)) { inner_ptr++; }
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
    while (consume(TOK_LBRACK)) {
        if (tok.kind != TOK_NUM) { error_here("array size\n"); }
        int len = tok.ival;
        next_token();
        expect(TOK_RBRACK);
        base = type_array(base, len);
    }
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
        sd = find_struct(sname);
        if (!sd) { error_here("unknown struct\n"); }
    }
    return is_union ? type_union(sd) : type_struct(sd);
}

static Type *parse_type_name(void) {
    Type *base = parse_type_spec();
    if (!base) { puts("type?\n"); sys_exit(1); }
    while (consume(TOK_MUL)) base = type_ptr(base);
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
        if (!f) { puts("no field\n"); sys_exit(1); }
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
                        if (call->argc >= 8) { puts("too many args\n"); sys_exit(1); }
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
        break;
    }
    return n;
}

static Node *parse_unary(void) {
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

static void conv_stack_to_int(int disp) {
    emit8(0xD9); emit8(0x84); emit8(0x24); emit32((uint32_t)disp); /* fld dword [esp+disp] */
    emit8(0xDB); emit8(0x9C); emit8(0x24); emit32((uint32_t)disp); /* fistp dword [esp+disp] */
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
    if (!strcmp(n->name, "sys_udp_send")) { if (n->argc != 4) { puts("sys_udp_send args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(31, 4); }
    if (!strcmp(n->name, "sys_udp_recv")) { if (n->argc != 4) { puts("sys_udp_recv args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(32, 4); }
    if (!strcmp(n->name, "sys_net_myip"))    { if (n->argc != 0) { puts("sys_net_myip args\n");    sys_exit(1); } return emit_syscall_builtin(33, 0); }
    if (!strcmp(n->name, "sys_udp_recv_nb")) { if (n->argc != 4) { puts("sys_udp_recv_nb args\n"); sys_exit(1); } gen_args_rev(n); return emit_syscall_builtin(34, 4); }
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
    for (int i = n->argc - 1; i >= 0; i--) {
        Type *t = gen_expr(n->args[i]);
        Type *pt = (i < f->param_count) ? f->params[i] : &type_int;
        if (is_record(pt) || is_array(pt)) { puts("param struct/array\n"); sys_exit(1); }
        if (is_float(pt) && !is_float(t)) conv_stack_to_float(0);
        if (!is_float(pt) && is_float(t)) conv_stack_to_int(0);
    }
    emit8(0xE8);
    int pos = code_len;
    emit32(0);
    const char *lname = f->link_name[0] ? f->link_name : n->name;
    add_sym_fix(pos, RELOC_REL32, lname);
    if (n->argc > 0) emit_add_esp(4 * n->argc);
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
        if (!f) { puts("no field\n"); sys_exit(1); }
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
        if (is_record(s->type)) { puts("struct rvalue\n"); sys_exit(1); }
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
        if (is_record(t) || is_array(t)) { puts("struct rvalue\n"); sys_exit(1); }
        emit_load_from_addr(t);
        return promote(t);
    }
    case ND_INDEX:
    case ND_MEMBER: {
        Type *t = gen_addr(n);
        if (is_array(t)) {
            return type_ptr(t->base);
        }
        if (is_record(t)) { puts("struct rvalue\n"); sys_exit(1); }
        emit_load_from_addr(t);
        return promote(t);
    }
    case ND_ASSIGN: {
        Type *lt = gen_addr(n->lhs);
        if (is_record(lt) || is_array(lt)) { puts("assign struct/array\n"); sys_exit(1); }
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
        Type *tt = infer_type(t);
        Type *tf = infer_type(f);
        if (is_record(tt) || is_array(tt) || is_record(tf) || is_array(tf)) { puts("ternary type\n"); sys_exit(1); }
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
                emit8(0xD9); emit8(0x44); emit8(0x24); emit8(0x04); /* fld [esp+4] */
                emit8(0xD9); emit8(0x04); emit8(0x24); /* fld [esp] */
                emit8(0xDE); emit8(0xD9); /* fcompp */
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
            Sym *s = add_local(name, ty);
            if (consume(TOK_ASSIGN)) {
                if (is_array(ty) || is_record(ty)) { puts("init array/struct\n"); sys_exit(1); }
                Node *e = parse_expr();
                emit_load_addr(s);
                Type *t = gen_expr(e);
                if (is_bool(ty)) {
                    emit_boolify(t);
                } else {
                    if (is_float(ty) && !is_float(t)) conv_stack_to_float(0);
                    if (!is_float(ty) && is_float(t)) conv_stack_to_int(0);
                }
                emit_store_to_addr(ty);
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
        while (!consume(TOK_RBRACE)) gen_stmt();
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
    if (consume(TOK_WHILE)) {
        int l_start = new_label();
        int l_end = new_label();
        set_label(l_start);
        expect(TOK_LPAREN);
        Node *cond = parse_expr();
        expect(TOK_RPAREN);
        break_stack[break_depth++] = (BreakCtx){ l_end, l_start };
        gen_cond(cond, l_end);
        gen_stmt();
        break_depth--;
        emit_jmp_label(l_start);
        set_label(l_end);
        return;
    }
    if (consume(TOK_FOR)) {
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
        if (!consume(TOK_SEMI)) { cond = parse_expr(); expect(TOK_SEMI); }
        if (dbg_for) { puts("for-after-cond: "); print_tok_desc(); putc('\n'); }
        Node *post = 0;
        if (!consume(TOK_RPAREN)) { post = parse_expr(); expect(TOK_RPAREN); }

        int l_start = new_label();
        int l_end = new_label();
        int l_post = new_label();
        set_label(l_start);
        if (cond) gen_cond(cond, l_end);
        break_stack[break_depth++] = (BreakCtx){ l_end, l_post };
        gen_stmt();
        break_depth--;
        set_label(l_post);
        if (post) { gen_expr(post); emit_add_esp(4); }
        emit_jmp_label(l_start);
        set_label(l_end);
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
        break_stack[break_depth++] = (BreakCtx){ end, -1 };
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
                int val = 0;
                int neg = 0;
                if (consume(TOK_MINUS)) neg = 1;
                if (tok.kind == TOK_NUM || tok.kind == TOK_CHAR) {
                    val = tok.ival;
                    next_token();
                } else {
                    puts("case const\n"); sys_exit(1);
                }
                if (neg) val = -val;
                expect(TOK_COLON);
                if (sw->case_count >= 64) { puts("case ovf\n"); sys_exit(1); }
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

static ParamInfo parse_param_list(void) {
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
                    if (is_record(pty)) { error_here("param struct\n"); }
                    if (pi.count >= 8) { puts("param ovf\n"); sys_exit(1); }
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
    return pi;
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

    for (int i = 0; i < pi->count; i++) {
        int offset = 8 + i * 4;
        add_param(pi->names[i], pi->types[i], offset);
    }

    set_label(f->label);
    /* prologue */
    emit8(0x55); /* push ebp */
    emit8(0x89); emit8(0xE5); /* mov ebp, esp */
    emit8(0x81); emit8(0xEC); int patch = code_len; emit32(0); /* sub esp, imm */

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
    in_func = 0;
}

static void parse_global(Type *ty, const char *name, int is_static) {
    Sym *exist = find_global(name);
    if (exist && !exist->is_extern) { puts("dup global\n"); sys_exit(1); }
    if (exist && is_static) { puts("extern/static\n"); sys_exit(1); }
    if (consume(TOK_ASSIGN)) {
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
            if (e->kind == ND_NUM) {
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
                puts("global init\n"); sys_exit(1);
            }
        }
        expect(TOK_SEMI);
        return;
    }
    Sym *s = exist ? define_global_bss_existing(exist, ty) : add_global_bss(name, ty);
    if (is_static) sym_set_link_name(s, 1);
    expect(TOK_SEMI);
}

static void parse_program(void) {
    next_token();
    while (tok.kind != TOK_EOF) {
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
        char name[32];
        Type *ty = parse_declarator(base, name);
        if (tok.kind == TOK_LPAREN) {
            if (is_array(ty) || is_record(ty)) { error_here("func return\n"); }
            ParamInfo pi = parse_param_list();
            if (tok.kind == TOK_SEMI) {
                next_token();
                add_func_decl(ty, name, &pi, ds.is_static);
            } else {
                parse_function_def(ty, name, &pi, ds.is_static);
            }
        } else {
            if (is_extern) {
                if (consume(TOK_ASSIGN)) { puts("extern init\n"); sys_exit(1); }
                add_global_decl(name, ty);
                expect(TOK_SEMI);
            } else {
                parse_global(ty, name, ds.is_static);
            }
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
    *(uint32_t*)(p+24)=header; /* entry */
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
    *(uint32_t*)(p+8)=0;
    *(uint32_t*)(p+12)=0;
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
    if (pp_expr_number(&(PPExpr){ b, pp }, &v)) return v;
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
                    int n = sys_load(name, incbuf, INC_BUF_MAX - 1);
                    if (n <= 0) { puts("include fail\n"); return -1; }
                    incbuf[n] = 0;
                    int n2 = pp_process(pp, incbuf, name, out + out_len, out_max - out_len - 1, depth + 1, skip_stdio);
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
    if (n <= 0) { puts("read fail\n"); return -1; }
    srcbuf[n] = 0;

    strncpy(cur_file, srcname, 63);
    cur_file[63] = 0;
    current_unit_id = unit_counter++;
    static_sym_id = 0;

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
        if (addr_fix[i].kind == AF_STR) {
            if (obj->lreloc_len >= MAX_FIX) { puts("reloc ovf\n"); return -1; }
            ObjRelocLoc *r = &obj->lreloc[obj->lreloc_len++];
            r->section = SEC_TEXT;
            r->offset = pos;
            r->type = RELOC_ABS32;
            r->target_section = SEC_RODATA;
            r->addend = addr_fix[i].index;
        } else if (addr_fix[i].kind == AF_GLOB) {
            if (obj->sreloc_len >= MAX_FIX) { puts("reloc ovf\n"); return -1; }
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
        if (obj->sreloc_len >= MAX_FIX) { puts("reloc ovf\n"); return -1; }
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
    /* set FPU control word to truncate (C casts truncate toward zero) */
    obj->text[p++] = 0x83; obj->text[p++] = 0xEC; obj->text[p++] = 0x04; /* sub esp, 4 */
    obj->text[p++] = 0x66; obj->text[p++] = 0xC7; obj->text[p++] = 0x04; obj->text[p++] = 0x24; /* mov word [esp], imm16 */
    obj->text[p++] = 0x7F; obj->text[p++] = 0x0F; /* 0x0F7F */
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
    int text_base = header;
    int rodata_base = header + text_total;
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
    static uint8_t outbuf[131072];
    static uint8_t text[MAX_OUT_TEXT];
    static uint8_t rodata_buf[MAX_OUT_RODATA];
    static uint8_t data_buf[MAX_OUT_DATA];

    puts("cc v7 (syscall args fix)\n");
    puts("source files: ");
    readline(srcline, sizeof(srcline));
    puts("output file: ");
    readline(outname, sizeof(outname));

    char files[MAX_OBJ][64];
    int file_count = split_sources(srcline, files, MAX_OBJ);
    if (file_count <= 0) { puts("no input\n"); return 1; }

    warn_count = 0;
    cur_file[0] = 0;

    int has_stdio = 0;
    for (int i = 0; i < file_count; i++) {
        if (!strcmp(files[i], "stdio.c")) has_stdio = 1;
    }

    int obj_count = 0;

    build_start_obj(&objs[obj_count++]);

    if (!has_stdio) {
        if (compile_obj("stdio.c", &objs[obj_count++], 0) != 0) return 1;
    }

    for (int i = 0; i < file_count; i++) {
        if (obj_count >= MAX_OBJ) { puts("too many files\n"); return 1; }
        int skip_stdio = 1;
        if (!strcmp(files[i], "stdio.c")) skip_stdio = 0;
        if (compile_obj(files[i], &objs[obj_count++], skip_stdio) != 0) return 1;
    }

    int text_len = 0, rodata_len = 0, data_len2 = 0, bss_len = 0;
    if (link_objects(objs, obj_count, text, &text_len, rodata_buf, &rodata_len, data_buf, &data_len2, &bss_len) != 0) return 1;

    int outsz = build_elf(outbuf, sizeof(outbuf), text, text_len, rodata_buf, rodata_len, data_buf, data_len2, bss_len);
    if (outsz < 0) { puts("build fail\n"); return 1; }

    if (sys_save(outname, outbuf, outsz) != 0) { puts("save fail\n"); return 1; }
    if (warn_count > 0) {
        puts("warnings: ");
        print_dec(warn_count);
        putc('\n');
    }
    puts("ok\n");
    return 0;
}
