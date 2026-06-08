/* ============================================================================
 * keel.c  —  The Keel reference interpreter (Stage 0 bootstrap oracle)
 *
 * Per the Keel design specification (§9): "Keel is bootstrapped: a reference
 * interpreter in C brings the language up, then the compiler is rewritten in
 * Keel itself (self-hosting) and the C interpreter is retired except as a
 * reference oracle."
 *
 * This file is that reference interpreter. It implements a coherent, runnable
 * subset of Keel large enough to (a) run the spec's flagship examples and
 * (b) host Keel-in-Keel tooling (the first link of the self-hosting chain).
 *
 * Faithful implementations: significant indentation + one canonical layout,
 * immutable-by-default bindings with `mut`, checked integer arithmetic (no
 * silent wraparound), exact `decimal`, no-null `T?` with `?.`/`??`, structs
 * /enums/pattern-matching, generics-by-structure, the `Fail<E>` effect with
 * `?` propagation and condition/restart `execute`/`handle`/`resume` (one-shot,
 * in-scope, via the libhandler setjmp/longjmp technique), capability-passing
 * (`System`), context-typed strings (`sql"..."` etc. — concatenated plain
 * strings cannot reach a sink), `Untrusted<T>`, `Secret<T>` with `reveal`,
 * first-class `test`/`test prop` with a runner + shrinking, and `comptime`.
 *
 * Pragmatic stand-ins (documented in DESIGN.md, honest per spec §11):
 *   - Refinement predicates (`where > 0`) are checked at runtime rather than
 *     discharged by an SMT solver. Same guarantee surface, decidable cost.
 *   - Effect resumption is one-shot / in-scope (the common, safe case).
 *   - Backend is a tree-walking interpreter (Stage 0), not LLVM AOT.
 * ==========================================================================*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <setjmp.h>
#include <math.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>

/* C5: the context-string escapers (the injection boundary) live in ONE place,
 * shared verbatim with the runtime that compiled Keel programs link against. */
#include "keel_escape.h"

/* ------------------------------------------------------------------ memory */
/* Stage-0 strategy: track every allocation, free all at process exit. The
 * interpreter itself is C; the *language* it implements is GC-free. Programs
 * are short-lived, so this is leak-free at exit and avoids GC complexity. */
typedef struct Alloc { struct Alloc *next; } Alloc;
static Alloc *g_allocs = NULL;
static void *xalloc(size_t n) {
    Alloc *a = (Alloc *)calloc(1, sizeof(Alloc) + n);
    if (!a) { fprintf(stderr, "keel: out of memory\n"); exit(70); }
    a->next = g_allocs; g_allocs = a;
    return (void *)(a + 1);
}
static char *xstrndup(const char *s, size_t n) {
    char *p = (char *)xalloc(n + 1);
    memcpy(p, s, n); p[n] = 0; return p;
}
static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
static void free_all(void) {
    Alloc *a = g_allocs;
    while (a) { Alloc *n = a->next; free(a); a = n; }
    g_allocs = NULL;
}

/* growable string buffer */
typedef struct { char *data; size_t len, cap; } Buf;
static void buf_init(Buf *b){ b->data=(char*)xalloc(16); b->len=0; b->cap=16; b->data[0]=0; }
static void buf_putn(Buf *b, const char *s, size_t n){
    if (b->len + n + 1 > b->cap){
        size_t nc = b->cap*2; while (nc < b->len+n+1) nc*=2;
        char *nd = (char*)xalloc(nc); memcpy(nd, b->data, b->len); b->data=nd; b->cap=nc;
    }
    memcpy(b->data+b->len, s, n); b->len+=n; b->data[b->len]=0;
}
static void buf_puts(Buf *b, const char *s){ buf_putn(b,s,strlen(s)); }
static void buf_putc(Buf *b, char c){ buf_putn(b,&c,1); }
static void buf_printf(Buf *b, const char *fmt, ...){
    char tmp[512]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < (int)sizeof tmp) buf_putn(b, tmp, (size_t)n);
    else { char *big=(char*)xalloc(n+1); va_start(ap,fmt); vsnprintf(big,n+1,fmt,ap); va_end(ap); buf_putn(b,big,n); }
}

/* growable pointer vector */
typedef struct { void **items; int len, cap; } Vec;
static void vec_init(Vec *v){ v->items=NULL; v->len=0; v->cap=0; }
static void vec_push(Vec *v, void *p){
    if (v->len == v->cap){
        int nc = v->cap ? v->cap*2 : 4;
        void **ni = (void**)xalloc(sizeof(void*)*nc);
        if (v->items) memcpy(ni, v->items, sizeof(void*)*v->len);
        v->items = ni; v->cap = nc;
    }
    v->items[v->len++] = p;
}

/* ============================================================= source spans
 * C1: a span is a first-class value — a file id plus a byte range [lo,hi) into
 * that file's source. Line/column are computed lazily from a per-file table of
 * line-start offsets, so a diagnostic can underline an entire sub-expression
 * rather than point at a single column. Tokens and AST nodes both carry spans;
 * the Keel-side compiler reproduces the same representation. */
typedef struct {
    char  *name;          /* interned file name */
    char  *text;          /* full source (owned) */
    int    len;
    int   *line_starts;   /* byte offset of the first char on each 1-based line */
    int    nlines;
} SrcFile;

#define MAX_SRC_FILES 256
static SrcFile g_srcs[MAX_SRC_FILES];
static int g_nsrcs = 0;

typedef struct { int file_id; int lo; int hi; } Span;

/* Register a source file, building its line-start table once. Returns file id. */
static int src_register(const char *name, const char *text){
    if (g_nsrcs >= MAX_SRC_FILES){ fprintf(stderr,"keel: too many source files\n"); exit(70); }
    int id = g_nsrcs++;
    SrcFile *f = &g_srcs[id];
    f->name = xstrdup(name);
    f->len  = (int)strlen(text);
    f->text = xstrndup(text, f->len);
    /* line_starts[1] = 0; one entry per line, plus sentinel */
    Vec ls; vec_init(&ls);
    vec_push(&ls, (void*)(intptr_t)0);              /* index 0 unused */
    vec_push(&ls, (void*)(intptr_t)0);              /* line 1 starts at 0 */
    for (int i = 0; i < f->len; i++)
        if (f->text[i] == '\n') vec_push(&ls, (void*)(intptr_t)(i+1));
    f->nlines = ls.len - 1;
    f->line_starts = (int*)xalloc(sizeof(int)*ls.len);
    for (int i = 0; i < ls.len; i++) f->line_starts[i] = (int)(intptr_t)ls.items[i];
    return id;
}

/* Pure function: byte offset → (1-based line, 1-based column). */
static void src_line_col(int file_id, int off, int *line, int *col){
    SrcFile *f = &g_srcs[file_id];
    int lo = 1, hi = f->nlines, ln = 1;
    while (lo <= hi){
        int mid = (lo+hi)/2;
        if (f->line_starts[mid] <= off){ ln = mid; lo = mid+1; }
        else hi = mid-1;
    }
    *line = ln;
    *col  = off - f->line_starts[ln] + 1;
}

/* Render the offending source line with a caret/underline under [lo,hi). */
static void src_render_excerpt(FILE *out, int file_id, int lo, int hi){
    if (file_id < 0 || file_id >= g_nsrcs) return;
    SrcFile *f = &g_srcs[file_id];
    if (lo < 0) lo = 0; if (lo > f->len) lo = f->len;
    if (hi < lo) hi = lo; if (hi > f->len) hi = f->len;
    int line, col; src_line_col(file_id, lo, &line, &col);
    int ls = f->line_starts[line];
    int le = ls; while (le < f->len && f->text[le] != '\n') le++;
    fprintf(out, "  %4d | %.*s\n", line, le-ls, f->text+ls);
    fprintf(out, "       | ");
    for (int i = 0; i < col-1; i++) fputc(' ', out);
    int width = hi - lo; if (width < 1) width = 1;
    if (lo + width > le) width = le - lo; if (width < 1) width = 1;
    for (int i = 0; i < width; i++) fputc('^', out);
    fputc('\n', out);
}

/* ----------------------------------------------------- diagnostics engine
 * 1.1: errors are accumulated with a stable mnemonic CODE and a span, then
 * rendered together with caret excerpts — never `exit()` on the first one.
 * Negative conformance cases (C2) assert on the code, not the prose. */
typedef struct { const char *code; int file_id; int lo; int hi; char *msg; } Diag;
static Vec  g_diags;                 /* Diag* */
static int  g_diag_cap_reached = 0;
#define MAX_DIAGS 25

static void diag_init(void){ vec_init(&g_diags); g_diag_cap_reached = 0; }

