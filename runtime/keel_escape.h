/* ============================================================================
 * keel_escape.h — the C5 security single-source.
 *
 * The context-string escapers (sql / html / shell / url), the exact-decimal
 * helpers, and the checked-arithmetic traps live HERE, in one header, included
 * by BOTH the Stage-0 interpreter (src/keel.c) and the runtime that compiled
 * Keel programs link against (runtime/keelrt.c). This is the roadmap's C5
 * decision made concrete: there is exactly one definition of "escape for SQL,"
 * so `keel run` and a compiled binary can never diverge on the headline
 * injection guarantee with no test failing.
 *
 * Each escaper returns a freshly malloc()'d NUL-terminated string; the caller
 * copies it and frees it. Returning by value keeps this header free of any
 * allocator coupling, so the interpreter (arena) and the runtime (malloc) can
 * share it verbatim.
 * ==========================================================================*/
#ifndef KEEL_ESCAPE_H
#define KEEL_ESCAPE_H

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

static inline char *kesc_dup(const char *s){ size_t n=strlen(s); char *p=(char*)malloc(n+1); memcpy(p,s,n+1); return p; }

/* a tiny private growable buffer, used only inside this header */
typedef struct { char *p; size_t n, cap; } kesc_buf;
static inline void kesc_init(kesc_buf *b){ b->cap=16; b->p=(char*)malloc(b->cap); b->n=0; b->p[0]=0; }
static inline void kesc_putn(kesc_buf *b, const char *s, size_t k){
    if (b->n+k+1 > b->cap){ while(b->n+k+1>b->cap) b->cap*=2; b->p=(char*)realloc(b->p,b->cap); }
    memcpy(b->p+b->n, s, k); b->n+=k; b->p[b->n]=0;
}
static inline void kesc_putc(kesc_buf *b, char c){ kesc_putn(b,&c,1); }
static inline void kesc_puts(kesc_buf *b, const char *s){ kesc_putn(b,s,strlen(s)); }

/* SQL: escape the backslash *and* double the single quote. Quote-doubling
 * alone is unsafe on MySQL/MariaDB in their default mode (BACKSLASH_ESCAPES on),
 * where a trailing `\` escapes the closing quote and lets input break out of the
 * string literal — the classic incomplete-escape injection (cf. CWE-89,
 * CVE-2026-33468). Escaping the backslash first closes that class; on
 * ANSI/standard-conforming dialects (PostgreSQL, SQLite, SQL Server) a literal
 * backslash is merely doubled, a benign data-fidelity cost, never an injection.
 * The real long-term answer is parameter binding (Part IV's static sink check);
 * this is the security-first runtime escape until then. */
static inline char *kesc_sql(const char *s){
    kesc_buf b; kesc_init(&b);
    for(const char *p=s;*p;p++){
        if(*p=='\'')      kesc_puts(&b,"''");
        else if(*p=='\\') kesc_puts(&b,"\\\\");
        else              kesc_putc(&b,*p);
    }
    return b.p;
}
/* HTML: the five entity-significant characters. */
static inline char *kesc_html(const char *s){
    kesc_buf b; kesc_init(&b);
    for(const char *p=s;*p;p++){
        switch(*p){
            case '<': kesc_puts(&b,"&lt;"); break;
            case '>': kesc_puts(&b,"&gt;"); break;
            case '&': kesc_puts(&b,"&amp;"); break;
            case '"': kesc_puts(&b,"&quot;"); break;
            case '\'':kesc_puts(&b,"&#39;"); break;
            default:  kesc_putc(&b,*p);
        }
    }
    return b.p;
}
/* Shell: wrap in single quotes; close-reopen around embedded single quotes. */
static inline char *kesc_shell(const char *s){
    kesc_buf b; kesc_init(&b); kesc_putc(&b,'\'');
    for(const char *p=s;*p;p++){ if(*p=='\'') kesc_puts(&b,"'\\''"); else kesc_putc(&b,*p); }
    kesc_putc(&b,'\''); return b.p;
}
/* URL: RFC-3986 unreserved set passes; everything else is percent-encoded. */
static inline char *kesc_url(const char *s){
    kesc_buf b; kesc_init(&b);
    for(const char *p=s;*p;p++){ unsigned char c=(unsigned char)*p;
        if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') kesc_putc(&b,(char)c);
        else { char t[4]; snprintf(t,sizeof t,"%%%02X",c); kesc_puts(&b,t); } }
    return b.p;
}

/* Dispatch by context tag. Unknown tags pass through unescaped (a plain string).*/
static inline char *kesc_for(const char *ctx, const char *s){
    if(!ctx) return kesc_dup(s);
    if(!strcmp(ctx,"sql"))   return kesc_sql(s);
    if(!strcmp(ctx,"html"))  return kesc_html(s);
    if(!strcmp(ctx,"shell")) return kesc_shell(s);
    if(!strcmp(ctx,"url"))   return kesc_url(s);
    return kesc_dup(s);
}

/* ----------------------------------------------- checked integer arithmetic */
/* Trap on overflow rather than wrapping silently (spec §5.4). The interpreter
 * and compiled code share these so the trap point is identical. */
static inline int kchk_add(int64_t a, int64_t b, int64_t *r){ return __builtin_add_overflow(a,b,r); }
static inline int kchk_sub(int64_t a, int64_t b, int64_t *r){ return __builtin_sub_overflow(a,b,r); }
static inline int kchk_mul(int64_t a, int64_t b, int64_t *r){ return __builtin_mul_overflow(a,b,r); }

/* ----------------------------------------------------- exact decimal helpers */
static inline int64_t kdec_pow10(int n){ int64_t r=1; while(n-->0) r*=10; return r; }

#endif /* KEEL_ESCAPE_H */