static void diag_add(const char *code, int file_id, int lo, int hi, const char *fmt, ...){
    if (g_diags.len >= MAX_DIAGS){ g_diag_cap_reached = 1; return; }
    Diag *d = (Diag*)xalloc(sizeof(Diag));
    d->code = code; d->file_id = file_id; d->lo = lo; d->hi = hi;
    char tmp[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    d->msg = xstrdup(tmp);
    vec_push(&g_diags, d);
}

/* Print all accumulated diagnostics. Returns the number reported. */
static int diag_flush(void){
    for (int i = 0; i < g_diags.len; i++){
        Diag *d = (Diag*)g_diags.items[i];
        int line, col; 
        if (d->file_id >= 0) src_line_col(d->file_id, d->lo, &line, &col);
        else { line = 0; col = 0; }
        const char *fname = d->file_id >= 0 ? g_srcs[d->file_id].name : "<input>";
        fprintf(stderr, "%s:%d:%d: error[%s]: %s\n", fname, line, col, d->code, d->msg);
        src_render_excerpt(stderr, d->file_id, d->lo, d->hi);
    }
    if (g_diag_cap_reached)
        fprintf(stderr, "... (further errors suppressed; fix the above and re-run)\n");
    return g_diags.len;
}

/* ================================================================== tokens */
typedef enum {
    T_EOF, T_NEWLINE, T_INDENT, T_DEDENT,
    T_INT, T_DECIMAL, T_FLOAT, T_STRING, T_IDENT,
    /* punctuation / operators */
    T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_LBRACE, T_RBRACE,
    T_COMMA, T_COLON, T_SEMI, T_DOT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_POW,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_INC, T_DEC,
    T_AMP, T_AMPMUT, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR,
    T_QUESTION, T_QDOT, T_QQ, T_ARROW,
    /* keywords */
    T_AND, T_ASSERT, T_CHECK, T_CLASS, T_COMPTIME, T_CONTINUE, T_DEF, T_DERIVE,
    T_ELIF, T_ELSE, T_ENUM, T_EXECUTE, T_EXTERN, T_HANDLE, T_IF, T_IMPORT,
    T_INTERFACE, T_IS, T_LOOP, T_MODULE, T_MUT, T_NONE, T_NOT, T_OR, T_PARALLEL,
    T_PRIVATE, T_PROP, T_PUBLIC, T_REGION, T_REFLECT, T_RESUME, T_RETURN,
    T_SCOPE, T_SHARED, T_SPAWN, T_STRUCT, T_TEST, T_THROUGH, T_TILL, T_TOTAL,
    T_TYPE, T_UNSAFE, T_WHERE, T_BREAK, T_TRUE, T_FALSE
} TokKind;

typedef struct {
    TokKind kind;
    const char *start;   /* into source */
    int len;
    int line;
    int file_id;         /* C1: span — which source */
    int lo, hi;          /* C1: span — byte range [lo,hi) into that source */
    char *strlit;        /* decoded string contents (with #interp markers) */
    char *tag;           /* context-string prefix, e.g. "sql"; or NULL */
} Token;

typedef struct {
    const char *src;
    const char *cur;
    int line;
    const char *filename;
    int file_id;         /* C1 */
    /* indentation engine */
    int indents[256];
    int indent_top;
    int pending_dedents;
    bool at_line_start;
    int paren_depth;     /* inside (), [], {} → ignore newlines */
    Vec tokens;          /* Token* */
    bool emitted_eof;
} Lexer;

static Token *new_tok(Lexer *L, TokKind k, const char *s, int len){
    Token *t = (Token*)xalloc(sizeof(Token));
    t->kind=k; t->start=s; t->len=len; t->line=L->line; t->strlit=NULL; t->tag=NULL;
    t->file_id = L->file_id;
    t->lo = (int)(s - L->src);
    t->hi = t->lo + (len>0?len:0);
    return t;
}

typedef struct { const char *kw; TokKind k; } KwEnt;
static KwEnt KEYWORDS[] = {
    {"and",T_AND},{"assert",T_ASSERT},{"break",T_BREAK},{"check",T_CHECK},
    {"class",T_CLASS},{"comptime",T_COMPTIME},{"continue",T_CONTINUE},
    {"def",T_DEF},{"derive",T_DERIVE},{"elif",T_ELIF},{"else",T_ELSE},
    {"enum",T_ENUM},{"execute",T_EXECUTE},{"extern",T_EXTERN},{"false",T_FALSE},
    {"handle",T_HANDLE},{"if",T_IF},{"import",T_IMPORT},{"interface",T_INTERFACE},
    {"is",T_IS},{"loop",T_LOOP},{"module",T_MODULE},{"mut",T_MUT},{"none",T_NONE},
    {"not",T_NOT},{"or",T_OR},{"parallel",T_PARALLEL},{"private",T_PRIVATE},
    {"prop",T_PROP},{"public",T_PUBLIC},{"region",T_REGION},{"reflect",T_REFLECT},
    {"resume",T_RESUME},{"return",T_RETURN},{"scope",T_SCOPE},{"shared",T_SHARED},
    {"spawn",T_SPAWN},{"struct",T_STRUCT},{"test",T_TEST},{"through",T_THROUGH},
    {"till",T_TILL},{"total",T_TOTAL},{"true",T_TRUE},{"type",T_TYPE},
    {"unsafe",T_UNSAFE},{"where",T_WHERE},{NULL,0}
};

/* Stable lexer error codes (asserted by negative conformance cases). */
#define E_LEX_CHAR    "lex.char"
#define E_LEX_STRING  "lex.string"
#define E_LEX_INDENT  "lex.indent"
#define E_LEX_DEDENT  "lex.dedent"

static jmp_buf g_lex_recover;
static void lex_error(Lexer *L, const char *code, const char *msg){
    int lo = (int)(L->cur - L->src);
    diag_add(code, L->file_id, lo, lo+1, "%s", msg);
    /* recover: discard the rest of the current line, then resynchronize */
    while (*L->cur && *L->cur != '\n') L->cur++;
    longjmp(g_lex_recover, 1);
}

/* Emit DEDENTs/INDENT according to a new line's indentation. */
static void handle_indent(Lexer *L, int width){
    int top = L->indents[L->indent_top];
    if (width > top){
        if (L->indent_top+1 >= 256) lex_error(L, E_LEX_INDENT, "indentation too deep");
        L->indents[++L->indent_top] = width;
        vec_push(&L->tokens, new_tok(L, T_INDENT, L->cur, 0));
    } else if (width < top){
        while (L->indent_top > 0 && L->indents[L->indent_top] > width){
            L->indent_top--;
            vec_push(&L->tokens, new_tok(L, T_DEDENT, L->cur, 0));
        }
        if (L->indents[L->indent_top] != width)
            lex_error(L, E_LEX_DEDENT, "inconsistent dedent (does not match any open indentation level)");
    }
}

static TokKind ident_kind(const char *s, int len){
    for (int i=0; KEYWORDS[i].kw; i++)
        if ((int)strlen(KEYWORDS[i].kw)==len && memcmp(KEYWORDS[i].kw,s,len)==0)
            return KEYWORDS[i].k;
    return T_IDENT;
}

/* Scan one string literal. `tag` is the optional context prefix (sql/...). The
 * raw lexeme (including interior) is stored; interpolation is parsed later. */
static Token *scan_string(Lexer *L, char *tag){
    const char *start = L->cur;   /* at opening quote */
    L->cur++;                     /* skip " */
    Buf b; buf_init(&b);
    while (*L->cur && *L->cur != '"'){
        if (*L->cur == '\\'){
            char c = L->cur[1];
            switch(c){
                case 'n': buf_putc(&b,'\n'); break;
                case 't': buf_putc(&b,'\t'); break;
                case 'r': buf_putc(&b,'\r'); break;
                case '\\': buf_putc(&b,'\\'); break;
                case '"': buf_putc(&b,'"'); break;
                case '#': buf_putc(&b,'\1'); buf_putc(&b,'#'); break; /* literal # escaped */
                default: buf_putc(&b,c); break;
            }
            L->cur += 2;
        } else {
            if (*L->cur=='\n') L->line++;
            buf_putc(&b, *L->cur++);
        }
    }
    if (*L->cur != '"') lex_error(L, E_LEX_STRING, "unterminated string literal");
    L->cur++; /* closing quote */
    Token *t = new_tok(L, T_STRING, start, (int)(L->cur-start));
    t->strlit = b.data;   /* decoded contents (with #interp markers intact) */
    t->tag = tag;         /* context prefix (sql/shell/html/url) or NULL */
    return t;
}

static void lex(Lexer *L){
    L->indent_top = 0; L->indents[0] = 0;
    L->at_line_start = true; L->paren_depth = 0; L->line = 1;
    volatile bool any_token_on_line = false;

    while (*L->cur){
        if (setjmp(g_lex_recover)){
            /* a lex error skipped to end-of-line; resynchronize cleanly */
            any_token_on_line = false;
            L->paren_depth = 0;
            if (*L->cur == '\n'){ L->cur++; L->line++; }
            L->at_line_start = true;
            continue;
        }
        /* line-start: measure indentation (only when not inside brackets) */
        if (L->at_line_start && L->paren_depth == 0){
            const char *p = L->cur; int width = 0;
            while (*p==' '||*p=='\t'){ width += (*p=='\t')?4:1; p++; }
            if (*p=='\n'){ L->cur=p+1; L->line++; continue; }      /* blank line */
            if (*p=='#'){ while (*p && *p!='\n') p++; if(*p){p++;L->line++;} L->cur=p; continue; } /* comment line */
            if (*p==0) break;
            L->cur = p;
            handle_indent(L, width);
            L->at_line_start = false;
            any_token_on_line = false;
        }

        char c = *L->cur;
        if (c==' '||c=='\t'){ L->cur++; continue; }
        if (c=='#'){ while (*L->cur && *L->cur!='\n') L->cur++; continue; } /* comment */
        if (c=='\n'){
            L->cur++; 
            if (L->paren_depth==0 && any_token_on_line){
                vec_push(&L->tokens, new_tok(L, T_NEWLINE, L->cur, 0));
            }
            L->line++;
            L->at_line_start = true;
            continue;
        }

        const char *s = L->cur;
        any_token_on_line = true;

        /* numbers */
        if (isdigit((unsigned char)c)){
            const char *q = L->cur; bool isfloat=false, isdec=false;
            while (isdigit((unsigned char)*q)||*q=='_') q++;
            if (*q=='.' && isdigit((unsigned char)q[1])){
                isdec=true; q++; while (isdigit((unsigned char)*q)||*q=='_') q++;
            }
            if (*q=='e'||*q=='E'){ isfloat=true; q++; if(*q=='+'||*q=='-')q++; while(isdigit((unsigned char)*q))q++; }
            /* suffix f = float, d/m = decimal */
            TokKind nk = T_INT;
            if (isfloat) nk = T_FLOAT;
            else if (isdec) nk = T_DECIMAL;     /* decimals are exact by default */
            if (*q=='f'){ nk=T_FLOAT; q++; }
            Token *t = new_tok(L, nk, s, (int)(q-s)); L->cur=q;
            vec_push(&L->tokens, t); continue;
        }

        /* identifiers / keywords / context-strings */
        if (isalpha((unsigned char)c) || c=='_'){
            const char *q = L->cur;
            while (isalnum((unsigned char)*q)||*q=='_') q++;
            int len=(int)(q-s);
            /* context string: ident immediately followed by " */
            if (*q=='"'){
                char *tag = xstrndup(s, len);
                L->cur = q;
                vec_push(&L->tokens, scan_string(L, tag));
                continue;
            }
            L->cur=q;
            TokKind k = ident_kind(s, len);
            vec_push(&L->tokens, new_tok(L, k, s, len));
            continue;
        }

        /* plain string */
        if (c=='"'){ vec_push(&L->tokens, scan_string(L, NULL)); continue; }

        /* operators (longest-match) */
        #define ADV(n) (L->cur+=(n))
        #define TOK1(k) do{ vec_push(&L->tokens,new_tok(L,k,s,1)); ADV(1);}while(0)
        #define TOK2(k) do{ vec_push(&L->tokens,new_tok(L,k,s,2)); ADV(2);}while(0)
        char d = L->cur[1];
        switch(c){
            case '(': L->paren_depth++; TOK1(T_LPAREN); break;
            case ')': if(L->paren_depth>0)L->paren_depth--; TOK1(T_RPAREN); break;
            case '[': L->paren_depth++; TOK1(T_LBRACK); break;
            case ']': if(L->paren_depth>0)L->paren_depth--; TOK1(T_RBRACK); break;
            case '{': L->paren_depth++; TOK1(T_LBRACE); break;
            case '}': if(L->paren_depth>0)L->paren_depth--; TOK1(T_RBRACE); break;
            case ',': TOK1(T_COMMA); break;
            case ':': TOK1(T_COLON); break;
            case ';': TOK1(T_SEMI); break;
            case '.': TOK1(T_DOT); break;
            case '+': if(d=='+')TOK2(T_INC); else if(d=='=')TOK2(T_PLUSEQ); else TOK1(T_PLUS); break;
            case '-': if(d=='-')TOK2(T_DEC); else if(d=='=')TOK2(T_MINUSEQ); else if(d=='>')TOK2(T_ARROW); else TOK1(T_MINUS); break;
            case '*': if(d=='*')TOK2(T_POW); else if(d=='=')TOK2(T_STAREQ); else TOK1(T_STAR); break;
            case '/': if(d=='=')TOK2(T_SLASHEQ); else TOK1(T_SLASH); break;
            case '%': if(d=='=')TOK2(T_PERCENTEQ); else TOK1(T_PERCENT); break;
            case '=': if(d=='=')TOK2(T_EQ); else TOK1(T_ASSIGN); break;
            case '!': if(d=='=')TOK2(T_NE); else lex_error(L,E_LEX_CHAR,"unexpected '!' (did you mean '!='?)"); break;
            case '<': if(d=='=')TOK2(T_LE); else if(d=='<')TOK2(T_SHL); else TOK1(T_LT); break;
            case '>': if(d=='=')TOK2(T_GE); else if(d=='>')TOK2(T_SHR); else TOK1(T_GT); break;
            case '&':
                if (d=='m' && L->cur[2]=='u' && L->cur[3]=='t' &&
                    !(isalnum((unsigned char)L->cur[4])||L->cur[4]=='_')){
                    vec_push(&L->tokens,new_tok(L,T_AMPMUT,s,4)); ADV(4);
                } else TOK1(T_AMP);
                break;
            case '|': TOK1(T_PIPE); break;
            case '^': TOK1(T_CARET); break;
            case '~': TOK1(T_TILDE); break;
            case '?':
                if(d=='.')TOK2(T_QDOT); else if(d=='?')TOK2(T_QQ); else TOK1(T_QUESTION); break;
            default: { char m[64]; snprintf(m,sizeof m,"unexpected character '%c' (0x%02x)",isprint((unsigned char)c)?c:'?',(unsigned char)c); lex_error(L,E_LEX_CHAR,m); }
        }
        #undef ADV
        #undef TOK1
        #undef TOK2
    }
    /* flush trailing newline + dedents + eof */
    if (L->tokens.len>0 && ((Token*)L->tokens.items[L->tokens.len-1])->kind!=T_NEWLINE)
        vec_push(&L->tokens, new_tok(L, T_NEWLINE, L->cur, 0));
    while (L->indent_top>0){ L->indent_top--; vec_push(&L->tokens,new_tok(L,T_DEDENT,L->cur,0)); }
    vec_push(&L->tokens, new_tok(L, T_EOF, L->cur, 0));
}

static Token **tokenize(const char *src, const char *filename, int *out_count){
    Lexer L; memset(&L,0,sizeof L);
    L.src=src; L.cur=src; L.filename=filename; vec_init(&L.tokens);
    L.file_id = src_register(filename, src);
    lex(&L);
    *out_count = L.tokens.len;
    return (Token**)L.tokens.items;
}

/* Variant used by the module loader / interp that already has a file id. */
static Token **tokenize_in(const char *src, const char *filename, int file_id, int *out_count){
    Lexer L; memset(&L,0,sizeof L);
    L.src=src; L.cur=src; L.filename=filename; vec_init(&L.tokens);
    L.file_id = file_id;
    lex(&L);
    *out_count = L.tokens.len;
    return (Token**)L.tokens.items;
}

/* ===================================================================== AST */
typedef enum {
    /* expressions */
    N_INT, N_DECIMAL, N_FLOAT, N_STRING, N_BOOL, N_NONE,
    N_IDENT, N_ARRAY, N_RECORD, N_INDEX, N_SLICE, N_CALL, N_FIELD,
    N_UNARY, N_BINARY, N_LOGICAL, N_TERNARY, N_QDOT, N_QQ, N_TRY,
    N_RANGE, N_LAMBDA, N_INTERP,
    /* statements */
    N_LET, N_ASSIGN, N_EXPRSTMT, N_RETURN, N_IF, N_CHECK, N_LOOP_TILL,
    N_LOOP_THROUGH, N_LOOP_INF, N_BREAK, N_CONTINUE, N_ASSERT, N_BLOCK,
    N_DEF, N_STRUCT, N_ENUM, N_INTERFACE, N_CLASS, N_TEST, N_EXECUTE,
    N_REGION, N_COMPTIME, N_IMPORT, N_MODULE, N_PARALLEL, N_UNSAFE, N_EXTERN,
    /* helper nodes */
    N_PARAM, N_FIELDDEF, N_VARIANT, N_ARM, N_HANDLER, N_TYPE, N_NAMEDARG
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    int line;
    int file_id, lo, hi;   /* C1: span carried on every node for diagnostics */
    /* literals */
    int64_t ival;
    double  fval;
    int64_t dec_unscaled; int dec_scale;   /* exact decimal */
    char   *sval;        /* string/ident/op name/type name */
    bool    bval;
    char   *tag;         /* context-string tag (sql/...) or attribute */
    /* tree structure (used per-kind, documented at use site) */
    Node   *a, *b, *c, *d;
    Vec     list;        /* children: args, statements, fields, arms... */
    Vec     list2;       /* secondary list: elif clauses, handlers... */
    /* declaration metadata */
    bool    is_mut;
    bool    is_public;
    bool    is_prop;     /* test prop */
    Node   *type;        /* declared type (N_TYPE) or NULL */
    Node   *refine;      /* refinement predicate expr or NULL */
    Vec     derives;     /* char* names for `derive (...)` */
    Node   *ret_type;    /* function return type */
    Vec     effects;     /* char* effect names */
};

/* Set by the parser before each node() so AST nodes inherit the current span. */
static int g_cur_file_id = 0, g_cur_lo = 0, g_cur_hi = 0;

static Node *node(NodeKind k, int line){
    Node *n = (Node*)xalloc(sizeof(Node));
    n->kind=k; n->line=line;
    n->file_id = g_cur_file_id; n->lo = g_cur_lo; n->hi = g_cur_hi;
    vec_init(&n->list); vec_init(&n->list2);
    vec_init(&n->derives); vec_init(&n->effects);
    return n;
}

/* ============================================================== type nodes */
/* N_TYPE: sval = base name; list = type args; refine = predicate; 
   is_mut flag reused as "optional T?"; tag may carry "shared"/"untrusted"/... */

/* ================================================================== parser */
typedef struct {
    Token **toks; int n; int pos;
    const char *filename;
    int file_id;
} Parser;

#define E_PARSE "parse.error"

static jmp_buf g_parse_recover;
static int g_parse_panicking = 0;   /* suppress cascade errors until we resync */

static Token *pk(Parser *P){
    Token *t = P->toks[P->pos];
    /* feed the node() span hook so AST nodes inherit the current token's span */
    g_cur_file_id = t->file_id; g_cur_lo = t->lo; g_cur_hi = t->hi>t->lo?t->hi:t->lo+1;
    return t;
}
static Token *pk2(Parser *P){ return (P->pos+1<P->n)?P->toks[P->pos+1]:P->toks[P->n-1]; }
static TokKind cur(Parser *P){ return pk(P)->kind; }
static bool chk(Parser *P, TokKind k){ return cur(P)==k; }
static Token *adv(Parser *P){ Token *t=pk(P); if(P->pos<P->n-1)P->pos++; return t; }
static bool mtch(Parser *P, TokKind k){ if(chk(P,k)){adv(P);return true;} return false; }
static void perr(Parser *P, const char *msg){
    if (g_parse_panicking) longjmp(g_parse_recover, 1);   /* already recovering */
    Token *t=pk(P);
    const char *near = t->len>0 ? t->start : "end of input";
    int nlen = t->len>0 ? t->len : (int)strlen("end of input");
    diag_add(E_PARSE, t->file_id, t->lo, t->hi>t->lo?t->hi:t->lo+1,
             "%s (found '%.*s')", msg, nlen, near);
    g_parse_panicking = 1;
    longjmp(g_parse_recover, 1);
}
static Token *expect(Parser *P, TokKind k, const char *msg){
    if(!chk(P,k)) perr(P,msg); return adv(P);
}
static void skip_newlines(Parser *P){ while(chk(P,T_NEWLINE)) adv(P); }
static char *tok_text(Token *t){ return xstrndup(t->start, t->len); }

/* 1.1 recovery: discard tokens to the next statement boundary at the current or
 * a shallower indentation, then resume — this bounds cascade errors without the
 * bracket-counting that brace languages use. */
static void parse_synchronize(Parser *P){
    int depth = 0;
    while (!chk(P, T_EOF)){
        TokKind k = cur(P);
        if (k == T_INDENT) depth++;
        else if (k == T_DEDENT){ if (depth>0) depth--; else { adv(P); break; } }
        else if (k == T_NEWLINE && depth == 0){ adv(P); break; }
        adv(P);
    }
    g_parse_panicking = 0;
}

/* forward decls */
static Node *parse_expr(Parser *P);
static Node *parse_stmt(Parser *P);
static Node *parse_block(Parser *P);          /* INDENT stmts DEDENT or inline */
static Node *parse_type(Parser *P);
static Node *parse_decl_or_stmt(Parser *P);
static Node *parse_refinement(Parser *P);

/* ---- types: Name[args]  with optional `?`, `where pred`, qualifiers ---- */
static Node *parse_type(Parser *P){
    Node *t = node(N_TYPE, pk(P)->line);
    /* qualifiers: shared<T>, mut, Untrusted<T>, Secret<T> handled as named generics */
    if (chk(P,T_SHARED)){ adv(P); t->tag=xstrdup("shared"); }
    if (chk(P,T_MUT)){ adv(P); t->is_public=true; } /* reuse flag: mut-type marker */
    Token *nm = expect(P, T_IDENT, "expected type name");
    t->sval = tok_text(nm);
    if (mtch(P,T_LBRACK)){
        while(!chk(P,T_RBRACK)){
            vec_push(&t->list, parse_type(P));
            if(!mtch(P,T_COMMA)) break;
        }
        expect(P,T_RBRACK,"expected ']' in type args");
    } else if (mtch(P,T_LT)){          /* generic<...> form */
        while(!chk(P,T_GT)){
            vec_push(&t->list, parse_type(P));
            if(!mtch(P,T_COMMA)) break;
        }
        expect(P,T_GT,"expected '>' in type args");
    }
    if (mtch(P,T_QUESTION)) t->is_mut=true;     /* optional T? */
    if (mtch(P,T_WHERE)){
        /* refinement predicate with implicit subject (see parse_refinement) */
        t->refine = parse_refinement(P);
    }
    return t;
}

/* operator precedence (binding power) for infix ops; higher binds tighter */
static int infix_prec(TokKind k){
    switch(k){
        case T_OR: return 1;
        case T_AND: return 2;
        case T_QQ: return 3;
        case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE: return 4;
        case T_ARROW: return 5;          /* range a -> b */
        case T_PIPE: return 6;
        case T_CARET: return 7;
        case T_AMP: return 8;            /* binary bitwise-and */
        case T_SHL: case T_SHR: return 9;
        case T_PLUS: case T_MINUS: return 10;
        case T_STAR: case T_SLASH: case T_PERCENT: return 11;
        case T_POW: return 12;          /* right assoc */
        default: return -1;
    }
}
static bool is_logical(TokKind k){ return k==T_AND||k==T_OR; }

static Node *parse_primary(Parser *P);
static Node *parse_unary(Parser *P);
static Node *parse_postfix(Parser *P, Node *e);
static Node *parse_execute(Parser *P);
static Node *parse_check(Parser *P);
static Node *parse_if(Parser *P);

/* parse interpolated/context string into N_INTERP (parts) or N_STRING */
static Node *parse_string(Token *t){
    const char *s = t->strlit; size_t n = strlen(s);
    Node *node_i = node(N_INTERP, t->line);
    node_i->tag = t->tag;       /* context tag or NULL */
    Buf cur_text; buf_init(&cur_text);
    bool has_interp = false;
    for (size_t i=0;i<n;i++){
        if (s[i]=='\1' && i+1<n && s[i+1]=='#'){ buf_putc(&cur_text,'#'); i++; continue; }
        if (s[i]=='#' && i+1<n && (isalpha((unsigned char)s[i+1])||s[i+1]=='_'||s[i+1]=='{')){
            /* flush literal */
            Node *lit = node(N_STRING, t->line); lit->sval = xstrdup(cur_text.data);
            vec_push(&node_i->list, lit);
            buf_init(&cur_text);
            has_interp = true;
            i++;
            if (s[i]=='{'){
                i++; Buf ex; buf_init(&ex); int depth=1;
                while(i<n && depth>0){ if(s[i]=='{')depth++; else if(s[i]=='}'){depth--; if(!depth)break;} buf_putc(&ex,s[i]); i++; }
                /* parse the embedded expression */
                int cnt; Token **tt = tokenize(ex.data, "<interp>", &cnt);
                Parser ip = {tt, cnt, 0, "<interp>", (cnt>0?tt[0]->file_id:0)};
                skip_newlines(&ip);
                Node *e = parse_expr(&ip);
                Node *holder = node(N_BLOCK, t->line); holder->a = e; holder->ival = 1; /* mark expr */
                vec_push(&node_i->list, e);
            } else {
                Buf id; buf_init(&id);
                while(i<n && (isalnum((unsigned char)s[i])||s[i]=='_')){ buf_putc(&id,s[i]); i++; }
                i--;
                Node *idn = node(N_IDENT, t->line); idn->sval = xstrdup(id.data);
                vec_push(&node_i->list, idn);
            }
        } else {
            buf_putc(&cur_text, s[i]);
        }
    }
    if (cur_text.len>0 || node_i->list.len==0){
        Node *lit = node(N_STRING, t->line); lit->sval = xstrdup(cur_text.data);
        vec_push(&node_i->list, lit);
    }
    if (!has_interp && !t->tag){
        /* plain string literal */
        Node *lit = node(N_STRING, t->line);
        lit->sval = (node_i->list.len==1)?((Node*)node_i->list.items[0])->sval:xstrdup(s);
        return lit;
    }
    return node_i;
}

/* decimal literal: parse "12.34" → unscaled=1234, scale=2 */
static void parse_decimal_lit(const char *s, int len, int64_t *unscaled, int *scale){
    int64_t u=0; int sc=0; bool dot=false;
    for(int i=0;i<len;i++){
        char c=s[i];
        if(c=='_') continue;
        if(c=='.'){ dot=true; continue; }
        if(c>='0'&&c<='9'){ u=u*10+(c-'0'); if(dot)sc++; }
    }
    *unscaled=u; *scale=sc;
}

static Node *parse_primary(Parser *P){
    Token *t = pk(P);
    switch(cur(P)){
        case T_INT: { adv(P); Node *n=node(N_INT,t->line);
            Buf b; buf_init(&b); for(int i=0;i<t->len;i++) if(t->start[i]!='_') buf_putc(&b,t->start[i]);
            n->ival=strtoll(b.data,NULL,10); return n; }
        case T_FLOAT:{ adv(P); Node *n=node(N_FLOAT,t->line);
            Buf b; buf_init(&b); for(int i=0;i<t->len;i++) if(t->start[i]!='_'&&t->start[i]!='f') buf_putc(&b,t->start[i]);
            n->fval=strtod(b.data,NULL); return n; }
        case T_DECIMAL:{ adv(P); Node *n=node(N_DECIMAL,t->line);
            parse_decimal_lit(t->start,t->len,&n->dec_unscaled,&n->dec_scale); return n; }
        case T_STRING:{ adv(P); return parse_string(t); }
        case T_TRUE: adv(P); { Node*n=node(N_BOOL,t->line); n->bval=true; return n; }
        case T_FALSE: adv(P);{ Node*n=node(N_BOOL,t->line); n->bval=false; return n; }
        case T_NONE: adv(P); return node(N_NONE,t->line);
        case T_IDENT:{ adv(P); Node*n=node(N_IDENT,t->line); n->sval=tok_text(t); return n; }
        case T_LPAREN:{
            adv(P);
            /* could be: grouping, lambda (params): expr, or unit */
            /* lambda detection: ( idlist ) : ...  OR  () : ... — but `):` after
               params with a following expr. We detect by scanning to matching ).*/
            int save=P->pos;
            /* try to parse as lambda param list */
            bool is_lambda=false;
            { int depth=1, q=P->pos;
              while(q<P->n && depth>0){ TokKind k=P->toks[q]->kind;
                  if(k==T_LPAREN)depth++; else if(k==T_RPAREN)depth--; q++; }
              if(q<P->n && P->toks[q]->kind==T_COLON) is_lambda=true;
            }
            if(is_lambda){
                Node *lam=node(N_LAMBDA,t->line);
                while(!chk(P,T_RPAREN)){
                    Node *par=node(N_PARAM,pk(P)->line);
                    par->sval=tok_text(expect(P,T_IDENT,"lambda param name"));
                    if(mtch(P,T_COLON)) par->type=parse_type(P);
                    vec_push(&lam->list,par);
                    if(!mtch(P,T_COMMA)) break;
                }
                expect(P,T_RPAREN,"expected ')'");
                expect(P,T_COLON,"expected ':' in lambda");
                lam->a=parse_expr(P);
                return lam;
            }
            P->pos=save;
            if(chk(P,T_RPAREN)){ adv(P); return node(N_NONE,t->line); } /* () unit */
            Node *e=parse_expr(P);
            expect(P,T_RPAREN,"expected ')'");
            return e;
        }
        case T_LBRACK:{
            adv(P); Node*n=node(N_ARRAY,t->line);
            skip_newlines(P);
            while(!chk(P,T_RBRACK)){
                vec_push(&n->list, parse_expr(P));
                skip_newlines(P);
                if(!mtch(P,T_COMMA)) break;
                skip_newlines(P);
            }
            expect(P,T_RBRACK,"expected ']'");
            return n;
        }
        case T_LBRACE:{
            adv(P); Node*n=node(N_RECORD,t->line);
            skip_newlines(P);
            while(!chk(P,T_RBRACE)){
                Node *kv=node(N_NAMEDARG,pk(P)->line);
                kv->sval=tok_text(expect(P,T_IDENT,"record field name"));
                expect(P,T_COLON,"expected ':'");
                kv->a=parse_expr(P);
                vec_push(&n->list,kv);
                skip_newlines(P);
                if(!mtch(P,T_COMMA)) break;
                skip_newlines(P);
            }
            expect(P,T_RBRACE,"expected '}'");
            return n;
        }
        case T_REFLECT:{
            adv(P); expect(P,T_LPAREN,"expected '(' after reflect");
            Node*n=node(N_UNARY,t->line); n->sval=xstrdup("reflect");
            n->a=parse_expr(P);
            expect(P,T_RPAREN,"expected ')'");
            return n;
        }
        case T_EXECUTE: return parse_execute(P);   /* execute-expression */
        case T_CHECK:   return parse_check(P);     /* check-expression   */
        case T_IF:      return parse_if(P);        /* if-expression      */
        default: perr(P,"expected expression"); return NULL;
    }
}

static bool starts_try_terminator(TokKind k){
    return k==T_NEWLINE||k==T_RPAREN||k==T_RBRACK||k==T_RBRACE||k==T_COMMA||
           k==T_SEMI||k==T_DEDENT||k==T_EOF||k==T_COLON;
}

/* member names after '.' / '?.' may be keywords (e.g. s.spawn(), x.loop) */
static char *member_name(Parser *P){
    Token *t=pk(P);
    if(t->len>0 && (isalpha((unsigned char)t->start[0])||t->start[0]=='_')){ adv(P); return tok_text(t); }
    perr(P,"expected member name"); return NULL;
}

static Node *parse_postfix(Parser *P, Node *e){
    for(;;){
        if(chk(P,T_LPAREN)){               /* call */
            int line=pk(P)->line; adv(P);
            Node *call=node(N_CALL,line); call->a=e;
            skip_newlines(P);
            while(!chk(P,T_RPAREN)){
                /* named arg?  name: value   (or name = value) */
                if((chk(P,T_IDENT)) && (pk2(P)->kind==T_COLON)){
                    Node *na=node(N_NAMEDARG,pk(P)->line);
                    na->sval=tok_text(adv(P)); adv(P); /* : */
                    na->a=parse_expr(P);
                    vec_push(&call->list,na);
                } else {
                    vec_push(&call->list, parse_expr(P));
                }
                skip_newlines(P);
                if(!mtch(P,T_COMMA)) break;
                skip_newlines(P);
            }
            expect(P,T_RPAREN,"expected ')'");
            e=call;
        } else if(chk(P,T_LBRACK)){        /* index or slice */
            int line=pk(P)->line; adv(P);
            Node *ix=node(N_INDEX,line); ix->a=e;
            while(!chk(P,T_RBRACK)){
                /* slice element: expr | `..` | a -> b */
                if(chk(P,T_DOT) && pk2(P)->kind==T_DOT){ adv(P);adv(P);
                    Node *all=node(N_IDENT,line); all->sval=xstrdup("..");
                    vec_push(&ix->list, all);
                    ix->kind=N_SLICE;
                } else {
                    Node *sub=parse_expr(P);
                    if(sub->kind==N_RANGE) ix->kind=N_SLICE;   /* a[lo -> hi] is a slice */
                    vec_push(&ix->list, sub);
                }
                if(!mtch(P,T_COMMA)) break;
                ix->kind=N_SLICE;
            }
            expect(P,T_RBRACK,"expected ']'");
            e=ix;
        } else if(chk(P,T_DOT)){           /* field / method */
            int line=pk(P)->line; adv(P);
            Node *f=node(N_FIELD,line); f->a=e;
            f->sval=member_name(P);
            e=f;
        } else if(chk(P,T_QDOT)){          /* optional chain ?. */
            int line=pk(P)->line; adv(P);
            Node *f=node(N_QDOT,line); f->a=e;
            f->sval=member_name(P);
            e=f;
        } else if(chk(P,T_QUESTION) && starts_try_terminator(pk2(P)->kind)){
            int line=pk(P)->line; adv(P);   /* try / Fail propagation */
            Node *tr=node(N_TRY,line); tr->a=e; e=tr;
        } else break;
    }
    return e;
}

static Node *parse_unary(Parser *P){
    Token *t=pk(P);
    if(chk(P,T_NOT)){ adv(P); Node*n=node(N_UNARY,t->line); n->sval=xstrdup("not"); n->a=parse_unary(P); return n; }
    if(chk(P,T_MINUS)){ adv(P); Node*n=node(N_UNARY,t->line); n->sval=xstrdup("-"); n->a=parse_unary(P); return n; }
    if(chk(P,T_TILDE)){ adv(P); Node*n=node(N_UNARY,t->line); n->sval=xstrdup("~"); n->a=parse_unary(P); return n; }
    if(chk(P,T_AMPMUT)){ adv(P); Node*n=node(N_UNARY,t->line); n->sval=xstrdup("&mut"); n->a=parse_unary(P); return n; }
    if(chk(P,T_AMP)){ adv(P); Node*n=node(N_UNARY,t->line); n->sval=xstrdup("&"); n->a=parse_unary(P); return n; }
    if(chk(P,T_STAR)){ adv(P); Node*n=node(N_UNARY,t->line); n->sval=xstrdup("*"); n->a=parse_unary(P); return n; }
    Node *e=parse_primary(P);
    return parse_postfix(P,e);
}

static Node *parse_binary(Parser *P, int min_prec){
    Node *left=parse_unary(P);
    for(;;){
        TokKind k=cur(P);
        /* `/ {` introduces an effect clause, never division by a record literal */
        if(k==T_SLASH && pk2(P)->kind==T_LBRACE) break;
        int prec=infix_prec(k);
        if(prec<min_prec) break;
        Token *opt=adv(P);
        bool right_assoc = (k==T_POW);
        int next_min = right_assoc ? prec : prec+1;
        Node *right=parse_binary(P, next_min);
        NodeKind nk = is_logical(k)?N_LOGICAL : (k==T_QQ?N_QQ : (k==T_ARROW?N_RANGE : N_BINARY));
        Node *bin=node(nk,opt->line);
        bin->a=left; bin->b=right; bin->sval=tok_text(opt);
        left=bin;
    }
    return left;
}

static Node *parse_expr(Parser *P){
    Node *e=parse_binary(P,1);
    /* ternary: cond ? then : else  (the try-? was consumed in postfix) */
    if(chk(P,T_QUESTION)){
        int line=pk(P)->line; adv(P);
        Node *tern=node(N_TERNARY,line); tern->a=e;
        tern->b=parse_expr(P);
        expect(P,T_COLON,"expected ':' in ternary");
        tern->c=parse_expr(P);
        return tern;
    }
    return e;
}

/* refinement: implicit-subject predicate.
   forms:  >= 0 | != 0 | > 0 | ...        → __subj__ OP rhs
           len > 0 | size >= 1            → len(__subj__) OP rhs
           otherwise a full bool expr (subject reachable as __subj__) */
static Node *parse_refinement(Parser *P){
    int line=pk(P)->line;
    TokKind k=cur(P);
    Node *subj=node(N_IDENT,line); subj->sval=xstrdup("__subj__");
    if(k==T_EQ||k==T_NE||k==T_LT||k==T_LE||k==T_GT||k==T_GE){
        Token *opt=adv(P);
        Node *rhs=parse_binary(P,5);
        Node *b=node(N_BINARY,line); b->sval=tok_text(opt); b->a=subj; b->b=rhs;
        return b;
    }
    /* general expression; bare `len`/`size`/`count` refer to subject length */
    return parse_expr(P);
}

/* a block follows a ':' — either inline statements on the same line, or an
   indented suite. Returns N_BLOCK with list of statements. */
static Node *parse_block(Parser *P){
    Node *blk=node(N_BLOCK,pk(P)->line);
    if(chk(P,T_NEWLINE)){
        skip_newlines(P);
        expect(P,T_INDENT,"expected indented block");
        while(!chk(P,T_DEDENT) && !chk(P,T_EOF)){
            skip_newlines(P);
            if(chk(P,T_DEDENT)||chk(P,T_EOF)) break;
            vec_push(&blk->list, parse_decl_or_stmt(P));
            while(mtch(P,T_SEMI)){       /* `;`-separated statements on a line */
                if(chk(P,T_NEWLINE)||chk(P,T_DEDENT)||chk(P,T_EOF)) break;
                vec_push(&blk->list, parse_decl_or_stmt(P));
            }
            skip_newlines(P);
        }
        expect(P,T_DEDENT,"expected dedent");
    } else {
        /* inline: one or more ';'-separated statements on the same line */
        vec_push(&blk->list, parse_decl_or_stmt(P));
        while(mtch(P,T_SEMI)){
            if(chk(P,T_NEWLINE)||chk(P,T_DEDENT)||chk(P,T_EOF)) break;
            vec_push(&blk->list, parse_decl_or_stmt(P));
        }
    }
    return blk;
}

static Node *parse_params(Parser *P){
    /* returns N_BLOCK whose list holds N_PARAM nodes; consumes (...) */
    Node *holder=node(N_BLOCK,pk(P)->line);
    expect(P,T_LPAREN,"expected '('");
    while(!chk(P,T_RPAREN)){
        Node *par=node(N_PARAM,pk(P)->line);
        if(chk(P,T_MUT)){ adv(P); par->is_mut=true; }
        /* `self` / `self: &mut` */
        par->sval=tok_text(expect(P,T_IDENT,"expected parameter name"));
        if(mtch(P,T_COLON)){
            if(chk(P,T_AMPMUT)){ adv(P); par->is_mut=true; par->type=NULL;
                if(chk(P,T_IDENT)) par->type=parse_type(P); }
            else if(chk(P,T_AMP)){ adv(P); if(chk(P,T_IDENT)) par->type=parse_type(P); }
            else par->type=parse_type(P);
        }
        if(mtch(P,T_ASSIGN)){ par->a=parse_expr(P); }   /* default value */
        vec_push(&holder->list,par);
        if(!mtch(P,T_COMMA)) break;
    }
    expect(P,T_RPAREN,"expected ')'");
    return holder;
}

static void parse_effects(Parser *P, Vec *effects){
    /* after '/' :  { Eff1, Eff2<...> }  or a single Eff */
    if(mtch(P,T_LBRACE)){
        while(!chk(P,T_RBRACE)){
            Buf b; buf_init(&b);
            buf_puts(&b, tok_text(expect(P,T_IDENT,"effect name")));
            if(mtch(P,T_LT)){ buf_puts(&b,"<");
                int depth=1; while(depth>0 && !chk(P,T_EOF)){
                    Token *tt=adv(P);
                    if(tt->kind==T_LT)depth++; else if(tt->kind==T_GT){depth--; if(!depth)break;}
                    buf_putn(&b,tt->start,tt->len);
                }
                buf_puts(&b,">");
            }
            vec_push(effects, xstrdup(b.data));
            if(!mtch(P,T_COMMA)) break;
        }
        expect(P,T_RBRACE,"expected '}'");
    } else {
        Buf b; buf_init(&b);
        buf_puts(&b, tok_text(expect(P,T_IDENT,"effect name")));
        vec_push(effects, xstrdup(b.data));
    }
}

static Node *parse_def(Parser *P, bool is_public){
    int line=pk(P)->line; expect(P,T_DEF,"expected 'def'");
    Node *fn=node(N_DEF,line); fn->is_public=is_public;
    fn->sval=tok_text(expect(P,T_IDENT,"expected function name"));
    Node *params=parse_params(P);
    fn->list2 = params->list;     /* params */
    if(mtch(P,T_SEMI)){
        fn->ret_type=parse_type(P);
    }
    if(mtch(P,T_SLASH)){
        parse_effects(P, &fn->effects);
    }
    expect(P,T_COLON,"expected ':' before function body");
    fn->a=parse_block(P);
    return fn;
}

static Node *parse_struct(Parser *P, Vec *derives){
    int line=pk(P)->line; expect(P,T_STRUCT,"expected 'struct'");
    Node *st=node(N_STRUCT,line);
    st->sval=tok_text(expect(P,T_IDENT,"expected struct name"));
    if(derives) st->derives=*derives;
    expect(P,T_COLON,"expected ':'");
    skip_newlines(P); expect(P,T_INDENT,"expected struct body");
    while(!chk(P,T_DEDENT)&&!chk(P,T_EOF)){
        skip_newlines(P); if(chk(P,T_DEDENT))break;
        Node *fd=node(N_FIELDDEF,pk(P)->line);
        if(chk(P,T_PUBLIC)){adv(P);fd->is_public=true;}
        else if(chk(P,T_PRIVATE)){adv(P);fd->is_public=false;}
        fd->sval=tok_text(expect(P,T_IDENT,"field name"));
        expect(P,T_COLON,"expected ':'");
        fd->type=parse_type(P);
        if(mtch(P,T_ASSIGN)) fd->a=parse_expr(P);  /* default */
        vec_push(&st->list,fd);
        skip_newlines(P);
    }
    expect(P,T_DEDENT,"expected dedent");
    return st;
}

static Node *parse_enum(Parser *P, Vec *derives){
    int line=pk(P)->line; expect(P,T_ENUM,"expected 'enum'");
    Node *en=node(N_ENUM,line);
    en->sval=tok_text(expect(P,T_IDENT,"expected enum name"));
    if(derives) en->derives=*derives;
    expect(P,T_COLON,"expected ':'");
    skip_newlines(P); expect(P,T_INDENT,"expected enum body");
    while(!chk(P,T_DEDENT)&&!chk(P,T_EOF)){
        skip_newlines(P); if(chk(P,T_DEDENT))break;
        Node *v=node(N_VARIANT,pk(P)->line);
        v->sval=tok_text(expect(P,T_IDENT,"variant name"));
        if(mtch(P,T_LPAREN)){
            while(!chk(P,T_RPAREN)){
                Node *fd=node(N_FIELDDEF,pk(P)->line);
                /* payload may be `name: Type` or just `Type` */
                if(chk(P,T_IDENT)&&pk2(P)->kind==T_COLON){
                    fd->sval=tok_text(adv(P)); adv(P);
                    fd->type=parse_type(P);
                } else {
                    fd->type=parse_type(P);
                    fd->sval=NULL;
                }
                vec_push(&v->list,fd);
                if(!mtch(P,T_COMMA)) break;
            }
            expect(P,T_RPAREN,"expected ')'");
        }
        vec_push(&en->list,v);
        skip_newlines(P);
    }
    expect(P,T_DEDENT,"expected dedent");
    return en;
}

static Node *parse_class(Parser *P, Vec *derives){
    int line=pk(P)->line;
    bool iface = chk(P,T_INTERFACE);
    adv(P); /* class or interface */
    Node *cl=node(iface?N_INTERFACE:N_CLASS,line);
    cl->sval=tok_text(expect(P,T_IDENT,"expected name"));
    if(derives) cl->derives=*derives;
    /* optional `: Base` inheritance or interface list — parse and ignore base for now */
    expect(P,T_COLON,"expected ':'");
    skip_newlines(P); expect(P,T_INDENT,"expected body");
    while(!chk(P,T_DEDENT)&&!chk(P,T_EOF)){
        skip_newlines(P); if(chk(P,T_DEDENT))break;
        bool is_pub=false;
        if(chk(P,T_PUBLIC)){adv(P);is_pub=true;}
        else if(chk(P,T_PRIVATE)){adv(P);is_pub=false;}
        if(chk(P,T_DEF)){
            vec_push(&cl->list2, parse_def(P,is_pub));
        } else {
            /* method signature (interface) or field (class) */
            Node *m=node(N_FIELDDEF,pk(P)->line);
            m->is_public=is_pub;
            m->sval=tok_text(expect(P,T_IDENT,"member name"));
            if(chk(P,T_LPAREN)){
                /* interface method signature */
                Node *params=parse_params(P);
                Node *sig=node(N_DEF,m->line); sig->sval=m->sval; sig->list2=params->list;
                if(mtch(P,T_SEMI)) sig->ret_type=parse_type(P);
                if(mtch(P,T_SLASH)) parse_effects(P,&sig->effects);
                vec_push(&cl->list2,sig);
            } else {
                expect(P,T_COLON,"expected ':'");
                m->type=parse_type(P);
                if(mtch(P,T_ASSIGN)) m->a=parse_expr(P);
                vec_push(&cl->list,m);
            }
        }
        skip_newlines(P);
    }
    expect(P,T_DEDENT,"expected dedent");
    return cl;
}

static Node *parse_pattern(Parser *P){
    int line=pk(P)->line;
    Node *arm=node(N_ARM,line);
    if(chk(P,T_NONE)){ adv(P); arm->sval=xstrdup("none"); return arm; }
    if(chk(P,T_INT)||chk(P,T_STRING)||chk(P,T_TRUE)||chk(P,T_FALSE)||
       chk(P,T_DECIMAL)||chk(P,T_FLOAT)||chk(P,T_MINUS)){
        arm->b=parse_expr(P); arm->sval=NULL; return arm;   /* literal match */
    }
    if(chk(P,T_IDENT)){
        char *name=tok_text(pk(P));
        bool upper = name[0]>='A'&&name[0]<='Z';
        bool is_some = strcmp(name,"some")==0;
        adv(P);
        if(chk(P,T_LPAREN)){
            adv(P); arm->sval=name;
            while(!chk(P,T_RPAREN)){
                Node *bind=node(N_IDENT,pk(P)->line);
                bind->sval=tok_text(expect(P,T_IDENT,"binding name"));
                vec_push(&arm->list,bind);
                if(!mtch(P,T_COMMA)) break;
            }
            expect(P,T_RPAREN,"expected ')'");
            return arm;
        }
        if(upper||is_some){ arm->sval=name; return arm; } /* nullary variant */
        /* lowercase bare ident → binding pattern (matches anything) */
        arm->sval=NULL; arm->c=node(N_IDENT,line); arm->c->sval=name;
        return arm;
    }
    perr(P,"expected pattern after 'is'"); return NULL;
}

static Node *parse_check(Parser *P){
    int line=pk(P)->line; expect(P,T_CHECK,"expected 'check'");
    Node *ch=node(N_CHECK,line);
    ch->a=parse_expr(P);
    expect(P,T_COLON,"expected ':'");
    skip_newlines(P); expect(P,T_INDENT,"expected match arms");
    while(!chk(P,T_DEDENT)&&!chk(P,T_EOF)){
        skip_newlines(P); if(chk(P,T_DEDENT))break;
        if(chk(P,T_IS)){
            adv(P);
            Node *arm=parse_pattern(P);
            expect(P,T_COLON,"expected ':'");
            arm->a=parse_block(P);
            vec_push(&ch->list,arm);
        } else if(chk(P,T_ELSE)){
            adv(P); Node *arm=node(N_ARM,pk(P)->line); arm->sval=xstrdup("__else__");
            expect(P,T_COLON,"expected ':'");
            arm->a=parse_block(P);
            vec_push(&ch->list,arm);
        } else perr(P,"expected 'is' or 'else' in check");
        skip_newlines(P);
    }
    expect(P,T_DEDENT,"expected dedent");
    return ch;
}

static Node *parse_if(Parser *P){
    int line=pk(P)->line; expect(P,T_IF,"expected 'if'");
    Node *iff=node(N_IF,line);
    iff->a=parse_expr(P);
    expect(P,T_COLON,"expected ':'");
    iff->b=parse_block(P);
    skip_newlines(P);
    /* elif chain stored in list2 as pairs (cond,block) via N_IF nodes */
    while(chk(P,T_ELIF)){
        adv(P); Node *ei=node(N_IF,pk(P)->line);
        ei->a=parse_expr(P); expect(P,T_COLON,"expected ':'");
        ei->b=parse_block(P);
        vec_push(&iff->list2, ei);
        skip_newlines(P);
    }
    if(chk(P,T_ELSE)){
        adv(P); expect(P,T_COLON,"expected ':'");
        iff->c=parse_block(P);
    }
    return iff;
}

static Node *parse_loop(Parser *P){
    int line=pk(P)->line; expect(P,T_LOOP,"expected 'loop'");
    if(chk(P,T_TILL)){
        adv(P); Node *lp=node(N_LOOP_TILL,line);
        lp->a=parse_expr(P);                 /* condition (loop until true) */
        if(mtch(P,T_SEMI)){ if(!chk(P,T_SEMI)&&!chk(P,T_COLON)) lp->b=parse_decl_or_stmt(P); }  /* init */
        if(mtch(P,T_SEMI)){ if(!chk(P,T_SEMI)&&!chk(P,T_COLON)) lp->c=parse_decl_or_stmt(P); }  /* step */
        expect(P,T_COLON,"expected ':'");
        lp->d=parse_block(P);
        return lp;
    }
    if(chk(P,T_THROUGH)){
        adv(P); Node *lp=node(N_LOOP_THROUGH,line);
        lp->a=parse_expr(P);                 /* iterable; element bound as `it` */
        expect(P,T_COLON,"expected ':'");
        lp->b=parse_block(P);
        return lp;
    }
    /* bare loop: */
    Node *lp=node(N_LOOP_INF,line);
    expect(P,T_COLON,"expected ':'");
    lp->a=parse_block(P);
    return lp;
}

static Node *parse_execute(Parser *P){
    int line=pk(P)->line; expect(P,T_EXECUTE,"expected 'execute'");
    Node *ex=node(N_EXECUTE,line);
    expect(P,T_COLON,"expected ':'");
    ex->a=parse_block(P);
    skip_newlines(P);
    while(chk(P,T_HANDLE)){
        adv(P); Node *h=node(N_HANDLER,pk(P)->line);
        h->sval=tok_text(expect(P,T_IDENT,"effect name"));   /* e.g. Fail */
        if(mtch(P,T_LT)){
            Buf b; buf_init(&b); int depth=1;
            while(depth>0&&!chk(P,T_EOF)){ Token *tt=adv(P);
                if(tt->kind==T_LT)depth++; else if(tt->kind==T_GT){depth--;if(!depth)break;}
                buf_putn(&b,tt->start,tt->len); }
            h->tag=xstrdup(b.data);     /* error type arg, e.g. IoError */
        }
        h->b=NULL;
        /* optional `as <name>` binding */
        if(chk(P,T_IDENT)&&strcmp(pk(P)->start,"as")!=0){
            /* tolerate missing 'as' but a direct binding name */
        }
        if(chk(P,T_IDENT)&&strncmp(pk(P)->start,"as",2)==0&&pk(P)->len==2){
            adv(P);  /* consume 'as' */
            Token *bn=expect(P,T_IDENT,"binding name after 'as'");
            h->b=node(N_IDENT,bn->line); h->b->sval=tok_text(bn);
        }
        expect(P,T_COLON,"expected ':'");
        h->a=parse_block(P);
        vec_push(&ex->list2,h);
        skip_newlines(P);
    }
    return ex;
}

static Node *parse_test(Parser *P){
    int line=pk(P)->line; expect(P,T_TEST,"expected 'test'");
    Node *ts=node(N_TEST,line);
    if(mtch(P,T_PROP)) ts->is_prop=true;
    Token *nm=expect(P,T_STRING,"expected test name string");
    ts->sval=nm->strlit;
    if(ts->is_prop && chk(P,T_LPAREN)){
        Node *params=parse_params(P);
        ts->list2=params->list;
    }
    expect(P,T_COLON,"expected ':'");
    ts->a=parse_block(P);
    return ts;
}

static Node *parse_simple_stmt(Parser *P){
    int line=pk(P)->line;
    /* typed let:  name : Type [where ...] = expr  */
    if(chk(P,T_IDENT)&&pk2(P)->kind==T_COLON){
        Node *let=node(N_LET,line);
        let->sval=tok_text(adv(P)); adv(P); /* : */
        let->type=parse_type(P);
        if(let->type && let->type->refine) let->refine=let->type->refine;
        expect(P,T_ASSIGN,"expected '=' in typed binding");
        let->a=parse_expr(P);
        return let;
    }
    Node *lhs=parse_expr(P);
    TokKind k=cur(P);
    if(k==T_ASSIGN){
        adv(P);
        if(lhs->kind==N_IDENT){
            Node *let=node(N_LET,line); let->sval=lhs->sval; let->a=parse_expr(P);
            return let;
        }
        Node *as=node(N_ASSIGN,line); as->sval=xstrdup("="); as->a=lhs; as->b=parse_expr(P);
        return as;
    }
    if(k==T_PLUSEQ||k==T_MINUSEQ||k==T_STAREQ||k==T_SLASHEQ||k==T_PERCENTEQ){
        Token *opt=adv(P);
        Node *as=node(N_ASSIGN,line); as->sval=tok_text(opt); as->a=lhs;
        as->b=parse_expr(P); return as;
    }
    if(k==T_INC||k==T_DEC){
        Token *opt=adv(P);
        Node *as=node(N_ASSIGN,line); as->sval=tok_text(opt); as->a=lhs; as->b=NULL;
        return as;
    }
    Node *es=node(N_EXPRSTMT,line); es->a=lhs; return es;
}

static Node *parse_decl_or_stmt(Parser *P){
    int line=pk(P)->line;
    switch(cur(P)){
        case T_DEF:    return parse_def(P,false);
        case T_PUBLIC:
            adv(P);
            if(chk(P,T_DEF)) return parse_def(P,true);
            if(chk(P,T_STRUCT)) return parse_struct(P,NULL);
            if(chk(P,T_ENUM)) return parse_enum(P,NULL);
            /* public field at top — fall through as simple */
            { Node *s=parse_simple_stmt(P); s->is_public=true; return s; }
        case T_STRUCT: return parse_struct(P,NULL);
        case T_ENUM:   return parse_enum(P,NULL);
        case T_CLASS:
        case T_INTERFACE: return parse_class(P,NULL);
        case T_TEST:   return parse_test(P);
        case T_IF:     return parse_if(P);
        case T_CHECK:  return parse_check(P);
        case T_LOOP:   return parse_loop(P);
        case T_EXECUTE:return parse_execute(P);
        case T_DERIVE:{
            adv(P);
            Vec derives; vec_init(&derives);
            expect(P,T_LPAREN,"expected '(' after derive");
            while(!chk(P,T_RPAREN)){
                vec_push(&derives, tok_text(expect(P,T_IDENT,"trait name")));
                if(!mtch(P,T_COMMA)) break;
            }
            expect(P,T_RPAREN,"expected ')'");
            skip_newlines(P);
            if(chk(P,T_STRUCT)) return parse_struct(P,&derives);
            if(chk(P,T_ENUM)) return parse_enum(P,&derives);
            if(chk(P,T_CLASS)||chk(P,T_INTERFACE)) return parse_class(P,&derives);
            perr(P,"derive must precede a struct/enum/class"); return NULL;
        }
        case T_RETURN:{
            adv(P); Node *r=node(N_RETURN,line);
            if(!chk(P,T_NEWLINE)&&!chk(P,T_DEDENT)&&!chk(P,T_EOF)) r->a=parse_expr(P);
            return r;
        }
        case T_BREAK:   adv(P); return node(N_BREAK,line);
        case T_CONTINUE:adv(P); return node(N_CONTINUE,line);
        case T_RESUME:{ adv(P); Node *r=node(N_UNARY,line); r->sval=xstrdup("resume");
            if(!chk(P,T_NEWLINE)&&!chk(P,T_DEDENT)&&!chk(P,T_EOF)&&!chk(P,T_SEMI)) r->a=parse_expr(P);
            Node *es=node(N_EXPRSTMT,line); es->a=r; return es; }
        case T_SPAWN:{ adv(P); Node *sp=node(N_EXPRSTMT,line); sp->a=parse_expr(P); return sp; }
        case T_ASSERT:{ adv(P); Node *a=node(N_ASSERT,line); a->a=parse_expr(P); return a; }
        case T_MUT:{
            adv(P); Node *let=node(N_LET,line); let->is_mut=true;
            let->sval=tok_text(expect(P,T_IDENT,"binding name"));
            if(mtch(P,T_COLON)){ let->type=parse_type(P); if(let->type)let->refine=let->type->refine; }
            expect(P,T_ASSIGN,"expected '=' in mut binding");
            let->a=parse_expr(P);
            return let;
        }
        case T_REGION:{
            adv(P); Node *rg=node(N_REGION,line);
            rg->sval=tok_text(expect(P,T_IDENT,"region name"));
            expect(P,T_COLON,"expected ':'");
            rg->a=parse_block(P); return rg;
        }
        case T_COMPTIME:{
            adv(P); Node *ct=node(N_COMPTIME,line);
            expect(P,T_COLON,"expected ':'");
            ct->a=parse_block(P); return ct;
        }
        case T_UNSAFE:{
            adv(P); Node *u=node(N_UNSAFE,line);
            expect(P,T_COLON,"expected ':'");
            u->a=parse_block(P); return u;
        }
        case T_PARALLEL:{
            adv(P); Node *pl=node(N_PARALLEL,line);
            expect(P,T_SCOPE,"expected 'scope'");
            pl->sval=tok_text(expect(P,T_IDENT,"scope name"));
            expect(P,T_COLON,"expected ':'");
            pl->a=parse_block(P); return pl;
        }
        case T_IMPORT:{
            adv(P); Node *im=node(N_IMPORT,line);
            if(chk(P,T_STRING)){               /* import "rel/path.keel" */
                Token *st=adv(P); im->sval=st->strlit?st->strlit:tok_text(st);
                return im;
            }
            Buf b; buf_init(&b);
            buf_puts(&b, tok_text(expect(P,T_IDENT,"module name or quoted path")));
            while(mtch(P,T_DOT)){ buf_putc(&b,'.'); buf_puts(&b,tok_text(expect(P,T_IDENT,"name"))); }
            im->sval=xstrdup(b.data); return im;
        }
        case T_MODULE:{
            adv(P); Node *md=node(N_MODULE,line);
            md->sval=tok_text(expect(P,T_IDENT,"module name"));
            return md;
        }
        case T_EXTERN:{
            adv(P); Node *ex=node(N_EXTERN,line);
            if(chk(P,T_STRING)) ex->tag=adv(P)->strlit;     /* "C" */
            expect(P,T_COLON,"expected ':'");
            ex->a=parse_block(P); return ex;
        }
        default: return parse_simple_stmt(P);
    }
}

static Node *parse_program(Token **toks, int n, const char *filename){
    Parser P={toks,n,0,filename,0};
    if (n>0) P.file_id = toks[0]->file_id;
    Node *prog=node(N_BLOCK,1);
    skip_newlines(&P);
    while(!chk(&P,T_EOF)){
        if (setjmp(g_parse_recover)){
            /* a parse error fired; resynchronize to the next statement boundary */
            parse_synchronize(&P);
            skip_newlines(&P);
            continue;
        }
        Node *d = parse_decl_or_stmt(&P);
        if (d) vec_push(&prog->list, d);
        skip_newlines(&P);
    }
    return prog;
}

/* ============================================================ runtime values */
typedef struct Env Env;
typedef struct Value Value;

typedef enum {
    V_NONE, V_INT, V_DEC, V_FLOAT, V_BOOL, V_STR, V_ARRAY,
    V_RECORD, V_ENUM, V_CLOSURE, V_BUILTIN, V_TYPE,
    V_OK, V_FAIL, V_SECRET, V_UNTRUSTED, V_CTXSTR, V_CAP
} VKind;

typedef struct { char *name; Value *val; } FieldVal;

struct Value {
    VKind kind;
    union {
        int64_t i;
        double f;
        bool b;
        struct { int64_t unscaled; int scale; } dec;
        struct { char *s; size_t len; } str;
        struct { Value **items; int len; int cap; } arr;
        struct { char *type; Vec fields; } rec;       /* FieldVal* */
        struct { char *type; char *variant; Value **payload; int np; } en;
        struct { Node *fn; Env *env; } clo;
        struct { Value*(*fn)(Value**,int,Env*); char *name; } bi;
        struct { char *name; Node *decl; } ty;
        Value *inner;                                   /* OK/FAIL/SECRET/UNTRUSTED */
        struct { char *ctx; char *s; } ctxstr;
        struct { char *kind; char *root; Env *env; } cap;
    } as;
};

static Value VNONE_S = { V_NONE };
static Value *VNONE = &VNONE_S;
static Value *V_TRUE_C, *V_FALSE_C;

static Value *mkval(VKind k){ Value *v=(Value*)xalloc(sizeof(Value)); v->kind=k; return v; }
static Value *mkint(int64_t i){ Value*v=mkval(V_INT); v->as.i=i; return v; }
static Value *mkfloat(double f){ Value*v=mkval(V_FLOAT); v->as.f=f; return v; }
static Value *mkbool(bool b){ return b?V_TRUE_C:V_FALSE_C; }
static Value *mkstr_n(const char *s,size_t n){ Value*v=mkval(V_STR); v->as.str.s=xstrndup(s,n); v->as.str.len=n; return v; }
static Value *mkstr(const char *s){ return mkstr_n(s,strlen(s)); }
static Value *mkdec(int64_t u,int sc){ Value*v=mkval(V_DEC); v->as.dec.unscaled=u; v->as.dec.scale=sc; return v; }
static Value *mkctx(const char *ctx,const char *s){ Value*v=mkval(V_CTXSTR); v->as.ctxstr.ctx=xstrdup(ctx); v->as.ctxstr.s=xstrdup(s); return v; }
static Value *mkok(Value *x){ Value*v=mkval(V_OK); v->as.inner=x; return v; }
static Value *mkfail(Value *e){ Value*v=mkval(V_FAIL); v->as.inner=e; return v; }
static Value *mkarr(Value **items,int n){ Value*v=mkval(V_ARRAY); v->as.arr.items=items; v->as.arr.len=n; v->as.arr.cap=n; return v; }

/* ----------------------------------------------------------------- environment */
typedef struct Binding { char *name; Value *val; bool is_mut; struct Binding *next; } Binding;
struct Env { Binding *vars; Env *parent; };

static Env *env_new(Env *parent){ Env*e=(Env*)xalloc(sizeof(Env)); e->vars=NULL; e->parent=parent; return e; }
static void env_define(Env *e, const char *name, Value *v, bool is_mut){
    Binding *b=(Binding*)xalloc(sizeof(Binding));
    b->name=xstrdup(name); b->val=v; b->is_mut=is_mut; b->next=e->vars; e->vars=b;
}
static Binding *env_find(Env *e, const char *name){
    for(; e; e=e->parent)
        for(Binding *b=e->vars; b; b=b->next)
            if(strcmp(b->name,name)==0) return b;
    return NULL;
}

/* ----------------------------------------------------------- type registry */
typedef struct { char *name; Node *decl; } TypeEnt;
static Vec g_types;            /* TypeEnt* : struct/enum/class/interface */
static Vec g_variants;         /* maps variant name → enum decl (TypeEnt*) */

static Node *find_type(const char *name){
    for(int i=0;i<g_types.len;i++){ TypeEnt*t=(TypeEnt*)g_types.items[i]; if(strcmp(t->name,name)==0) return t->decl; }
    return NULL;
}
static Node *find_enum_of_variant(const char *vname){
    for(int i=0;i<g_variants.len;i++){ TypeEnt*t=(TypeEnt*)g_variants.items[i]; if(strcmp(t->name,vname)==0) return t->decl; }
    return NULL;
}

/* ----------------------------------------------------- runtime error / panic
 * 1.3 taxonomy: a `panic` is the interpreter's stand-in for a *compile-time
 * type error* — a violation the type system is meant to make impossible (a
 * plain string reaching a SQL sink, an unknown method, an arity mismatch).
 * These are uncatchable by design: making them recoverable would model an
 * "impossible" condition as a runtime concern. A recoverable runtime condition
 * (bad input, a missing file, a path that escapes its root) is a catchable
 * `Fail` instead. Each abort carries a stable code so negative conformance
 * cases assert on the code, and so the self-hosted checker knows which aborts
 * become static rejections. */
static jmp_buf g_panic_buf;
static char g_panic_msg[512];
static int g_panic_line;
static char g_panic_code[64] = "runtime.panic";
static const char *g_next_panic_code = NULL;   /* set just before a typed panic */

static void panic(int line, const char *fmt, ...){
    va_list ap; va_start(ap,fmt); vsnprintf(g_panic_msg,sizeof g_panic_msg,fmt,ap); va_end(ap);
    g_panic_line=line;
    snprintf(g_panic_code,sizeof g_panic_code,"%s", g_next_panic_code?g_next_panic_code:"runtime.panic");
    g_next_panic_code=NULL;
    longjmp(g_panic_buf,1);
}
/* tag the next panic with a specific type-error code */
#define PANIC_CODE(c) (g_next_panic_code=(c))

/* ------------------------------------------------- control-flow signals */
typedef enum { CTL_NONE, CTL_RETURN, CTL_BREAK, CTL_CONTINUE } Ctl;
static Ctl g_ctl = CTL_NONE;
static Value *g_ctl_val = NULL;

/* --------------------------------------------- algebraic effects + handlers
 * One-shot, in-scope resumption via the libhandler setjmp/longjmp technique
 * (Leijen 2019). A `Fail` handler frame marks an abort target (the `execute`
 * block); a `perform` runs the handler clause synchronously on the live stack;
 * `resume v` longjmps back to the perform site returning v (condition/restart);
 * a handler completing without `resume` aborts to the `execute` with its value.
 */
typedef struct Handler {
    char *effect;            /* "Fail" (or a user effect / function boundary) */
    Vec   clauses;           /* N_HANDLER* (may be empty for function boundary) */
    Env  *env;
    jmp_buf abort_buf;       /* longjmp here to terminate the execute */
    Value *result;
    bool   is_fn_boundary;   /* implicit per-call Fail→V_FAIL boundary */
    struct Handler *prev;
} Handler;
static Handler *g_handlers = NULL;

typedef struct PerformCtx {
    jmp_buf resume_buf;      /* longjmp here on `resume` */
    Value  *resume_val;
    Handler *h;
    struct PerformCtx *prev;
} PerformCtx;
static PerformCtx *g_perform = NULL;   /* current handling context (for `resume`) */

static void push_handler(Handler *h){ h->prev=g_handlers; g_handlers=h; }
static void pop_handler(Handler *h){ if(g_handlers==h) g_handlers=h->prev; }

static Handler *find_handler(const char *effect){
    for(Handler *h=g_handlers; h; h=h->prev){
        if(h->is_fn_boundary && strcmp(effect,"Fail")==0) return h;
        if(strcmp(h->effect,effect)==0) return h;
        /* match by prefix: effect "Fail" matches clause "Fail<...>" */
        if(strncmp(h->effect,effect,strlen(effect))==0) return h;
    }
    return NULL;
}

/* forward */
static Value *eval(Node *n, Env *env);
static Value *eval_block(Node *blk, Env *env);
static Value *call_value(Value *callee, Value **args, int nargs, int line, Env *callsite);
static void   value_to_buf(Buf *b, Value *v, bool repr);
static char  *value_to_cstr(Value *v, bool repr);
static bool   value_truthy(Value *v);
static bool   values_equal(Value *a, Value *b);
static Value *builtin_method(Value *recv, const char *m, Value **args, int nargs, int line);
static Value *eval_execute(Node *n, Env *env);
static Env   *g_genv;   /* global environment (for method closures) */

static Value *perform_fail(Value *err, int line){
    Handler *h = find_handler("Fail");
    if(!h){
        char *m = value_to_cstr(err, true);
        panic(line, "unhandled failure: %s", m);
    }
    if(h->is_fn_boundary){
        h->result = mkfail(err);
        longjmp(h->abort_buf, 1);
    }
    PerformCtx pc; pc.h=h; pc.resume_val=NULL; pc.prev=g_perform;
    if(setjmp(pc.resume_buf)==0){
        g_perform=&pc;
        Node *clause=NULL;
        for(int i=0;i<h->clauses.len;i++){
            Node *c=(Node*)h->clauses.items[i];
            if(strncmp(c->sval,"Fail",4)==0){ clause=c; break; }
        }
        if(!clause && h->clauses.len) clause=(Node*)h->clauses.items[0];
        Env *henv=env_new(h->env);
        if(clause && clause->b) env_define(henv, clause->b->sval, err, false);
        Value *hv = clause ? eval_block(clause->a, henv) : VNONE;
        g_perform=pc.prev;
        h->result=hv;
        longjmp(h->abort_buf,1);
    } else {
        g_perform=pc.prev;
        return pc.resume_val;      /* condition/restart: resume at failure site */
    }
}

static Value *do_resume(Value *v, int line){
    if(!g_perform) panic(line, "`resume` used outside an effect handler");
    PerformCtx *pc=g_perform;
    pc->resume_val=v;
    longjmp(pc->resume_buf,1);
    return NULL; /* unreachable */
}

/* ------------------------------------------------------------ value helpers */
static bool value_truthy(Value *v){
    switch(v->kind){
        case V_NONE: return false;
        case V_BOOL: return v->as.b;
        case V_INT: return v->as.i!=0;
        case V_STR: return v->as.str.len>0;
        case V_ARRAY: return v->as.arr.len>0;
        case V_OK: return true;
        case V_FAIL: return false;
        default: return true;
    }
}

static int64_t ipow10(int n){ int64_t r=1; while(n-->0) r*=10; return r; }

static void dec_to_buf(Buf *b, int64_t unscaled, int scale){
    bool neg = unscaled<0; if(neg) unscaled=-unscaled;
    int64_t p = ipow10(scale);
    int64_t ip = scale? unscaled/p : unscaled;
    int64_t fp = scale? unscaled%p : 0;
    if(neg) buf_putc(b,'-');
    buf_printf(b,"%lld",(long long)ip);
    if(scale){ char tmp[32]; snprintf(tmp,sizeof tmp,"%0*lld",scale,(long long)fp); buf_putc(b,'.'); buf_puts(b,tmp); }
}

static void value_to_buf(Buf *b, Value *v, bool repr){
    switch(v->kind){
        case V_NONE: buf_puts(b,"none"); break;
        case V_INT: buf_printf(b,"%lld",(long long)v->as.i); break;
        case V_FLOAT:{ char t[32]; snprintf(t,sizeof t,"%g",v->as.f); buf_puts(b,t); } break;
        case V_DEC: dec_to_buf(b,v->as.dec.unscaled,v->as.dec.scale); break;
        case V_BOOL: buf_puts(b, v->as.b?"true":"false"); break;
        case V_STR:
            if(repr){ buf_putc(b,'"'); buf_putn(b,v->as.str.s,v->as.str.len); buf_putc(b,'"'); }
            else buf_putn(b,v->as.str.s,v->as.str.len);
            break;
        case V_CTXSTR: buf_printf(b,"%s\"%s\"",v->as.ctxstr.ctx,v->as.ctxstr.s); break;
        case V_SECRET: buf_puts(b,"Secret(<redacted>)"); break;
        case V_UNTRUSTED:{ buf_puts(b,"Untrusted("); value_to_buf(b,v->as.inner,true); buf_putc(b,')'); } break;
        case V_ARRAY:{
            buf_putc(b,'[');
            for(int i=0;i<v->as.arr.len;i++){ if(i)buf_puts(b,", "); value_to_buf(b,v->as.arr.items[i],true); }
            buf_putc(b,']');
        } break;
        case V_OK:{ buf_puts(b,"ok("); value_to_buf(b,v->as.inner,true); buf_putc(b,')'); } break;
        case V_FAIL:{ buf_puts(b,"fail("); value_to_buf(b,v->as.inner,true); buf_putc(b,')'); } break;
        case V_RECORD:{
            buf_printf(b,"%s{",v->as.rec.type?v->as.rec.type:"");
            for(int i=0;i<v->as.rec.fields.len;i++){ FieldVal*f=(FieldVal*)v->as.rec.fields.items[i];
                if(i)buf_puts(b,", "); buf_printf(b,"%s: ",f->name); value_to_buf(b,f->val,true); }
            buf_putc(b,'}');
        } break;
        case V_ENUM:{
            buf_printf(b,"%s",v->as.en.variant);
            if(v->as.en.np){ buf_putc(b,'('); for(int i=0;i<v->as.en.np;i++){ if(i)buf_puts(b,", "); value_to_buf(b,v->as.en.payload[i],true); } buf_putc(b,')'); }
        } break;
        case V_CLOSURE: buf_printf(b,"<fn %s>", v->as.clo.fn->sval?v->as.clo.fn->sval:"lambda"); break;
        case V_BUILTIN: buf_printf(b,"<builtin %s>", v->as.bi.name); break;
        case V_TYPE: buf_printf(b,"<type %s>", v->as.ty.name); break;
        case V_CAP: buf_printf(b,"<capability %s>", v->as.cap.kind); break;
    }
}
static char *value_to_cstr(Value *v, bool repr){ Buf b; buf_init(&b); value_to_buf(&b,v,repr); return b.data; }

static bool values_equal(Value *a, Value *b){
    if(a->kind!=b->kind){
        if(a->kind==V_INT&&b->kind==V_FLOAT) return (double)a->as.i==b->as.f;
        if(a->kind==V_FLOAT&&b->kind==V_INT) return a->as.f==(double)b->as.i;
        return false;
    }
    switch(a->kind){
        case V_NONE: return true;
        case V_INT: return a->as.i==b->as.i;
        case V_FLOAT: return a->as.f==b->as.f;
        case V_BOOL: return a->as.b==b->as.b;
        case V_DEC: {
            int s=a->as.dec.scale>b->as.dec.scale?a->as.dec.scale:b->as.dec.scale;
            return a->as.dec.unscaled*ipow10(s-a->as.dec.scale)==b->as.dec.unscaled*ipow10(s-b->as.dec.scale);
        }
        case V_STR: return a->as.str.len==b->as.str.len && memcmp(a->as.str.s,b->as.str.s,a->as.str.len)==0;
        case V_ARRAY:
            if(a->as.arr.len!=b->as.arr.len) return false;
            for(int i=0;i<a->as.arr.len;i++) if(!values_equal(a->as.arr.items[i],b->as.arr.items[i])) return false;
            return true;
        case V_ENUM:
            if(strcmp(a->as.en.variant,b->as.en.variant)!=0||a->as.en.np!=b->as.en.np) return false;
            for(int i=0;i<a->as.en.np;i++) if(!values_equal(a->as.en.payload[i],b->as.en.payload[i])) return false;
            return true;
        case V_RECORD:{
            if(a->as.rec.fields.len!=b->as.rec.fields.len) return false;
            for(int i=0;i<a->as.rec.fields.len;i++){
                FieldVal*fa=(FieldVal*)a->as.rec.fields.items[i];
                FieldVal*fb=(FieldVal*)b->as.rec.fields.items[i];
                if(strcmp(fa->name,fb->name)!=0||!values_equal(fa->val,fb->val)) return false;
            }
            return true;
        }
        default: return a==b;
    }
}

/* ------------------------------------------------------ checked arithmetic */
static Value *dec_normalize_pair(Value *a, Value *b, int64_t *ua, int64_t *ub, int *scale){
    int s = a->as.dec.scale>b->as.dec.scale?a->as.dec.scale:b->as.dec.scale;
    *ua = a->as.dec.unscaled*ipow10(s-a->as.dec.scale);
    *ub = b->as.dec.unscaled*ipow10(s-b->as.dec.scale);
    *scale=s; return NULL;
}
static Value *as_dec(Value *v){
    if(v->kind==V_DEC) return v;
    if(v->kind==V_INT) return mkdec(v->as.i,0);
    return NULL;
}
static double as_double(Value *v){
    if(v->kind==V_FLOAT) return v->as.f;
    if(v->kind==V_INT) return (double)v->as.i;
    if(v->kind==V_DEC) return (double)v->as.dec.unscaled/(double)ipow10(v->as.dec.scale);
    return 0;
}

static Value *binop(const char *op, Value *a, Value *b, int line){
    /* array broadcasting for arithmetic */
    if((a->kind==V_ARRAY||b->kind==V_ARRAY) &&
       (strcmp(op,"+")==0||strcmp(op,"-")==0||strcmp(op,"*")==0||strcmp(op,"/")==0)){
        if(a->kind==V_ARRAY && b->kind==V_ARRAY){
            if(a->as.arr.len!=b->as.arr.len) panic(line,"broadcast: shape mismatch (%d vs %d)",a->as.arr.len,b->as.arr.len);
            Value **out=(Value**)xalloc(sizeof(Value*)*a->as.arr.len);
            for(int i=0;i<a->as.arr.len;i++) out[i]=binop(op,a->as.arr.items[i],b->as.arr.items[i],line);
            return mkarr(out,a->as.arr.len);
        }
        Value *arr=a->kind==V_ARRAY?a:b, *sc=a->kind==V_ARRAY?b:a;
        Value **out=(Value**)xalloc(sizeof(Value*)*arr->as.arr.len);
        for(int i=0;i<arr->as.arr.len;i++)
            out[i]= (a->kind==V_ARRAY)? binop(op,arr->as.arr.items[i],sc,line)
                                      : binop(op,sc,arr->as.arr.items[i],line);
        return mkarr(out,arr->as.arr.len);
    }

    /* comparisons */
    if(strcmp(op,"==")==0) return mkbool(values_equal(a,b));
    if(strcmp(op,"!=")==0) return mkbool(!values_equal(a,b));

    /* string concat */
    if(a->kind==V_STR && b->kind==V_STR && strcmp(op,"+")==0){
        Buf bf; buf_init(&bf); buf_putn(&bf,a->as.str.s,a->as.str.len); buf_putn(&bf,b->as.str.s,b->as.str.len);
        return mkstr_n(bf.data,bf.len);
    }
    /* string ordering (lexicographic) */
    if(a->kind==V_STR && b->kind==V_STR){
        size_t m=a->as.str.len<b->as.str.len?a->as.str.len:b->as.str.len;
        int c=memcmp(a->as.str.s,b->as.str.s,m);
        if(c==0) c = (a->as.str.len<b->as.str.len)?-1:(a->as.str.len>b->as.str.len?1:0);
        if(strcmp(op,"<")==0) return mkbool(c<0);
        if(strcmp(op,"<=")==0) return mkbool(c<=0);
        if(strcmp(op,">")==0) return mkbool(c>0);
        if(strcmp(op,">=")==0) return mkbool(c>=0);
    }
    /* context-string: refuse plain string concatenation into a sink type */
    if(a->kind==V_CTXSTR || b->kind==V_CTXSTR){
        if(strcmp(op,"+")==0)
            panic(line,"a plain string cannot be concatenated into a %s context "
                       "(use parameterized interpolation)", a->kind==V_CTXSTR?a->as.ctxstr.ctx:b->as.ctxstr.ctx);
    }

    bool cmp = strcmp(op,"<")==0||strcmp(op,"<=")==0||strcmp(op,">")==0||strcmp(op,">=")==0;

    /* decimal path (exact) */
    if((a->kind==V_DEC||b->kind==V_DEC) && a->kind!=V_FLOAT && b->kind!=V_FLOAT){
        Value *da=as_dec(a), *db=as_dec(b);
        if(da&&db){
            int64_t ua,ub; int s; dec_normalize_pair(da,db,&ua,&ub,&s);
            if(strcmp(op,"+")==0) return mkdec(ua+ub,s);
            if(strcmp(op,"-")==0) return mkdec(ua-ub,s);
            if(strcmp(op,"*")==0) return mkdec(da->as.dec.unscaled*db->as.dec.unscaled, da->as.dec.scale+db->as.dec.scale);
            if(strcmp(op,"/")==0){
                if(ub==0) panic(line,"division by zero");
                /* exact when divisible at scale s; else extend precision */
                int es=s+6; int64_t num=da->as.dec.unscaled*ipow10(es-da->as.dec.scale);
                return mkdec(num/db->as.dec.unscaled, es-db->as.dec.scale<0?0:es-db->as.dec.scale);
            }
            if(cmp){ int64_t d=ua-ub;
                if(strcmp(op,"<")==0) return mkbool(d<0);
                if(strcmp(op,"<=")==0)return mkbool(d<=0);
                if(strcmp(op,">")==0) return mkbool(d>0);
                return mkbool(d>=0);
            }
        }
    }

    /* float path */
    if(a->kind==V_FLOAT||b->kind==V_FLOAT){
        double x=as_double(a),y=as_double(b);
        if(strcmp(op,"+")==0)return mkfloat(x+y);
        if(strcmp(op,"-")==0)return mkfloat(x-y);
        if(strcmp(op,"*")==0)return mkfloat(x*y);
        if(strcmp(op,"/")==0){ if(y==0)panic(line,"division by zero"); return mkfloat(x/y); }
        if(strcmp(op,"%")==0)return mkfloat(fmod(x,y));
        if(strcmp(op,"**")==0)return mkfloat(pow(x,y));
        if(strcmp(op,"<")==0)return mkbool(x<y);
        if(strcmp(op,"<=")==0)return mkbool(x<=y);
        if(strcmp(op,">")==0)return mkbool(x>y);
        if(strcmp(op,">=")==0)return mkbool(x>=y);
    }

    /* int path — CHECKED (no silent wraparound) */
    if(a->kind==V_INT&&b->kind==V_INT){
        int64_t x=a->as.i,y=b->as.i,r;
        if(strcmp(op,"+")==0){ if(__builtin_add_overflow(x,y,&r)) panic(line,"integer overflow in %lld + %lld",(long long)x,(long long)y); return mkint(r); }
        if(strcmp(op,"-")==0){ if(__builtin_sub_overflow(x,y,&r)) panic(line,"integer overflow in %lld - %lld",(long long)x,(long long)y); return mkint(r); }
        if(strcmp(op,"*")==0){ if(__builtin_mul_overflow(x,y,&r)) panic(line,"integer overflow in %lld * %lld",(long long)x,(long long)y); return mkint(r); }
        if(strcmp(op,"/")==0){ if(y==0)panic(line,"division by zero"); return mkint(x/y); }
        if(strcmp(op,"%")==0){ if(y==0)panic(line,"division by zero"); return mkint(x%y); }
        if(strcmp(op,"**")==0){ int64_t rr=1; for(int64_t k=0;k<y;k++){ if(__builtin_mul_overflow(rr,x,&rr)) panic(line,"integer overflow in exponentiation"); } return mkint(rr); }
        if(strcmp(op,"&")==0)return mkint(x&y);
        if(strcmp(op,"|")==0)return mkint(x|y);
        if(strcmp(op,"^")==0)return mkint(x^y);
        if(strcmp(op,"<<")==0)return mkint(x<<y);
        if(strcmp(op,">>")==0)return mkint(x>>y);
        if(strcmp(op,"<")==0)return mkbool(x<y);
        if(strcmp(op,"<=")==0)return mkbool(x<=y);
        if(strcmp(op,">")==0)return mkbool(x>y);
        if(strcmp(op,">=")==0)return mkbool(x>=y);
    }
    panic(line,"unsupported operands for '%s'",op);
    return VNONE;
}

/* ------------------------------------------------------- construction etc. */
static FieldVal *mk_fieldval(const char *name, Value *v){
    FieldVal *f=(FieldVal*)xalloc(sizeof(FieldVal)); f->name=xstrdup(name); f->val=v; return f;
}
static Value *record_get(Value *rec, const char *name){
    for(int i=0;i<rec->as.rec.fields.len;i++){ FieldVal*f=(FieldVal*)rec->as.rec.fields.items[i]; if(strcmp(f->name,name)==0) return f->val; }
    return NULL;
}
static void record_set(Value *rec, const char *name, Value *v){
    for(int i=0;i<rec->as.rec.fields.len;i++){ FieldVal*f=(FieldVal*)rec->as.rec.fields.items[i]; if(strcmp(f->name,name)==0){ f->val=v; return; } }
    vec_push(&rec->as.rec.fields, mk_fieldval(name,v));
}

static bool check_refinement(Node *refine, Value *v, int line){
    if(!refine) return true;
    Env *e=env_new(NULL);
    env_define(e,"__subj__",v,false);
    int64_t length = (v->kind==V_ARRAY)?v->as.arr.len : (v->kind==V_STR)?(int64_t)v->as.str.len : 0;
    env_define(e,"len",mkint(length),false);
    env_define(e,"size",mkint(length),false);
    env_define(e,"count",mkint(length),false);
    Value *r=eval(refine,e);
    return value_truthy(r);
}
static Value *enforce_type(Node *type, Value *v, int line, const char *what){
    if(!type) return v;
    /* validation boundary: a refined/plain target unwraps Untrusted */
    if(v->kind==V_UNTRUSTED) v=v->as.inner;
    /* numeric coercion toward the declared type */
    const char *tn = type->sval;
    if(tn){
        if((!strcmp(tn,"f64")||!strcmp(tn,"f32")||!strcmp(tn,"float")) &&
           (v->kind==V_INT||v->kind==V_DEC)) v=mkfloat(as_double(v));
        else if(!strcmp(tn,"decimal") && v->kind==V_INT) v=mkdec(v->as.i,0);
        else if(!strcmp(tn,"int") && v->kind==V_DEC && v->as.dec.unscaled%ipow10(v->as.dec.scale)==0)
            v=mkint(v->as.dec.unscaled/ipow10(v->as.dec.scale));
    }
    if(type->refine && !check_refinement(type->refine, v, line))
        panic(line,"refinement violated for %s: value %s fails `where` predicate",
              what, value_to_cstr(v,true));
    return v;
}

/* construct a struct/class instance from a decl + args */
static Value *construct(Node *decl, Value **args, int nargs, Vec *named, int line){
    Value *rec=mkval(V_RECORD); rec->as.rec.type=decl->sval; vec_init(&rec->as.rec.fields);
    int fi=0;
    /* a single record-literal positional arg supplies named fields */
    for(int i=0;i<decl->list.len;i++){
        Node *fd=(Node*)decl->list.items[i];
        Value *val=NULL;
        if(named){
            for(int j=0;j<named->len;j++){ FieldVal*nf=(FieldVal*)named->items[j]; if(strcmp(nf->name,fd->sval)==0){ val=nf->val; break; } }
        }
        if(!val && fi<nargs) val=args[fi++];
        if(!val && fd->a) val=eval(fd->a,env_new(NULL));   /* default */
        if(!val) val=VNONE;
        val=enforce_type(fd->type,val,line,fd->sval);
        record_set(rec,fd->sval,val);
    }
    return rec;
}

/* match `check` arm pattern against value; on success define bindings in `out` */
static bool match_pattern(Node *arm, Value *v, Env *out){
    if(arm->sval && strcmp(arm->sval,"__else__")==0) return true;
    if(arm->sval && strcmp(arm->sval,"none")==0) return v->kind==V_NONE;
    if(arm->sval && strcmp(arm->sval,"some")==0){
        if(v->kind==V_NONE) return false;
        if(arm->list.len==1){ Node*b=(Node*)arm->list.items[0];
            env_define(out,b->sval, v->kind==V_OK?v->as.inner:v, false); }
        return true;
    }
    if(arm->sval){ /* variant pattern */
        if(v->kind==V_ENUM && strcmp(v->as.en.variant,arm->sval)==0){
            for(int i=0;i<arm->list.len && i<v->as.en.np;i++){
                Node*b=(Node*)arm->list.items[i];
                env_define(out,b->sval,v->as.en.payload[i],false);
            }
            return true;
        }
        if(v->kind==V_OK && strcmp(arm->sval,"ok")==0){
            if(arm->list.len==1){ Node*b=(Node*)arm->list.items[0]; env_define(out,b->sval,v->as.inner,false); }
            return true;
        }
        if(v->kind==V_FAIL && strcmp(arm->sval,"fail")==0){
            if(arm->list.len==1){ Node*b=(Node*)arm->list.items[0]; env_define(out,b->sval,v->as.inner,false); }
            return true;
        }
        return false;
    }
    if(arm->b){ Value *lit=eval(arm->b,out); return values_equal(v,lit); }
    if(arm->c){ env_define(out,arm->c->sval,v,false); return true; } /* binding */
    return false;
}

/* call a user closure: bind params, install Fail boundary, run body */
static Value *call_closure(Value *clo, Value **args, int nargs, int line, const char *self_name, Value *self_val){
    Node *fn=clo->as.clo.fn;
    Env *fenv=env_new(clo->as.clo.env);
    Vec *params=&fn->list2;
    int pi=0;
    if(self_name){ env_define(fenv,"self",self_val,true); }
    /* skip a leading `self` param if present in signature */
    int start=0;
    if(params->len>0 && strcmp(((Node*)params->items[0])->sval,"self")==0) start=1;
    for(int i=start;i<params->len;i++){
        Node *par=(Node*)params->items[i];
        Value *val=NULL;
        if(pi<nargs) val=args[pi++];
        else if(par->a) val=eval(par->a,fenv);   /* default */
        else val=VNONE;
        val=enforce_type(par->type,val,line,par->sval);
        env_define(fenv,par->sval,val,par->is_mut);
    }
    /* implicit Fail boundary: an unhandled Fail becomes a V_FAIL return */
    Handler hb; memset(&hb,0,sizeof hb);
    hb.effect="Fail"; hb.is_fn_boundary=true; hb.env=fenv; vec_init(&hb.clauses);
    Value *ret;
    push_handler(&hb);
    if(setjmp(hb.abort_buf)==0){
        Ctl saved=g_ctl; g_ctl=CTL_NONE;
        Value *body = (fn->a->kind==N_BLOCK)? eval_block(fn->a,fenv) : eval(fn->a,fenv);
        Value *rv = (g_ctl==CTL_RETURN)? g_ctl_val : body;
        g_ctl=saved;
        pop_handler(&hb);
        ret=rv?rv:VNONE;
    } else {
        pop_handler(&hb);
        ret=hb.result;     /* V_FAIL(e) */
    }
    return ret;
}

static Value *mk_cap(const char *type){
    Value *v=mkval(V_RECORD); v->as.rec.type=xstrdup(type); vec_init(&v->as.rec.fields); return v;
}
static bool is_builtin_type(const char *t){
    static const char *names[]={"System","Dir","Net","Db","File","Listener",
        "Request","Response","Namespace","Scope",NULL};
    for(int i=0;names[i];i++) if(t&&strcmp(t,names[i])==0) return true;
    return false;
}

static Value *call_value(Value *callee, Value **args, int nargs, int line, Env *callsite){
    if(callee->kind==V_CLOSURE) return call_closure(callee,args,nargs,line,NULL,NULL);
    if(callee->kind==V_BUILTIN) return callee->as.bi.fn(args,nargs,callsite);
    panic(line,"value of kind %d is not callable",callee->kind);
    return VNONE;
}

/* 2.1 real path containment: canonicalize `req` *relative to* `root`, rejecting
 * any `..` segment that would pop above the capability root. This both
 * over-rejects nothing legitimate (a name merely containing ".." is fine) and
 * under-rejects nothing dangerous (absolute escape, `foo/../../etc`, `.`-games).
 * On escape it returns NULL; the caller raises a *catchable* Fail (1.3). */
static char *path_contain(const char *root, const char *req){
    /* split root into segments */
    Vec seg; vec_init(&seg);
    char *rcopy=xstrdup(root);
    for(char *p=strtok(rcopy,"/"); p; p=strtok(NULL,"/")) vec_push(&seg,p);
    int root_depth=seg.len;
    /* walk requested segments relative to root; a leading '/' is treated as
       relative to the capability root, never the real filesystem root */
    char *qcopy=xstrdup(req);
    for(char *p=strtok(qcopy,"/"); p; p=strtok(NULL,"/")){
        if(strcmp(p,".")==0||p[0]==0) continue;
        if(strcmp(p,"..")==0){
            if(seg.len>root_depth) seg.len--;   /* pop inside the cap */
            else return NULL;                   /* escape above the root */
        } else vec_push(&seg,p);
    }
    Buf b; buf_init(&b);
    if(root[0]=='/'||seg.len==0) buf_putc(&b,'/');
    for(int i=0;i<seg.len;i++){ if(i)buf_putc(&b,'/'); buf_puts(&b,(char*)seg.items[i]); }
    if(seg.len==0 && root[0]!='/') buf_puts(&b,".");
    return b.data;
}

static Value *mk_cap(const char *type);
static Value *record_get(Value *rec, const char *name);
static void   record_set(Value *rec, const char *name, Value *v);
static Value *perform_fail(Value *err, int line);

/* capability + builtin-type method dispatch */
static Value *cap_method(Value *recv, const char *method, Value **args, int nargs, int line){
    const char *t=recv->as.rec.type;
    if(strcmp(t,"Dir")==0){
        Value *rootv=record_get(recv,"root"); const char *root=rootv?rootv->as.str.s:"/";
        if(strcmp(method,"subtree")==0||strcmp(method,"path")==0){
            const char *p=args[0]->as.str.s;
            char *resolved=path_contain(root,p);
            if(!resolved)
                return perform_fail(mkstr("path traversal rejected: escapes capability root"),line);
            Value *d=mk_cap("Dir"); record_set(d,"root",mkstr(resolved)); return d;
        }
        if(strcmp(method,"open")==0){
            const char *p=args[0]->as.str.s;
            char *resolved=path_contain(root,p);
            if(!resolved)
                return perform_fail(mkstr("path traversal rejected: escapes capability root"),line);
            Value *f=mk_cap("File"); record_set(f,"name",mkstr(resolved));
            /* 2.1: real I/O — read actual bytes from disk within the cap root.
               A test seed (second arg) overrides for hermetic conformance cases. */
            if(nargs>1){ record_set(f,"content",args[1]); }
            else {
                FILE *fp=fopen(resolved,"rb");
                if(!fp) return perform_fail(mkstr("no such file or unreadable within capability"),line);
                fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
                char *buf=(char*)xalloc(n+1); size_t got=fread(buf,1,n,fp); buf[got]=0; fclose(fp);
                record_set(f,"content",mkstr_n(buf,got));
            }
            return mkok(f);
        }
        if(strcmp(method,"root_path")==0){ return mkstr(root); }
    } else if(strcmp(t,"File")==0){
        if(strcmp(method,"read")==0){ Value *c=record_get(recv,"content"); return mkok(c?c:mkstr("")); }
        if(strcmp(method,"name")==0){ Value *c=record_get(recv,"name"); return c?c:mkstr(""); }
    } else if(strcmp(t,"Net")==0){
        if(strcmp(method,"database")==0){ Value *db=mk_cap("Db"); record_set(db,"rows",mkarr(NULL,0)); record_set(db,"url",args[0]); return db; }
        if(strcmp(method,"listen")==0){ Value *l=mk_cap("Listener"); record_set(l,"port",args[0]); return l; }
    } else if(strcmp(t,"Db")==0){
        if(strcmp(method,"run")==0){
            if(nargs<1 || (args[0]->kind!=V_CTXSTR)){
                PANIC_CODE("type.sink");
                panic(line,"db.run requires a parameterized `sql\"...\"` value, not a plain string "
                           "(injection cannot type-check)");
            }
            Value *rows=record_get(recv,"rows");
            if(!rows) return mkok(mkarr(NULL,0));
            /* stand-in query execution: if the sql has `id = N`, filter rows by id */
            const char *q=args[0]->as.ctxstr.s;
            const char *eq=strstr(q,"id = ");
            if(eq && rows->kind==V_ARRAY){
                long long want=strtoll(eq+5,NULL,10);
                int c=0; Value**out=(Value**)xalloc(sizeof(Value*)*rows->as.arr.len);
                for(int i=0;i<rows->as.arr.len;i++){ Value*r=rows->as.arr.items[i];
                    Value*idf = r->kind==V_RECORD? record_get(r,"id") : NULL;
                    if(idf && idf->kind==V_INT && idf->as.i==want) out[c++]=r; }
                return mkok(mkarr(out,c));
            }
            return mkok(rows);
        }
    } else if(strcmp(t,"Request")==0){
        if(strcmp(method,"query")==0){
            Value *qm=record_get(recv,"query"); Value *raw=NULL;
            if(qm&&qm->kind==V_RECORD) raw=record_get(qm,args[0]->as.str.s);
            Value *u=mkval(V_UNTRUSTED); u->as.inner=raw?raw:mkstr(""); return u; /* Untrusted<string> */
        }
    } else if(strcmp(t,"Response")==0||strcmp(t,"Namespace")==0){
        /* handled via field builtins */
    } else if(strcmp(t,"Scope")==0){
        if(strcmp(method,"spawn")==0) return VNONE;   /* structured task (run inline) */
    }
    PANIC_CODE("type.method");
    panic(line,"no method `%s` on %s",method,t);
    return VNONE;
}

/* ===================================================================== eval */
static void interp_escape(Buf *b, const char *ctx, Value *v){
    /* context-aware escaping of interpolated data into a typed string —
       delegated to the C5 single-source so `keel run` and compiled binaries
       escape identically. */
    char *s = value_to_cstr(v,false);
    char *e = kesc_for(ctx, s);
    buf_puts(b, e);
    free(e);
}

static Value *eval_field(Node *n, Env *env){
    Value *recv=eval(n->a,env);
    const char *name=n->sval;
    if(recv->kind==V_RECORD){
        /* capability sub-accessors */
        if(strcmp(recv->as.rec.type,"System")==0){
            Value *f=record_get(recv,name);
            if(f) return f;
        }
        Value *f=record_get(recv,name);
        if(f) return f;
        /* method as bound value? fall through to none */
        PANIC_CODE("type.field"); panic(n->line,"no field `%s` on %s",name,recv->as.rec.type[0]?recv->as.rec.type:"record");
    }
    if(recv->kind==V_ENUM){
        /* access payload by index name not supported; expose .name */
        if(strcmp(name,"name")==0) return mkstr(recv->as.en.variant);
    }
    if(recv->kind==V_NONE) return VNONE;            /* lenient */
    if(recv->kind==V_ARRAY){
        if(strcmp(name,"len")==0||strcmp(name,"size")==0||strcmp(name,"count")==0) return mkint(recv->as.arr.len);
    }
    if(recv->kind==V_STR){
        if(strcmp(name,"len")==0||strcmp(name,"size")==0) return mkint((int64_t)recv->as.str.len);
    }
    panic(n->line,"cannot access field `%s`",name);
    return VNONE;
}

static Value *eval(Node *n, Env *env){
    if(!n) return VNONE;
    switch(n->kind){
        case N_INT: return mkint(n->ival);
        case N_FLOAT: return mkfloat(n->fval);
        case N_DECIMAL: return mkdec(n->dec_unscaled,n->dec_scale);
        case N_BOOL: return mkbool(n->bval);
        case N_NONE: return VNONE;
        case N_STRING: return mkstr(n->sval);
        case N_INTERP:{
            Buf b; buf_init(&b);
            for(int i=0;i<n->list.len;i++){
                Node *part=(Node*)n->list.items[i];
                if(part->kind==N_STRING) buf_puts(&b,part->sval);
                else {
                    Value *v=eval(part,env);
                    if(n->tag) interp_escape(&b,n->tag,v);
                    else { char *s=value_to_cstr(v,false); buf_puts(&b,s); }
                }
            }
            if(n->tag) return mkctx(n->tag,b.data);
            return mkstr_n(b.data,b.len);
        }
        case N_IDENT:{
            Binding *bd=env_find(env,n->sval);
            if(bd) return bd->val;
            if(find_type(n->sval)){ Value*t=mkval(V_TYPE); t->as.ty.name=n->sval; t->as.ty.decl=find_type(n->sval); return t; }
            if(find_enum_of_variant(n->sval)){ Value*e=mkval(V_ENUM); e->as.en.type=NULL; e->as.en.variant=n->sval; e->as.en.np=0; e->as.en.payload=NULL; return e; }
            PANIC_CODE("name.undefined");
            panic(n->line,"undefined name `%s`",n->sval);
        }
        case N_ARRAY:{
            int m=n->list.len; Value**items=m?(Value**)xalloc(sizeof(Value*)*m):NULL;
            for(int i=0;i<m;i++) items[i]=eval((Node*)n->list.items[i],env);
            return mkarr(items,m);
        }
        case N_RECORD:{
            Value *rec=mkval(V_RECORD); rec->as.rec.type=xstrdup(""); vec_init(&rec->as.rec.fields);
            for(int i=0;i<n->list.len;i++){ Node*kv=(Node*)n->list.items[i];
                record_set(rec,kv->sval,eval(kv->a,env)); }
            return rec;
        }
        case N_RANGE:{
            Value *a=eval(n->a,env),*b=eval(n->b,env);
            if(a->kind!=V_INT||b->kind!=V_INT) panic(n->line,"range bounds must be int");
            int64_t lo=a->as.i,hi=b->as.i; if(hi<lo)hi=lo;
            int len=(int)(hi-lo); Value**items=len?(Value**)xalloc(sizeof(Value*)*len):NULL;
            for(int i=0;i<len;i++) items[i]=mkint(lo+i);
            return mkarr(items,len);
        }
        case N_LAMBDA:{
            Value *c=mkval(V_CLOSURE); c->as.clo.fn=n; c->as.clo.env=env;
            /* lambda params live in n->list; reuse list2 convention for call */
            n->list2=n->list; if(!n->a){} return c;
        }
        case N_INDEX:{
            Value *base=eval(n->a,env);
            Value *idx=eval((Node*)n->list.items[0],env);
            if(base->kind==V_ARRAY){
                int64_t i=idx->as.i; if(i<0)i+=base->as.arr.len;
                if(i<0||i>=base->as.arr.len) panic(n->line,"index %lld out of bounds (len %d)",(long long)idx->as.i,base->as.arr.len);
                return base->as.arr.items[i];
            }
            if(base->kind==V_STR){
                int64_t i=idx->as.i; if(i<0)i+=base->as.str.len;
                if(i<0||i>=(int64_t)base->as.str.len) panic(n->line,"index out of bounds");
                return mkstr_n(base->as.str.s+i,1);
            }
            if(base->kind==V_RECORD){ Value*f=record_get(base, value_to_cstr(idx,false)); return f?f:VNONE; }
            panic(n->line,"cannot index value");
        }
        case N_SLICE:{
            Value *base=eval(n->a,env);
            /* expects [lo -> hi] encoded as a single RANGE arg, or `..` */
            Node *spec=(Node*)n->list.items[0];
            int64_t lo=0,hi;
            if(base->kind==V_ARRAY) hi=base->as.arr.len; else if(base->kind==V_STR) hi=base->as.str.len; else { panic(n->line,"cannot slice"); return VNONE; }
            if(spec->kind==N_RANGE){ lo=eval(spec->a,env)->as.i; hi=eval(spec->b,env)->as.i; }
            if(lo<0)lo=0; if(hi>(base->kind==V_ARRAY?base->as.arr.len:(int64_t)base->as.str.len)) hi=(base->kind==V_ARRAY?base->as.arr.len:(int64_t)base->as.str.len);
            if(base->kind==V_ARRAY){ int len=(int)(hi-lo); if(len<0)len=0;
                Value**it=len?(Value**)xalloc(sizeof(Value*)*len):NULL;
                for(int i=0;i<len;i++) it[i]=base->as.arr.items[lo+i]; return mkarr(it,len); }
            int len=(int)(hi-lo); if(len<0)len=0; return mkstr_n(base->as.str.s+lo,len);
        }
        case N_FIELD: return eval_field(n,env);
        case N_QDOT:{
            Value *recv=eval(n->a,env);
            if(recv->kind==V_NONE) return VNONE;
            Node tmp=*n; tmp.kind=N_FIELD; return eval_field(&tmp,env);
        }

        case N_CALL:{
            /* gather args: positional + named */
            int cap_n=n->list.len; 
            Value **args=cap_n?(Value**)xalloc(sizeof(Value*)*cap_n):NULL;
            int nargs=0; Vec named; vec_init(&named);
            for(int i=0;i<n->list.len;i++){
                Node *a=(Node*)n->list.items[i];
                if(a->kind==N_NAMEDARG) vec_push(&named, mk_fieldval(a->sval, eval(a->a,env)));
                else args[nargs++]=eval(a,env);
            }
            Node *callee=n->a;
            if(callee->kind==N_FIELD){
                Value *recv=eval(callee->a,env);
                const char *m=callee->sval;
                if(recv->kind==V_RECORD){
                    Value *member=record_get(recv,m);
                    if(member&&(member->kind==V_CLOSURE||member->kind==V_BUILTIN))
                        return call_value(member,args,nargs,n->line,env);
                    if(is_builtin_type(recv->as.rec.type))
                        return cap_method(recv,m,args,nargs,n->line);
                    /* user-defined method on struct/class */
                    Node *decl=find_type(recv->as.rec.type);
                    if(decl){
                        for(int i=0;i<decl->list2.len;i++){ Node*md=(Node*)decl->list2.items[i];
                            if(strcmp(md->sval,m)==0){
                                Value *clo=mkval(V_CLOSURE); clo->as.clo.fn=md; clo->as.clo.env=g_genv;
                                return call_closure(clo,args,nargs,n->line,"self",recv);
                            } }
                    }
                    PANIC_CODE("type.method"); panic(n->line,"no method `%s` on %s",m,recv->as.rec.type);
                }
                /* primitive/builtin methods (arrays, strings, enums) */
                return builtin_method(recv,m,args,nargs,n->line);
            }
            if(callee->kind==N_IDENT){
                const char *name=callee->sval;
                Binding *bd=env_find(env,name);
                if(!bd){
                    Node *ty=find_type(name);
                    if(ty&&(ty->kind==N_STRUCT||ty->kind==N_CLASS))
                        return construct(ty,args,nargs,&named,n->line);
                    Node *en=find_enum_of_variant(name);
                    if(en){
                        Value *ev=mkval(V_ENUM); ev->as.en.type=en->sval; ev->as.en.variant=xstrdup(name);
                        ev->as.en.np=nargs; ev->as.en.payload=args; 
                        /* enforce variant field refinements */
                        for(int i=0;i<en->list.len;i++){ Node*v=(Node*)en->list.items[i];
                            if(strcmp(v->sval,name)==0){ for(int k=0;k<v->list.len&&k<nargs;k++){
                                Node*fd=(Node*)v->list.items[k]; args[k]=enforce_type(fd->type,args[k],n->line,fd->sval); } } }
                        return ev;
                    }
                }
                Value *fn=eval(callee,env);
                return call_value(fn,args,nargs,n->line,env);
            }
            Value *fn=eval(callee,env);
            return call_value(fn,args,nargs,n->line,env);
        }
        case N_UNARY:{
            if(strcmp(n->sval,"resume")==0){ Value *v=n->a?eval(n->a,env):VNONE; return do_resume(v,n->line); }
            if(strcmp(n->sval,"reflect")==0){
                Value *v=eval(n->a,env);
                Value *r=mkval(V_RECORD); r->as.rec.type=xstrdup("TypeInfo"); vec_init(&r->as.rec.fields);
                const char *tn; switch(v->kind){case V_INT:tn="int";break;case V_FLOAT:tn="f64";break;
                    case V_STR:tn="string";break;case V_BOOL:tn="bool";break;case V_ARRAY:tn="array";break;
                    case V_DEC:tn="decimal";break;case V_RECORD:tn=v->as.rec.type;break;case V_ENUM:tn=v->as.en.type?v->as.en.type:"enum";break;
                    default:tn="value";}
                record_set(r,"name",mkstr(tn)); return r;
            }
            Value *v=eval(n->a,env);
            if(strcmp(n->sval,"not")==0) return mkbool(!value_truthy(v));
            if(strcmp(n->sval,"-")==0){ if(v->kind==V_INT)return mkint(-v->as.i); if(v->kind==V_FLOAT)return mkfloat(-v->as.f); if(v->kind==V_DEC)return mkdec(-v->as.dec.unscaled,v->as.dec.scale); panic(n->line,"cannot negate"); }
            if(strcmp(n->sval,"~")==0){ if(v->kind==V_INT)return mkint(~v->as.i); panic(n->line,"~ requires int"); }
            if(strcmp(n->sval,"&")==0||strcmp(n->sval,"&mut")==0||strcmp(n->sval,"*")==0) return v; /* borrows are identity at runtime */
            panic(n->line,"unknown unary op %s",n->sval);
        }
        case N_BINARY: return binop(n->sval, eval(n->a,env), eval(n->b,env), n->line);
        case N_LOGICAL:{
            Value *a=eval(n->a,env);
            if(strcmp(n->sval,"and")==0) return value_truthy(a)? eval(n->b,env) : a;
            return value_truthy(a)? a : eval(n->b,env);   /* or */
        }
        case N_TERNARY: return value_truthy(eval(n->a,env))? eval(n->b,env) : eval(n->c,env);
        case N_QQ:{ Value *a=eval(n->a,env); return (a->kind==V_NONE)? eval(n->b,env) : a; }
        case N_TRY:{
            Value *v=eval(n->a,env);
            if(v->kind==V_FAIL) return perform_fail(v->as.inner,n->line);
            if(v->kind==V_OK) return v->as.inner;
            return v;
        }

        case N_LET:{
            Value *v=eval(n->a,env);
            v=enforce_type(n->type,v,n->line,n->sval);
            if(!n->type && n->refine) v=enforce_type(n,v,n->line,n->sval);
            Binding *bd=env_find(env,n->sval);
            if(bd && !n->is_mut){
                if(!bd->is_mut) { env_define(env,n->sval,v,false); }  /* shadow in scope */
                else bd->val=v;
            } else {
                env_define(env,n->sval,v,n->is_mut);
            }
            return VNONE;
        }
        case N_ASSIGN:{
            Node *lhs=n->a; const char *op=n->sval;
            Value *rhs = n->b? eval(n->b,env) : NULL;
            if(lhs->kind==N_IDENT){
                Binding *bd=env_find(env,lhs->sval);
                if(!bd) panic(n->line,"assignment to undefined `%s`",lhs->sval);
                if(!bd->is_mut) panic(n->line,"cannot reassign immutable binding `%s` (use `mut`)",lhs->sval);
                Value *nv;
                if(strcmp(op,"=")==0) nv=rhs;
                else if(strcmp(op,"++")==0) nv=binop("+",bd->val,mkint(1),n->line);
                else if(strcmp(op,"--")==0) nv=binop("-",bd->val,mkint(1),n->line);
                else { char o[2]={op[0],0}; nv=binop(o,bd->val,rhs,n->line); }
                bd->val=nv; return VNONE;
            }
            if(lhs->kind==N_FIELD){
                Value *recv=eval(lhs->a,env);
                if(recv->kind!=V_RECORD) panic(n->line,"cannot assign field of non-record");
                Value *old=record_get(recv,lhs->sval);
                Value *nv;
                if(strcmp(op,"=")==0) nv=rhs;
                else if(strcmp(op,"++")==0) nv=binop("+",old,mkint(1),n->line);
                else if(strcmp(op,"--")==0) nv=binop("-",old,mkint(1),n->line);
                else { char o[2]={op[0],0}; nv=binop(o,old,rhs,n->line); }
                record_set(recv,lhs->sval,nv); return VNONE;
            }
            if(lhs->kind==N_INDEX){
                Value *base=eval(lhs->a,env);
                Value *idx=eval((Node*)lhs->list.items[0],env);
                if(base->kind!=V_ARRAY) panic(n->line,"cannot index-assign");
                int64_t i=idx->as.i; if(i<0)i+=base->as.arr.len;
                if(i<0||i>=base->as.arr.len) panic(n->line,"index out of bounds");
                Value *old=base->as.arr.items[i],*nv;
                if(strcmp(op,"=")==0) nv=rhs;
                else if(strcmp(op,"++")==0) nv=binop("+",old,mkint(1),n->line);
                else if(strcmp(op,"--")==0) nv=binop("-",old,mkint(1),n->line);
                else { char o[2]={op[0],0}; nv=binop(o,old,rhs,n->line); }
                base->as.arr.items[i]=nv; return VNONE;
            }
            panic(n->line,"invalid assignment target"); return VNONE;
        }
        case N_EXPRSTMT: return eval(n->a,env);
        case N_RETURN:{ g_ctl_val = n->a? eval(n->a,env) : VNONE; g_ctl=CTL_RETURN; return g_ctl_val; }
        case N_BREAK: g_ctl=CTL_BREAK; return VNONE;
        case N_CONTINUE: g_ctl=CTL_CONTINUE; return VNONE;
        case N_ASSERT:{
            Value *v=eval(n->a,env);
            if(!value_truthy(v)) panic(n->line,"assertion failed");
            return VNONE;
        }
        case N_BLOCK: return eval_block(n,env);
        case N_IF:{
            if(value_truthy(eval(n->a,env))) return eval_block(n->b,env);
            for(int i=0;i<n->list2.len;i++){ Node*ei=(Node*)n->list2.items[i];
                if(value_truthy(eval(ei->a,env))) return eval_block(ei->b,env); }
            if(n->c) return eval_block(n->c,env);
            return VNONE;
        }
        case N_CHECK:{
            Value *subj=eval(n->a,env);
            for(int i=0;i<n->list.len;i++){
                Node *arm=(Node*)n->list.items[i];
                Env *ae=env_new(env);
                if(match_pattern(arm,subj,ae)) return eval_block(arm->a,ae);
            }
            return VNONE;
        }
        case N_LOOP_TILL:{
            Env *le=env_new(env);
            if(n->b) eval(n->b,le);                 /* init */
            while(!value_truthy(eval(n->a,le))){    /* loop until condition true */
                eval_block(n->d, env_new(le));      /* fresh scope per iteration */
                if(g_ctl==CTL_BREAK){ g_ctl=CTL_NONE; break; }
                if(g_ctl==CTL_CONTINUE) g_ctl=CTL_NONE;
                if(g_ctl==CTL_RETURN) break;
                if(n->c) eval(n->c,le);             /* step */
            }
            return VNONE;
        }
        case N_LOOP_THROUGH:{
            Value *it=eval(n->a,env);
            if(it->kind!=V_ARRAY) panic(n->line,"`loop through` requires an array");
            for(int i=0;i<it->as.arr.len;i++){
                Env *le=env_new(env);
                env_define(le,"it",it->as.arr.items[i],false);
                env_define(le,"i",mkint(i),false);
                eval_block(n->b,le);
                if(g_ctl==CTL_BREAK){ g_ctl=CTL_NONE; break; }
                if(g_ctl==CTL_CONTINUE) g_ctl=CTL_NONE;
                if(g_ctl==CTL_RETURN) break;
            }
            return VNONE;
        }
        case N_LOOP_INF:{
            for(;;){
                eval_block(n->a, env_new(env));     /* fresh scope per iteration */
                if(g_ctl==CTL_BREAK){ g_ctl=CTL_NONE; break; }
                if(g_ctl==CTL_CONTINUE) g_ctl=CTL_NONE;
                if(g_ctl==CTL_RETURN) break;
            }
            return VNONE;
        }
        case N_DEF:{
            Value *c=mkval(V_CLOSURE); c->as.clo.fn=n; c->as.clo.env=env;
            env_define(env,n->sval,c,false);
            return VNONE;
        }
        case N_STRUCT: case N_ENUM: case N_CLASS: case N_INTERFACE:
            return VNONE;       /* registered in pre-pass */
        case N_TEST: return VNONE;     /* collected by the test runner */
        case N_EXECUTE: return eval_execute(n,env);
        case N_REGION: return eval_block(n->a,env);      /* region freed at scope end */
        case N_COMPTIME: return eval_block(n->a,env);    /* evaluated now */
        case N_UNSAFE: return eval_block(n->a,env);
        case N_PARALLEL:{
            Env *pe=env_new(env);
            Value *scope=mk_cap("Scope"); record_set(scope,"name",mkstr(n->sval?n->sval:"s"));
            env_define(pe,n->sval?n->sval:"s",scope,false);
            return eval_block(n->a,pe);                  /* structured: run sequentially */
        }
        case N_IMPORT: case N_MODULE: case N_EXTERN: return VNONE;
        default:
            panic(n->line,"cannot evaluate node kind %d",n->kind);
    }
    return VNONE;
}

static Value *eval_block(Node *blk, Env *env){
    Value *last=VNONE;
    Env *be=env;
    for(int i=0;i<blk->list.len;i++){
        last=eval((Node*)blk->list.items[i], be);
        if(g_ctl!=CTL_NONE) return last;
    }
    return last;
}

static Value *eval_execute(Node *n, Env *env){
    Handler h; memset(&h,0,sizeof h);
    h.effect = n->list2.len ? ((Node*)n->list2.items[0])->sval : "Fail";
    h.env=env; h.clauses=n->list2; h.is_fn_boundary=false;
    push_handler(&h);
    if(setjmp(h.abort_buf)==0){
        Value *res=eval_block(n->a,env);
        pop_handler(&h);
        return res;
    } else {
        pop_handler(&h);
        return h.result;          /* value produced by a handler clause */
    }
}

/* ---------------------------------------------------- primitive methods */
static Value *arr_push_inplace(Value *arr, Value *x){
    if(arr->as.arr.len >= arr->as.arr.cap){
        int nc = arr->as.arr.cap? arr->as.arr.cap*2 : 4;
        Value **ni=(Value**)xalloc(sizeof(Value*)*nc);
        for(int i=0;i<arr->as.arr.len;i++) ni[i]=arr->as.arr.items[i];
        arr->as.arr.items=ni; arr->as.arr.cap=nc;
    }
    arr->as.arr.items[arr->as.arr.len]=x; arr->as.arr.len++;
    return arr;
}
static Value *builtin_method(Value *recv, const char *m, Value **args, int nargs, int line){
    /* parsing untrusted input IS the validation step; methods see the inner value */
    if(recv->kind==V_UNTRUSTED) recv=recv->as.inner;
    if(recv->kind==V_ARRAY){
        int N=recv->as.arr.len; Value **it=recv->as.arr.items;
        if(!strcmp(m,"len")||!strcmp(m,"size")||!strcmp(m,"count")) return mkint(N);
        if(!strcmp(m,"first")) return N? it[0] : VNONE;
        if(!strcmp(m,"last"))  return N? it[N-1] : VNONE;
        if(!strcmp(m,"is_empty")) return mkbool(N==0);
        if(!strcmp(m,"push")){ return arr_push_inplace(recv,args[0]); }
        if(!strcmp(m,"contains")){ for(int i=0;i<N;i++) if(values_equal(it[i],args[0])) return mkbool(true); return mkbool(false); }
        if(!strcmp(m,"reverse")){ Value**o=N?(Value**)xalloc(sizeof(Value*)*N):NULL; for(int i=0;i<N;i++)o[i]=it[N-1-i]; return mkarr(o,N); }
        if(!strcmp(m,"map")){ Value**o=N?(Value**)xalloc(sizeof(Value*)*N):NULL; for(int i=0;i<N;i++){ Value*a=it[i]; o[i]=call_value(args[0],&a,1,line,NULL); } return mkarr(o,N); }
        if(!strcmp(m,"filter")){ Value**o=N?(Value**)xalloc(sizeof(Value*)*N):NULL; int c=0; for(int i=0;i<N;i++){ Value*a=it[i]; if(value_truthy(call_value(args[0],&a,1,line,NULL))) o[c++]=it[i]; } return mkarr(o,c); }
        if(!strcmp(m,"each")){ for(int i=0;i<N;i++){ Value*a=it[i]; call_value(args[0],&a,1,line,NULL); } return VNONE; }
        if(!strcmp(m,"sum")){ Value*acc=mkint(0); for(int i=0;i<N;i++) acc=binop("+",acc,it[i],line); return acc; }
        if(!strcmp(m,"join")){ Buf b; buf_init(&b); const char*sep=nargs?args[0]->as.str.s:""; for(int i=0;i<N;i++){ if(i)buf_puts(&b,sep); char*s=value_to_cstr(it[i],false); buf_puts(&b,s);} return mkstr_n(b.data,b.len); }
        PANIC_CODE("type.method"); panic(line,"no array method `%s`",m);
    }
    if(recv->kind==V_STR){
        const char *s=recv->as.str.s; size_t L=recv->as.str.len;
        if(!strcmp(m,"len")||!strcmp(m,"size")) return mkint((int64_t)L);
        if(!strcmp(m,"upper")){ char*o=xstrndup(s,L); for(size_t i=0;i<L;i++)o[i]=toupper((unsigned char)o[i]); return mkstr_n(o,L); }
        if(!strcmp(m,"lower")){ char*o=xstrndup(s,L); for(size_t i=0;i<L;i++)o[i]=tolower((unsigned char)o[i]); return mkstr_n(o,L); }
        if(!strcmp(m,"trim")){ size_t a=0,b=L; while(a<b&&isspace((unsigned char)s[a]))a++; while(b>a&&isspace((unsigned char)s[b-1]))b--; return mkstr_n(s+a,b-a); }
        if(!strcmp(m,"contains")) return mkbool(strstr(s,args[0]->as.str.s)!=NULL);
        if(!strcmp(m,"starts_with")) return mkbool(strncmp(s,args[0]->as.str.s,strlen(args[0]->as.str.s))==0);
        if(!strcmp(m,"to_int")){ const char*p=s; while(*p&&isspace((unsigned char)*p))p++; if(!*p) return mkfail(mkstr("empty input is not an integer")); char*end; long long v=strtoll(s,&end,10); while(*end&&isspace((unsigned char)*end))end++; if(*end) return mkfail(mkstr("not an integer")); return mkok(mkint(v)); }
        if(!strcmp(m,"split")){ const char*sep=args[0]->as.str.s; Vec parts; vec_init(&parts);
            const char*p=s; const char*q; size_t sl=strlen(sep);
            if(sl==0){ for(size_t i=0;i<L;i++) vec_push(&parts,mkstr_n(s+i,1)); }
            else { while((q=strstr(p,sep))){ vec_push(&parts,mkstr_n(p,q-p)); p=q+sl; } vec_push(&parts,mkstr(p)); }
            Value**o=(Value**)xalloc(sizeof(Value*)*parts.len); for(int i=0;i<parts.len;i++)o[i]=parts.items[i]; return mkarr(o,parts.len); }
        PANIC_CODE("type.method"); panic(line,"no string method `%s`",m);
    }
    if(recv->kind==V_ENUM){
        if(!strcmp(m,"name")) return mkstr(recv->as.en.variant);
    }
    if(recv->kind==V_INT){
        if(!strcmp(m,"abs")) return mkint(recv->as.i<0?-recv->as.i:recv->as.i);
        if(!strcmp(m,"to_f")) return mkfloat((double)recv->as.i);
    }
    PANIC_CODE("type.method"); panic(line,"value has no method `%s`",m);
    return VNONE;
}

/* ================================================================ builtins */
static void json_to_buf(Buf *b, Value *v){
    switch(v->kind){
        case V_NONE: buf_puts(b,"null"); break;
        case V_INT: buf_printf(b,"%lld",(long long)v->as.i); break;
        case V_FLOAT:{ char t[32]; snprintf(t,sizeof t,"%g",v->as.f); buf_puts(b,t);} break;
        case V_DEC: dec_to_buf(b,v->as.dec.unscaled,v->as.dec.scale); break;
        case V_BOOL: buf_puts(b,v->as.b?"true":"false"); break;
        case V_STR: case V_CTXSTR:{
            const char*s=v->kind==V_STR?v->as.str.s:v->as.ctxstr.s;
            buf_putc(b,'"'); for(const char*p=s;*p;p++){ if(*p=='"'||*p=='\\'){buf_putc(b,'\\');buf_putc(b,*p);} else if(*p=='\n')buf_puts(b,"\\n"); else buf_putc(b,*p);} buf_putc(b,'"');
        } break;
        case V_ARRAY: buf_putc(b,'['); for(int i=0;i<v->as.arr.len;i++){ if(i)buf_putc(b,','); json_to_buf(b,v->as.arr.items[i]);} buf_putc(b,']'); break;
        case V_RECORD:{ buf_putc(b,'{'); for(int i=0;i<v->as.rec.fields.len;i++){ FieldVal*f=(FieldVal*)v->as.rec.fields.items[i]; if(i)buf_putc(b,','); buf_printf(b,"\"%s\":",f->name); json_to_buf(b,f->val);} buf_putc(b,'}'); } break;
        case V_SECRET: buf_puts(b,"\"<redacted>\""); break;
        case V_UNTRUSTED: json_to_buf(b,v->as.inner); break;
        case V_OK: json_to_buf(b,v->as.inner); break;
        case V_ENUM: buf_printf(b,"\"%s\"",v->as.en.variant); break;
        default: buf_puts(b,"null");
    }
}

static Value *bi_print(Value**a,int n,Env*e){ (void)e; Buf b; buf_init(&b); for(int i=0;i<n;i++){ if(i)buf_putc(&b,' '); value_to_buf(&b,a[i],false);} fputs(b.data,stdout); return VNONE; }
static Value *bi_println(Value**a,int n,Env*e){ bi_print(a,n,e); fputc('\n',stdout); return VNONE; }
static Value *bi_len(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0];
    if(v->kind==V_ARRAY)return mkint(v->as.arr.len); if(v->kind==V_STR)return mkint((int64_t)v->as.str.len);
    if(v->kind==V_RECORD)return mkint(v->as.rec.fields.len); return mkint(0); }
static Value *bi_str(Value**a,int n,Env*e){ (void)e;(void)n; return mkstr(value_to_cstr(a[0],false)); }
static Value *bi_repr(Value**a,int n,Env*e){ (void)e;(void)n; return mkstr(value_to_cstr(a[0],true)); }
static Value *bi_int(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0];
    if(v->kind==V_INT)return v; if(v->kind==V_FLOAT)return mkint((int64_t)v->as.f);
    if(v->kind==V_DEC)return mkint(v->as.dec.unscaled/ipow10(v->as.dec.scale));
    if(v->kind==V_STR)return mkint(strtoll(v->as.str.s,NULL,10)); if(v->kind==V_BOOL)return mkint(v->as.b?1:0); return mkint(0); }
static Value *bi_float(Value**a,int n,Env*e){ (void)e;(void)n; return mkfloat(as_double(a[0])); }
static Value *bi_some(Value**a,int n,Env*e){ (void)e;(void)n; return a[0]; }   /* Option some(x) = x */
static Value *bi_ok(Value**a,int n,Env*e){ (void)e;(void)n; return mkok(n? a[0]:VNONE); }
static Value *bi_fail(Value**a,int n,Env*e){ (void)e; return mkfail(n? a[0]:VNONE); }
static Value *bi_secret(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=mkval(V_SECRET); v->as.inner=a[0]; return v; }
static Value *bi_reveal(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0]; return v->kind==V_SECRET? v->as.inner : v; }
static Value *bi_untrusted(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=mkval(V_UNTRUSTED); v->as.inner=a[0]; return v; }
static Value *bi_range(Value**a,int n,Env*e){ (void)e; int64_t lo=0,hi; if(n==1){hi=a[0]->as.i;} else {lo=a[0]->as.i;hi=a[1]->as.i;} if(hi<lo)hi=lo; int len=(int)(hi-lo); Value**it=len?(Value**)xalloc(sizeof(Value*)*len):NULL; for(int i=0;i<len;i++)it[i]=mkint(lo+i); return mkarr(it,len); }
static Value *bi_abs(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0]; if(v->kind==V_INT)return mkint(v->as.i<0?-v->as.i:v->as.i); if(v->kind==V_FLOAT)return mkfloat(fabs(v->as.f)); return v; }
static Value *bi_min(Value**a,int n,Env*e){ (void)e; Value*m=a[0]; for(int i=1;i<n;i++) if(value_truthy(binop("<",a[i],m,0)))m=a[i]; return m; }
static Value *bi_max(Value**a,int n,Env*e){ (void)e; Value*m=a[0]; for(int i=1;i<n;i++) if(value_truthy(binop(">",a[i],m,0)))m=a[i]; return m; }
static Value *bi_assert_eq(Value**a,int n,Env*e){ (void)e;(void)n; if(!values_equal(a[0],a[1])) panic(0,"assert_eq failed: %s != %s",value_to_cstr(a[0],true),value_to_cstr(a[1],true)); return VNONE; }
static Value *bi_abort(Value**a,int n,Env*e){ (void)e; panic(0,"abort: %s", n?value_to_cstr(a[0],false):"(no message)"); return VNONE; }
static Value *bi_type_name(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0]; const char*t; switch(v->kind){case V_INT:t="int";break;case V_FLOAT:t="f64";break;case V_STR:t="string";break;case V_BOOL:t="bool";break;case V_ARRAY:t="array";break;case V_DEC:t="decimal";break;case V_RECORD:t=v->as.rec.type;break;case V_ENUM:t=v->as.en.type?v->as.en.type:"enum";break;case V_NONE:t="none";break;default:t="value";} return mkstr(t); }
static Value *bi_json(Value**a,int n,Env*e){ (void)e;(void)n; Buf b; buf_init(&b); json_to_buf(&b,a[0]); return mkstr_n(b.data,b.len); }
static Value *bi_fake_db(Value**a,int n,Env*e){ (void)e; Value*db=mk_cap("Db"); record_set(db,"rows", n? a[0] : mkarr(NULL,0)); return db; }
static Value *bi_make_request(Value**a,int n,Env*e){ (void)e; Value*r=mk_cap("Request"); record_set(r,"query", n? a[0] : (mkval(V_RECORD))); return r; }
static Value *bi_ord(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0]; return mkint(v->kind==V_STR&&v->as.str.len? (unsigned char)v->as.str.s[0] : 0); }
static Value *bi_chr(Value**a,int n,Env*e){ (void)e;(void)n; char c=(char)a[0]->as.i; return mkstr_n(&c,1); }
static Value *bi_is_digit(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0]; return mkbool(v->kind==V_STR&&v->as.str.len&&isdigit((unsigned char)v->as.str.s[0])); }
static Value *bi_is_alpha(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0]; char c=v->kind==V_STR&&v->as.str.len?v->as.str.s[0]:0; return mkbool(isalpha((unsigned char)c)||c=='_'); }
static Value *bi_is_alnum(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0]; char c=v->kind==V_STR&&v->as.str.len?v->as.str.s[0]:0; return mkbool(isalnum((unsigned char)c)||c=='_'); }
static Value *bi_is_space(Value**a,int n,Env*e){ (void)e;(void)n; Value*v=a[0]; return mkbool(v->kind==V_STR&&v->as.str.len&&isspace((unsigned char)v->as.str.s[0])); }

/* Response.* namespace methods */
static Value *bi_resp_json(Value**a,int n,Env*e){ (void)e;(void)n; Value*r=mk_cap("Response"); record_set(r,"status",mkint(200)); Buf b; buf_init(&b); json_to_buf(&b,a[0]); record_set(r,"body",mkstr_n(b.data,b.len)); record_set(r,"content_type",mkstr("application/json")); return r; }
static Value *bi_resp_not_found(Value**a,int n,Env*e){ (void)e; Value*r=mk_cap("Response"); record_set(r,"status",mkint(404)); record_set(r,"body", n? mkstr(value_to_cstr(a[0],false)) : mkstr("Not Found")); return r; }
static Value *bi_resp_ok(Value**a,int n,Env*e){ (void)e; Value*r=mk_cap("Response"); record_set(r,"status",mkint(200)); record_set(r,"body", n? mkstr(value_to_cstr(a[0],false)) : mkstr("OK")); return r; }
static Value *bi_resp_error(Value**a,int n,Env*e){ (void)e; Value*r=mk_cap("Response"); record_set(r,"status", n>1?a[1]:mkint(500)); record_set(r,"body", n? mkstr(value_to_cstr(a[0],false)) : mkstr("Error")); return r; }

static Value *mk_builtin(const char *name, Value*(*fn)(Value**,int,Env*)){
    Value *v=mkval(V_BUILTIN); v->as.bi.fn=fn; v->as.bi.name=xstrdup(name); return v;
}

/* ----------------------------------------------------- register prelude */
static void register_builtins(Env *g){
    env_define(g,"print",   mk_builtin("print",bi_print),false);
    env_define(g,"println", mk_builtin("println",bi_println),false);
    env_define(g,"len",     mk_builtin("len",bi_len),false);
    env_define(g,"str",     mk_builtin("str",bi_str),false);
    env_define(g,"repr",    mk_builtin("repr",bi_repr),false);
    env_define(g,"int",     mk_builtin("int",bi_int),false);
    env_define(g,"float",   mk_builtin("float",bi_float),false);
    env_define(g,"some",    mk_builtin("some",bi_some),false);
    env_define(g,"ok",      mk_builtin("ok",bi_ok),false);
    env_define(g,"fail",    mk_builtin("fail",bi_fail),false);
    env_define(g,"secret",  mk_builtin("secret",bi_secret),false);
    env_define(g,"reveal",  mk_builtin("reveal",bi_reveal),false);
    env_define(g,"untrusted",mk_builtin("untrusted",bi_untrusted),false);
    env_define(g,"range",   mk_builtin("range",bi_range),false);
    env_define(g,"abs",     mk_builtin("abs",bi_abs),false);
    env_define(g,"min",     mk_builtin("min",bi_min),false);
    env_define(g,"max",     mk_builtin("max",bi_max),false);
    env_define(g,"assert_eq",mk_builtin("assert_eq",bi_assert_eq),false);
    env_define(g,"abort",   mk_builtin("abort",bi_abort),false);
    env_define(g,"type_name",mk_builtin("type_name",bi_type_name),false);
    env_define(g,"json",    mk_builtin("json",bi_json),false);
    env_define(g,"fake_db", mk_builtin("fake_db",bi_fake_db),false);
    env_define(g,"make_request", mk_builtin("make_request",bi_make_request),false);
    env_define(g,"ord",      mk_builtin("ord",bi_ord),false);
    env_define(g,"chr",      mk_builtin("chr",bi_chr),false);
    env_define(g,"is_digit", mk_builtin("is_digit",bi_is_digit),false);
    env_define(g,"is_alpha", mk_builtin("is_alpha",bi_is_alpha),false);
    env_define(g,"is_alnum", mk_builtin("is_alnum",bi_is_alnum),false);
    env_define(g,"is_space", mk_builtin("is_space",bi_is_space),false);
    /* Response namespace */
    Value *resp=mk_cap("Namespace"); resp->as.rec.type=xstrdup("Response");
    record_set(resp,"json",     mk_builtin("Response.json",bi_resp_json));
    record_set(resp,"not_found",mk_builtin("Response.not_found",bi_resp_not_found));
    record_set(resp,"ok",       mk_builtin("Response.ok",bi_resp_ok));
    record_set(resp,"error",    mk_builtin("Response.error",bi_resp_error));
    env_define(g,"Response",resp,false);
}

static Value *make_system(int argc, char **argv){
    Value *sys=mk_cap("System");
    /* 2.1: root the filesystem capability at the real working directory, so the
       one root authority handed to main(sys) can read actual files (e.g. the
       compiler reading its own source) while narrowing stays containment-safe. */
    char cwd[4096]; if(!getcwd(cwd,sizeof cwd)) strcpy(cwd,"/");
    Value *fs=mk_cap("Dir"); record_set(fs,"root",mkstr(cwd));
    record_set(sys,"fs",fs);
    record_set(sys,"net",mk_cap("Net"));
    /* sys.args = user arguments AFTER `keel run script.keel` (argv[3..]),
       matching the compiled runtime's argv[1..] so a program sees the same
       args whether interpreted or compiled. */
    int extra = argc>3? argc-3 : 0;
    Value **as = extra?(Value**)xalloc(sizeof(Value*)*extra):NULL;
    for(int i=0;i<extra;i++) as[i]=mkstr(argv[i+3]);
    record_set(sys,"args",mkarr(as,extra));
    return sys;
}

/* hoist type/function declarations from the top level */
static void hoist(Node *prog, Env *g){
    for(int i=0;i<prog->list.len;i++){
        Node *d=(Node*)prog->list.items[i];
        if(d->kind==N_STRUCT||d->kind==N_CLASS||d->kind==N_INTERFACE){
            TypeEnt *t=(TypeEnt*)xalloc(sizeof(TypeEnt)); t->name=d->sval; t->decl=d; vec_push(&g_types,t);
        } else if(d->kind==N_ENUM){
            TypeEnt *t=(TypeEnt*)xalloc(sizeof(TypeEnt)); t->name=d->sval; t->decl=d; vec_push(&g_types,t);
            for(int j=0;j<d->list.len;j++){ Node*v=(Node*)d->list.items[j];
                TypeEnt *vt=(TypeEnt*)xalloc(sizeof(TypeEnt)); vt->name=v->sval; vt->decl=d; vec_push(&g_variants,vt); }
        } else if(d->kind==N_DEF){
            Value *c=mkval(V_CLOSURE); c->as.clo.fn=d; c->as.clo.env=g;
            env_define(g,d->sval,c,false);
        }
    }
}

static int g_had_error=0;
/* run top-level statements, then main(sys) if defined */
static void run_program(Node *prog, Env *g, Value *sys){
    if(setjmp(g_panic_buf)){
        fprintf(stderr,"runtime error[%s]", g_panic_code);
        if(g_panic_line) fprintf(stderr," (line %d)",g_panic_line);
        fprintf(stderr,": %s\n",g_panic_msg); g_had_error=1; return;
    }
    for(int i=0;i<prog->list.len;i++){
        Node *d=(Node*)prog->list.items[i];
        if(d->kind==N_STRUCT||d->kind==N_ENUM||d->kind==N_CLASS||d->kind==N_INTERFACE||
           d->kind==N_TEST||d->kind==N_DEF) continue;
        g_ctl=CTL_NONE; eval(d,g);
    }
    Binding *mainb=env_find(g,"main");
    if(mainb && mainb->val->kind==V_CLOSURE){
        Value *args[1]={sys}; g_ctl=CTL_NONE;
        Value *r=call_closure(mainb->val,args,1,0,NULL,NULL);
        if(r&&r->kind==V_FAIL){ fprintf(stderr,"main failed: %s\n",value_to_cstr(r->as.inner,false)); g_had_error=1; }
    }
}

/* ============================================================ test runner */
static unsigned g_seed=0x9e3779b9u;
static unsigned xrand(){ g_seed^=g_seed<<13; g_seed^=g_seed>>17; g_seed^=g_seed<<5; return g_seed; }

static Value *gen_arg(Node *type){
    const char *t = (type&&type->sval)?type->sval:"int";
    if(!strcmp(t,"int")) return mkint((int64_t)(xrand()%201)-100);
    if(!strcmp(t,"f64")||!strcmp(t,"float")) return mkfloat(((double)(xrand()%2000)-1000.0)/10.0);
    if(!strcmp(t,"bool")) return mkbool(xrand()&1);
    if(!strcmp(t,"string")){ int L=xrand()%8; char*s=(char*)xalloc(L+1); for(int i=0;i<L;i++)s[i]='a'+(xrand()%26); s[L]=0; return mkstr_n(s,L); }
    if(!strcmp(t,"array")){ int L=xrand()%6; Value**it=L?(Value**)xalloc(sizeof(Value*)*L):NULL;
        Node *inner = type->list.len? (Node*)type->list.items[0] : NULL;
        for(int i=0;i<L;i++) it[i]=gen_arg(inner); return mkarr(it,L); }
    return mkint((int64_t)(xrand()%201)-100);
}

static bool prop_trial(Node *body, Env *g, Vec *params, Value **args){
    if(setjmp(g_panic_buf)) return false;     /* panic ⇒ property failed */
    Env *te=env_new(g);
    for(int i=0;i<params->len;i++){ Node*p=(Node*)params->items[i]; env_define(te,p->sval,args[i],false); }
    g_ctl=CTL_NONE; eval_block(body,te); g_ctl=CTL_NONE;
    return true;
}

/* attempt to shrink a failing counterexample */
static void shrink(Node *body, Env *g, Vec *params, Value **args){
    bool improved=true; int rounds=0;
    while(improved && rounds++<40){
        improved=false;
        for(int i=0;i<params->len;i++){
            Value *orig=args[i];
            Value *cands[4]; int nc=0;
            if(orig->kind==V_INT){
                if(orig->as.i!=0) cands[nc++]=mkint(0);
                if(orig->as.i!=0) cands[nc++]=mkint(orig->as.i/2);
                if(orig->as.i>0) cands[nc++]=mkint(orig->as.i-1); else if(orig->as.i<0) cands[nc++]=mkint(orig->as.i+1);
            } else if(orig->kind==V_ARRAY && orig->as.arr.len>0){
                int L=orig->as.arr.len; Value**it=(Value**)xalloc(sizeof(Value*)*(L-1));
                for(int k=0;k<L-1;k++) it[k]=orig->as.arr.items[k];
                cands[nc++]=mkarr(it,L-1);
            }
            for(int c=0;c<nc;c++){ args[i]=cands[c];
                if(!prop_trial(body,g,params,args)){ improved=true; break; }
                args[i]=orig; }
        }
    }
}

static int run_tests(Node *prog, Env *g){
    volatile int passed=0, failed=0;
    for(int i=0;i<prog->list.len;i++){
        Node *t=(Node*)prog->list.items[i];
        if(t->kind!=N_TEST) continue;
        if(!t->is_prop){
            printf("test %-40s ", t->sval);
            if(setjmp(g_panic_buf)){ printf("FAIL — %s\n",g_panic_msg); failed++; }
            else { Env*te=env_new(g); g_ctl=CTL_NONE; eval_block(t->a,te); g_ctl=CTL_NONE; printf("ok\n"); passed++; }
        } else {
            printf("prop %-40s ", t->sval);
            int trials=100; bool ok=true; Value **fail_args=NULL;
            for(int k=0;k<trials;k++){
                int np=t->list2.len; Value**args=np?(Value**)xalloc(sizeof(Value*)*np):NULL;
                for(int p=0;p<np;p++) args[p]=gen_arg(((Node*)t->list2.items[p])->type);
                if(!prop_trial(t->a,g,&t->list2,args)){ ok=false; fail_args=args; break; }
            }
            if(ok){ printf("ok (%d cases)\n",trials); passed++; }
            else {
                shrink(t->a,g,&t->list2,fail_args);
                Buf b; buf_init(&b);
                for(int p=0;p<t->list2.len;p++){ if(p)buf_puts(&b,", ");
                    buf_printf(&b,"%s=",((Node*)t->list2.items[p])->sval); value_to_buf(&b,fail_args[p],true); }
                printf("FAIL — counterexample: %s\n", b.data); failed++;
            }
        }
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed;
}

/* ==================================================================== main */
static char *read_file(const char *path){
    FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"keel: cannot open %s\n",path); exit(66); }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=(char*)xalloc(n+1);
    size_t got=fread(buf,1,n,f); buf[got]=0; fclose(f); return buf;
}

static void init_runtime(){
    V_TRUE_C=mkval(V_BOOL); V_TRUE_C->as.b=true;
    V_FALSE_C=mkval(V_BOOL); V_FALSE_C->as.b=false;
    vec_init(&g_types); vec_init(&g_variants);
}

/* The shared front-end: read → tokenize → parse → report. On any diagnostic,
 * flush every accumulated error (with caret excerpts) and exit 65 — but only
 * after the whole file has been scanned, never on the first error. */
static Node *front_end(const char *path){
    char *src=read_file(path);
    int ntok; Token **toks=tokenize(src,path,&ntok);
    Node *prog=parse_program(toks,ntok,path);
    if (g_diags.len > 0){ diag_flush(); exit(65); }
    return prog;
}

/* ------- C4: canonical token / AST dumps for differential testing ------- */
static const char *tok_kind_name(TokKind k);
static int dump_tokens_main(const char *path){
    diag_init();
    char *src=read_file(path);
    int ntok; Token **toks=tokenize(src,path,&ntok);
    if (g_diags.len > 0){ diag_flush(); return 65; }
    for(int i=0;i<ntok;i++){
        Token *t=toks[i];
        Buf b; buf_init(&b);
        for(int j=0;j<t->len;j++){ char c=t->start[j]; if(c=='\n')buf_puts(&b,"\\n"); else if(c=='\t')buf_puts(&b,"\\t"); else buf_putc(&b,c); }
        printf("%-9s %d:%d", tok_kind_name(t->kind), t->lo, t->hi);
        if(t->tag) printf(" tag=%s", t->tag);
        if(t->len>0) printf(" '%s'", b.data);
        printf("\n");
    }
    return 0;
}

static void ast_dump(Buf *b, Node *n, int depth);
static int dump_ast_main(const char *path){
    diag_init();
    Node *prog=front_end(path);
    Buf b; buf_init(&b);
    for(int i=0;i<prog->list.len;i++){ ast_dump(&b,(Node*)prog->list.items[i],0); buf_putc(&b,'\n'); }
    fputs(b.data,stdout);
    return 0;
}

int format_main(const char *path);   /* formatter, defined below */
static void load_imports(Node *prog, const char *base_path, Env *g);  /* 2.3 module loader */

int main(int argc, char **argv){
    if(argc<2){
        fprintf(stderr,"keel — reference interpreter & oracle for the Keel language\n");
        fprintf(stderr,"usage: keel <run|test|fmt|tokens|ast|version> [file.keel]\n");
        return 64;
    }
    const char *cmd=argv[1];
    if(!strcmp(cmd,"version")){ printf("keel 0.2.0 (Stage-0 reference oracle)\n"); return 0; }
    if(argc<3){ fprintf(stderr,"expected a .keel file\n"); return 64; }

    diag_init();
    if(!strcmp(cmd,"fmt"))    return format_main(argv[2]);
    if(!strcmp(cmd,"tokens")) return dump_tokens_main(argv[2]);
    if(!strcmp(cmd,"ast"))    return dump_ast_main(argv[2]);

    Node *prog=front_end(argv[2]);

    init_runtime();
    Env *g=env_new(NULL); g_genv=g;
    register_builtins(g);
    load_imports(prog, argv[2], g);   /* 2.3: resolve and hoist imported modules */
    hoist(prog,g);

    if(!strcmp(cmd,"run")){
        Value *sys=make_system(argc,argv);
        run_program(prog,g,sys);
        free_all();
        return g_had_error?70:0;
    }
    if(!strcmp(cmd,"test")){
        int failed=run_tests(prog,g);
        free_all();
        return failed?1:0;
    }
    fprintf(stderr,"unknown command '%s'\n",cmd);
    return 64;
}

/* ===================================================== C4: token/AST dumps */
static const char *tok_kind_name(TokKind k){
    switch(k){
        case T_EOF:return "EOF"; case T_NEWLINE:return "NEWLINE";
        case T_INDENT:return "INDENT"; case T_DEDENT:return "DEDENT";
        case T_INT:return "INT"; case T_DECIMAL:return "DECIMAL"; case T_FLOAT:return "FLOAT";
        case T_STRING:return "STRING"; case T_IDENT:return "IDENT";
        case T_LPAREN:return "LPAREN"; case T_RPAREN:return "RPAREN";
        case T_LBRACK:return "LBRACK"; case T_RBRACK:return "RBRACK";
        case T_LBRACE:return "LBRACE"; case T_RBRACE:return "RBRACE";
        case T_COMMA:return "COMMA"; case T_COLON:return "COLON"; case T_SEMI:return "SEMI"; case T_DOT:return "DOT";
        case T_PLUS:return "PLUS"; case T_MINUS:return "MINUS"; case T_STAR:return "STAR";
        case T_SLASH:return "SLASH"; case T_PERCENT:return "PERCENT"; case T_POW:return "POW";
        case T_EQ:return "EQ"; case T_NE:return "NE"; case T_LT:return "LT"; case T_LE:return "LE";
        case T_GT:return "GT"; case T_GE:return "GE";
        case T_ASSIGN:return "ASSIGN"; case T_PLUSEQ:return "PLUSEQ"; case T_MINUSEQ:return "MINUSEQ";
        case T_STAREQ:return "STAREQ"; case T_SLASHEQ:return "SLASHEQ"; case T_PERCENTEQ:return "PERCENTEQ";
        case T_INC:return "INC"; case T_DEC:return "DEC";
        case T_AMP:return "AMP"; case T_AMPMUT:return "AMPMUT"; case T_PIPE:return "PIPE";
        case T_CARET:return "CARET"; case T_TILDE:return "TILDE"; case T_SHL:return "SHL"; case T_SHR:return "SHR";
        case T_QUESTION:return "QUESTION"; case T_QDOT:return "QDOT"; case T_QQ:return "QQ"; case T_ARROW:return "ARROW";
        case T_AND:return "AND"; case T_ASSERT:return "ASSERT"; case T_CHECK:return "CHECK"; case T_CLASS:return "CLASS";
        case T_COMPTIME:return "COMPTIME"; case T_CONTINUE:return "CONTINUE"; case T_DEF:return "DEF"; case T_DERIVE:return "DERIVE";
        case T_ELIF:return "ELIF"; case T_ELSE:return "ELSE"; case T_ENUM:return "ENUM"; case T_EXECUTE:return "EXECUTE";
        case T_EXTERN:return "EXTERN"; case T_HANDLE:return "HANDLE"; case T_IF:return "IF"; case T_IMPORT:return "IMPORT";
        case T_INTERFACE:return "INTERFACE"; case T_IS:return "IS"; case T_LOOP:return "LOOP"; case T_MODULE:return "MODULE";
        case T_MUT:return "MUT"; case T_NONE:return "NONE"; case T_NOT:return "NOT"; case T_OR:return "OR"; case T_PARALLEL:return "PARALLEL";
        case T_PRIVATE:return "PRIVATE"; case T_PROP:return "PROP"; case T_PUBLIC:return "PUBLIC"; case T_REGION:return "REGION";
        case T_REFLECT:return "REFLECT"; case T_RESUME:return "RESUME"; case T_RETURN:return "RETURN"; case T_SCOPE:return "SCOPE";
        case T_SHARED:return "SHARED"; case T_SPAWN:return "SPAWN"; case T_STRUCT:return "STRUCT"; case T_TEST:return "TEST";
        case T_THROUGH:return "THROUGH"; case T_TILL:return "TILL"; case T_TOTAL:return "TOTAL"; case T_TYPE:return "TYPE";
        case T_UNSAFE:return "UNSAFE"; case T_WHERE:return "WHERE"; case T_BREAK:return "BREAK"; case T_TRUE:return "TRUE"; case T_FALSE:return "FALSE";
        default: return "?";
    }
}

static const char *node_kind_name(NodeKind k){
    switch(k){
        case N_INT:return "int"; case N_DECIMAL:return "dec"; case N_FLOAT:return "float";
        case N_STRING:return "str"; case N_BOOL:return "bool"; case N_NONE:return "none";
        case N_IDENT:return "id"; case N_ARRAY:return "array"; case N_RECORD:return "record";
        case N_INDEX:return "index"; case N_SLICE:return "slice"; case N_CALL:return "call"; case N_FIELD:return "field";
        case N_UNARY:return "unary"; case N_BINARY:return "bin"; case N_LOGICAL:return "logical"; case N_TERNARY:return "ternary";
        case N_QDOT:return "qdot"; case N_QQ:return "qq"; case N_TRY:return "try"; case N_RANGE:return "range";
        case N_LAMBDA:return "lambda"; case N_INTERP:return "interp";
        case N_LET:return "let"; case N_ASSIGN:return "assign"; case N_EXPRSTMT:return "expr"; case N_RETURN:return "return";
        case N_IF:return "if"; case N_CHECK:return "check"; case N_LOOP_TILL:return "loop-till"; case N_LOOP_THROUGH:return "loop-through";
        case N_LOOP_INF:return "loop"; case N_BREAK:return "break"; case N_CONTINUE:return "continue"; case N_ASSERT:return "assert";
        case N_BLOCK:return "block"; case N_DEF:return "def"; case N_STRUCT:return "struct"; case N_ENUM:return "enum";
        case N_INTERFACE:return "interface"; case N_CLASS:return "class"; case N_TEST:return "test"; case N_EXECUTE:return "execute";
        case N_REGION:return "region"; case N_COMPTIME:return "comptime"; case N_IMPORT:return "import"; case N_MODULE:return "module";
        case N_PARALLEL:return "parallel"; case N_UNSAFE:return "unsafe"; case N_EXTERN:return "extern";
        case N_PARAM:return "param"; case N_FIELDDEF:return "fielddef"; case N_VARIANT:return "variant"; case N_ARM:return "arm";
        case N_HANDLER:return "handler"; case N_TYPE:return "type"; case N_NAMEDARG:return "named";
        default:return "?";
    }
}

/* Compact, deterministic S-expression dump for differential testing (C4). */
static void ast_dump(Buf *b, Node *n, int depth){
    if(!n){ buf_puts(b,"()"); return; }
    buf_putc(b,'(');
    buf_puts(b, node_kind_name(n->kind));
    switch(n->kind){
        case N_INT: buf_printf(b," %lld",(long long)n->ival); break;
        case N_FLOAT: buf_printf(b," %g",n->fval); break;
        case N_BOOL: buf_printf(b," %s",n->bval?"true":"false"); break;
        case N_STRING: buf_printf(b," %s", n->sval?n->sval:""); break;
        case N_IDENT: buf_printf(b," %s", n->sval?n->sval:""); break;
        case N_BINARY: case N_LOGICAL: case N_QQ: case N_RANGE:
        case N_UNARY: case N_ASSIGN:
            buf_printf(b," %s", n->sval?n->sval:""); break;
        default: if(n->sval) buf_printf(b," %s",n->sval); break;
    }
    if(n->type){ buf_puts(b," :"); ast_dump(b,n->type,depth+1); }
    if(n->a){ buf_putc(b,' '); ast_dump(b,n->a,depth+1); }
    if(n->b){ buf_putc(b,' '); ast_dump(b,n->b,depth+1); }
    if(n->c){ buf_putc(b,' '); ast_dump(b,n->c,depth+1); }
    if(n->d){ buf_putc(b,' '); ast_dump(b,n->d,depth+1); }
    for(int i=0;i<n->list.len;i++){ buf_putc(b,' '); ast_dump(b,(Node*)n->list.items[i],depth+1); }
    if(n->list2.len){ buf_puts(b," |"); for(int i=0;i<n->list2.len;i++){ buf_putc(b,' '); ast_dump(b,(Node*)n->list2.items[i],depth+1); } }
    buf_putc(b,')');
}

/* ============================================ 2.3: capability-honest modules
 * `import "rel/path.keel"` (or `import name`) resolves a file relative to the
 * importing file's directory, parses it, and hoists its top-level defs/types
 * into the importing namespace. Imports bring NAMES, never AUTHORITY: the
 * loaded module does not inherit any Dir/Net capability — capabilities only
 * ever enter at `main(sys: System)` and flow by value. Import cycles are
 * detected and reported, and modules may not run effectful top-level code. */
static Vec g_loaded_modules;    /* char* absolute-ish paths already loaded */
static Vec g_loading_stack;     /* char* for cycle detection */

static char *dir_of(const char *path){
    const char *slash=strrchr(path,'/');
    if(!slash) return xstrdup(".");
    return xstrndup(path,(size_t)(slash-path));
}
static char *join_path(const char *dir, const char *rel){
    Buf b; buf_init(&b); buf_puts(&b,dir);
    if(b.len && b.data[b.len-1]!='/') buf_putc(&b,'/');
    buf_puts(&b,rel);
    return b.data;
}
static int vec_has_str(Vec *v, const char *s){
    for(int i=0;i<v->len;i++) if(strcmp((char*)v->items[i],s)==0) return 1; return 0;
}

static void hoist(Node *prog, Env *g);   /* fwd */

static void load_imports(Node *prog, const char *base_path, Env *g){
    char *base_dir = dir_of(base_path);
    for(int i=0;i<prog->list.len;i++){
        Node *d=(Node*)prog->list.items[i];
        if(d->kind!=N_IMPORT) continue;
        /* import target: either a quoted path, or dotted name → name.keel */
        char *rel;
        if(strstr(d->sval,".keel") || strchr(d->sval,'/')) rel=xstrdup(d->sval);
        else { Buf nb; buf_init(&nb); buf_puts(&nb,d->sval); buf_puts(&nb,".keel"); rel=nb.data; }
        char *full=join_path(base_dir,rel);

        if(vec_has_str(&g_loading_stack, full)){
            diag_add("module.cycle", d->file_id, d->lo, d->hi,
                     "import cycle detected involving '%s'", rel);
            diag_flush(); exit(65);
        }
        if(vec_has_str(&g_loaded_modules, full)) continue;   /* already loaded */

        vec_push(&g_loading_stack, full);
        Node *mod = front_end(full);                 /* read+parse the module */
        /* a module may not run effectful top-level statements at load */
        for(int j=0;j<mod->list.len;j++){
            Node *s=(Node*)mod->list.items[j];
            if(s->kind!=N_DEF&&s->kind!=N_STRUCT&&s->kind!=N_ENUM&&s->kind!=N_CLASS&&
               s->kind!=N_INTERFACE&&s->kind!=N_TEST&&s->kind!=N_IMPORT&&s->kind!=N_MODULE&&
               s->kind!=N_COMPTIME){
                diag_add("module.toplevel", s->file_id, s->lo, s->hi,
                         "a module may only declare definitions at top level "
                         "(no effectful statements run at import — authority enters only at main)");
                diag_flush(); exit(65);
            }
        }
        load_imports(mod, full, g);                  /* nested imports first */
        hoist(mod, g);                               /* bind NAMES (no authority) */
        /* pop loading stack, mark loaded */
        g_loading_stack.len--;
        vec_push(&g_loaded_modules, full);
    }
}

static void fmt_expr(Buf *b, Node *n);
static void fmt_block(Buf *b, Node *blk, int ind);
static void fmt_stmt(Buf *b, Node *n, int ind);
static void ind(Buf *b, int n){ for(int i=0;i<n*4;i++) buf_putc(b,' '); }

static void fmt_type(Buf *b, Node *t){
    if(!t){ return; }
    if(t->tag && !strcmp(t->tag,"shared")) buf_puts(b,"shared ");
    buf_puts(b,t->sval);
    if(t->list.len){ buf_putc(b,'['); for(int i=0;i<t->list.len;i++){ if(i)buf_puts(b,", "); fmt_type(b,(Node*)t->list.items[i]); } buf_putc(b,']'); }
    if(t->is_mut) buf_putc(b,'?');
    if(t->refine){ buf_puts(b," where "); fmt_expr(b,t->refine); }
}

static void fmt_interp(Buf *b, Node *n){
    if(n->tag) buf_puts(b,n->tag);
    buf_putc(b,'"');
    for(int i=0;i<n->list.len;i++){
        Node *p=(Node*)n->list.items[i];
        if(p->kind==N_STRING){ for(char*s=p->sval;*s;s++){ if(*s=='"'||*s=='\\')buf_putc(b,'\\'); buf_putc(b,*s);} }
        else if(p->kind==N_IDENT){ buf_putc(b,'#'); buf_puts(b,p->sval); }
        else { buf_puts(b,"#{"); fmt_expr(b,p); buf_putc(b,'}'); }
    }
    buf_putc(b,'"');
}

static void fmt_expr(Buf *b, Node *n){
    if(!n) return;
    switch(n->kind){
        case N_INT: buf_printf(b,"%lld",(long long)n->ival); break;
        case N_FLOAT: buf_printf(b,"%g",n->fval); break;
        case N_DECIMAL: dec_to_buf(b,n->dec_unscaled,n->dec_scale); break;
        case N_BOOL: buf_puts(b,n->bval?"true":"false"); break;
        case N_NONE: buf_puts(b,"none"); break;
        case N_STRING:{ buf_putc(b,'"'); for(char*s=n->sval;*s;s++){ if(*s=='"'||*s=='\\')buf_putc(b,'\\'); buf_putc(b,*s);} buf_putc(b,'"'); } break;
        case N_INTERP: fmt_interp(b,n); break;
        case N_IDENT: buf_puts(b,n->sval); break;
        case N_ARRAY:{ buf_putc(b,'['); for(int i=0;i<n->list.len;i++){ if(i)buf_puts(b,", "); fmt_expr(b,(Node*)n->list.items[i]); } buf_putc(b,']'); } break;
        case N_RECORD:{ buf_puts(b,"{"); for(int i=0;i<n->list.len;i++){ Node*kv=(Node*)n->list.items[i]; if(i)buf_puts(b,", "); buf_printf(b,"%s: ",kv->sval); fmt_expr(b,kv->a);} buf_puts(b,"}"); } break;
        case N_NAMEDARG: buf_printf(b,"%s: ",n->sval); fmt_expr(b,n->a); break;
        case N_INDEX: case N_SLICE:{ fmt_expr(b,n->a); buf_putc(b,'['); for(int i=0;i<n->list.len;i++){ if(i)buf_puts(b,", "); fmt_expr(b,(Node*)n->list.items[i]); } buf_putc(b,']'); } break;
        case N_CALL:{ fmt_expr(b,n->a); buf_putc(b,'('); for(int i=0;i<n->list.len;i++){ if(i)buf_puts(b,", "); fmt_expr(b,(Node*)n->list.items[i]); } buf_putc(b,')'); } break;
        case N_FIELD: fmt_expr(b,n->a); buf_putc(b,'.'); buf_puts(b,n->sval); break;
        case N_QDOT: fmt_expr(b,n->a); buf_puts(b,"?."); buf_puts(b,n->sval); break;
        case N_UNARY:
            if(!strcmp(n->sval,"reflect")){ buf_puts(b,"reflect("); fmt_expr(b,n->a); buf_putc(b,')'); }
            else if(!strcmp(n->sval,"resume")){ buf_puts(b,"resume"); if(n->a){ buf_putc(b,' '); fmt_expr(b,n->a);} }
            else if(!strcmp(n->sval,"not")){ buf_puts(b,"not "); fmt_expr(b,n->a); }
            else { buf_puts(b,n->sval); fmt_expr(b,n->a); }
            break;
        case N_BINARY: case N_LOGICAL: case N_QQ: case N_RANGE:
            fmt_expr(b,n->a); buf_printf(b," %s ",n->sval); fmt_expr(b,n->b); break;
        case N_TERNARY: fmt_expr(b,n->a); buf_puts(b," ? "); fmt_expr(b,n->b); buf_puts(b," : "); fmt_expr(b,n->c); break;
        case N_TRY: fmt_expr(b,n->a); buf_putc(b,'?'); break;
        case N_LAMBDA:{ buf_putc(b,'('); for(int i=0;i<n->list.len;i++){ Node*p=(Node*)n->list.items[i]; if(i)buf_puts(b,", "); buf_puts(b,p->sval); if(p->type){buf_puts(b,": ");fmt_type(b,p->type);} } buf_puts(b,"): "); fmt_expr(b,n->a); } break;
        default: buf_puts(b,"<expr>");
    }
}

static void fmt_params(Buf *b, Vec *params){
    buf_putc(b,'(');
    for(int i=0;i<params->len;i++){ Node*p=(Node*)params->items[i]; if(i)buf_puts(b,", ");
        if(p->is_mut&&!p->type) buf_puts(b,"mut ");
        buf_puts(b,p->sval);
        if(p->type){ buf_puts(b,": "); if(p->is_mut)buf_puts(b,"&mut "); fmt_type(b,p->type); }
        if(p->a){ buf_puts(b," = "); fmt_expr(b,p->a); } }
    buf_putc(b,')');
}

static void fmt_block(Buf *b, Node *blk, int indent){
    for(int i=0;i<blk->list.len;i++) fmt_stmt(b,(Node*)blk->list.items[i],indent);
}

/* execute / check / if can appear in value position; they format multi-line */
static bool is_block_expr(Node *n){ return n && (n->kind==N_EXECUTE||n->kind==N_CHECK||n->kind==N_IF); }

/* emit a block construct continuing the current line (caller positions the cursor) */
static void fmt_construct(Buf *b, Node *n, int indlvl){
    switch(n->kind){
        case N_IF:
            buf_puts(b,"if "); fmt_expr(b,n->a); buf_puts(b,":\n"); fmt_block(b,n->b,indlvl+1);
            for(int i=0;i<n->list2.len;i++){ Node*ei=(Node*)n->list2.items[i]; ind(b,indlvl); buf_puts(b,"elif "); fmt_expr(b,ei->a); buf_puts(b,":\n"); fmt_block(b,ei->b,indlvl+1); }
            if(n->c){ ind(b,indlvl); buf_puts(b,"else:\n"); fmt_block(b,n->c,indlvl+1); }
            break;
        case N_CHECK:
            buf_puts(b,"check "); fmt_expr(b,n->a); buf_puts(b,":\n");
            for(int i=0;i<n->list.len;i++){ Node*arm=(Node*)n->list.items[i]; ind(b,indlvl+1);
                if(arm->sval&&!strcmp(arm->sval,"__else__")) buf_puts(b,"else");
                else { buf_puts(b,"is ");
                    if(arm->b){ fmt_expr(b,arm->b); }
                    else if(arm->c){ buf_puts(b,arm->c->sval); }
                    else { buf_puts(b,arm->sval); if(arm->list.len){ buf_putc(b,'('); for(int j=0;j<arm->list.len;j++){ if(j)buf_puts(b,", "); buf_puts(b,((Node*)arm->list.items[j])->sval);} buf_putc(b,')'); } } }
                buf_puts(b,":\n"); fmt_block(b,arm->a,indlvl+2); }
            break;
        case N_EXECUTE:
            buf_puts(b,"execute:\n"); fmt_block(b,n->a,indlvl+1);
            for(int i=0;i<n->list2.len;i++){ Node*h=(Node*)n->list2.items[i]; ind(b,indlvl); buf_printf(b,"handle %s",h->sval);
                if(h->tag)buf_printf(b,"<%s>",h->tag); if(h->b)buf_printf(b," as %s",h->b->sval); buf_puts(b,":\n"); fmt_block(b,h->a,indlvl+1); }
            break;
        default: fmt_expr(b,n);
    }
}

static void fmt_stmt(Buf *b, Node *n, int indlvl){
    ind(b,indlvl);
    switch(n->kind){
        case N_LET:
            if(n->is_mut) buf_puts(b,"mut ");
            buf_puts(b,n->sval);
            if(n->type){ buf_puts(b,": "); fmt_type(b,n->type); }
            buf_puts(b," = ");
            if(is_block_expr(n->a)) fmt_construct(b,n->a,indlvl);
            else { fmt_expr(b,n->a); buf_putc(b,'\n'); }
            break;
        case N_ASSIGN:
            fmt_expr(b,n->a);
            if(!strcmp(n->sval,"++")){ buf_puts(b,"++"); buf_putc(b,'\n'); }
            else if(!strcmp(n->sval,"--")){ buf_puts(b,"--"); buf_putc(b,'\n'); }
            else if(!strcmp(n->sval,"=")){ buf_puts(b," = ");
                if(is_block_expr(n->b)) fmt_construct(b,n->b,indlvl);
                else { fmt_expr(b,n->b); buf_putc(b,'\n'); } }
            else { buf_printf(b," %s ",n->sval); fmt_expr(b,n->b); buf_putc(b,'\n'); }
            break;
        case N_EXPRSTMT:
            if(is_block_expr(n->a)) fmt_construct(b,n->a,indlvl);
            else { fmt_expr(b,n->a); buf_putc(b,'\n'); }
            break;
        case N_RETURN: buf_puts(b,"return");
            if(n->a){ buf_putc(b,' ');
                if(is_block_expr(n->a)){ fmt_construct(b,n->a,indlvl); break; }
                fmt_expr(b,n->a); }
            buf_putc(b,'\n'); break;
        case N_BREAK: buf_puts(b,"break\n"); break;
        case N_CONTINUE: buf_puts(b,"continue\n"); break;
        case N_ASSERT: buf_puts(b,"assert "); fmt_expr(b,n->a); buf_putc(b,'\n'); break;
        case N_DEF:
            if(n->is_public) buf_puts(b,"public ");
            buf_puts(b,"def "); buf_puts(b,n->sval); fmt_params(b,&n->list2);
            if(n->ret_type){ buf_puts(b,"; "); fmt_type(b,n->ret_type); }
            if(n->effects.len){ buf_puts(b," / {"); for(int i=0;i<n->effects.len;i++){ if(i)buf_puts(b,", "); buf_puts(b,(char*)n->effects.items[i]); } buf_putc(b,'}'); }
            buf_puts(b,":\n"); fmt_block(b,n->a,indlvl+1); break;
        case N_STRUCT:
            if(n->derives.len){ buf_puts(b,"derive ("); for(int i=0;i<n->derives.len;i++){ if(i)buf_puts(b,", "); buf_puts(b,(char*)n->derives.items[i]); } buf_puts(b,")\n"); ind(b,indlvl); }
            buf_printf(b,"struct %s:\n",n->sval);
            for(int i=0;i<n->list.len;i++){ Node*f=(Node*)n->list.items[i]; ind(b,indlvl+1);
                if(f->is_public)buf_puts(b,"public "); buf_printf(b,"%s: ",f->sval); fmt_type(b,f->type);
                if(f->a){buf_puts(b," = ");fmt_expr(b,f->a);} buf_putc(b,'\n'); }
            break;
        case N_ENUM:
            if(n->derives.len){ buf_puts(b,"derive ("); for(int i=0;i<n->derives.len;i++){ if(i)buf_puts(b,", "); buf_puts(b,(char*)n->derives.items[i]); } buf_puts(b,")\n"); ind(b,indlvl); }
            buf_printf(b,"enum %s:\n",n->sval);
            for(int i=0;i<n->list.len;i++){ Node*v=(Node*)n->list.items[i]; ind(b,indlvl+1); buf_puts(b,v->sval);
                if(v->list.len){ buf_putc(b,'('); for(int j=0;j<v->list.len;j++){ Node*fd=(Node*)v->list.items[j]; if(j)buf_puts(b,", "); if(fd->sval){buf_printf(b,"%s: ",fd->sval);} fmt_type(b,fd->type);} buf_putc(b,')'); }
                buf_putc(b,'\n'); }
            break;
        case N_CLASS: case N_INTERFACE:
            buf_printf(b,"%s %s:\n", n->kind==N_CLASS?"class":"interface", n->sval);
            for(int i=0;i<n->list2.len;i++) fmt_stmt(b,(Node*)n->list2.items[i],indlvl+1);
            break;
        case N_IF: case N_CHECK: fmt_construct(b,n,indlvl); break;
        case N_LOOP_TILL:
            buf_puts(b,"loop till "); fmt_expr(b,n->a);
            if(n->b){ buf_puts(b,"; "); /* init */ Node*s=n->b; if(s->kind==N_LET){buf_printf(b,"%s = ",s->sval);fmt_expr(b,s->a);} else fmt_expr(b,s->a?s->a:s); }
            buf_puts(b,":\n"); fmt_block(b,n->d,indlvl+1); break;
        case N_LOOP_THROUGH:
            buf_puts(b,"loop through "); fmt_expr(b,n->a); buf_puts(b,":\n"); fmt_block(b,n->b,indlvl+1); break;
        case N_LOOP_INF: buf_puts(b,"loop:\n"); fmt_block(b,n->a,indlvl+1); break;
        case N_EXECUTE: fmt_construct(b,n,indlvl); break;
        case N_TEST:
            buf_puts(b,"test "); if(n->is_prop)buf_puts(b,"prop "); buf_printf(b,"\"%s\"",n->sval);
            if(n->is_prop&&n->list2.len) fmt_params(b,&n->list2);
            buf_puts(b,":\n"); fmt_block(b,n->a,indlvl+1); break;
        case N_REGION: buf_printf(b,"region %s:\n",n->sval); fmt_block(b,n->a,indlvl+1); break;
        case N_COMPTIME: buf_puts(b,"comptime:\n"); fmt_block(b,n->a,indlvl+1); break;
        case N_UNSAFE: buf_puts(b,"unsafe:\n"); fmt_block(b,n->a,indlvl+1); break;
        case N_PARALLEL: buf_printf(b,"parallel scope %s:\n",n->sval); fmt_block(b,n->a,indlvl+1); break;
        case N_IMPORT: buf_printf(b,"import %s\n",n->sval); break;
        case N_MODULE: buf_printf(b,"module %s\n",n->sval); break;
        default: fmt_expr(b,n); buf_putc(b,'\n');
    }
}

int format_main(const char *path){
    char *src=read_file(path);
    int ntok; Token **toks=tokenize(src,path,&ntok);
    Node *prog=parse_program(toks,ntok,path);
    Buf b; buf_init(&b);
    for(int i=0;i<prog->list.len;i++){ fmt_stmt(&b,(Node*)prog->list.items[i],0);
        Node*nx=(Node*)prog->list.items[i];
        if(nx->kind==N_DEF||nx->kind==N_STRUCT||nx->kind==N_ENUM||nx->kind==N_CLASS||nx->kind==N_TEST||nx->kind==N_INTERFACE) buf_putc(&b,'\n'); }
    fputs(b.data,stdout);
    return 0;
}
