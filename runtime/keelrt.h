/* ============================================================================
 * keelrt.h — the runtime that compiled Keel programs link against.
 *
 * Codegen (compiler/keelc.keel, Tier 4.2) emits C that calls this API. The
 * value representation and every operation mirror the Stage-0 interpreter's
 * semantics exactly, which is what makes `keel run` and a compiled binary
 * observably equivalent (Tier 4.3) — and the C5 escapers are shared verbatim
 * via keel_escape.h, so there is one definition of "escape for SQL."
 *
 * The allocator is a tracked arena freed at exit: the roadmap's explicitly
 * time-boxed "conservative interim" for 4.2 (Part IV replaces it with the
 * ownership-directed allocator that delivers the GC-free promise). DESIGN.md
 * states this plainly.
 * ==========================================================================*/
#ifndef KEELRT_H
#define KEELRT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <setjmp.h>
#include "keel_escape.h"

typedef enum {
    K_NONE, K_INT, K_FLOAT, K_BOOL, K_STR, K_ARR, K_REC, K_ENUM,
    K_OK, K_FAIL, K_CTXSTR, K_SECRET, K_UNTRUSTED, K_CAP
} KKind;

typedef struct kl_obj kl_obj;
typedef kl_obj *kl;

typedef struct { char *name; kl val; } KField;

struct kl_obj {
    KKind kind;
    union {
        int64_t i;
        double  f;
        int     b;
        struct { char *s; int len; } str;
        struct { kl *items; int len; int cap; } arr;
        struct { char *type; KField *fields; int nf; int cap; } rec;
        struct { char *type; char *variant; kl *payload; int np; } en;
        kl inner;                          /* OK / FAIL / SECRET / UNTRUSTED */
        struct { char *ctx; char *s; } ctxstr;
    } as;
};

/* ---- arena ---- */
void *kl_alloc(size_t n);
char *kl_strndup(const char *s, size_t n);
void  kl_free_all(void);

/* ---- constructors ---- */
kl kl_none(void);
kl kl_int(int64_t i);
kl kl_float(double f);
kl kl_bool(int b);
kl kl_str(const char *s);
kl kl_str_n(const char *s, int n);
kl kl_ok(kl x);
kl kl_fail(kl e);
kl kl_secret(kl x);
kl kl_untrusted(kl x);
kl kl_arr_new(void);
kl kl_arr_lit(int n, ...);
kl kl_rec_new(const char *type);
void kl_rec_set(kl r, const char *field, kl v);
kl kl_rec_get(kl r, const char *field);
kl kl_enum0(const char *variant);                 /* nullary variant */
kl kl_enumN(const char *variant, int n, ...);     /* variant with payload */

/* ---- operations ---- */
kl  kl_binop(const char *op, kl a, kl b);
kl  kl_neg(kl a);
kl  kl_bnot(kl a);
int kl_truthy(kl v);
int kl_len(kl v);
int kl_eq(kl a, kl b);
kl  kl_index(kl base, kl idx);
kl  kl_slice(kl base, kl lo, kl hi);
void kl_setindex(kl base, kl idx, kl v);
kl  kl_field(kl base, const char *name);
void kl_setfield(kl base, const char *name, kl v);
kl  kl_qq(kl a, kl b);            /* ?? : a if not none else b */
kl  kl_range(kl lo, kl hi);

/* pattern matching on enums / options / results */
int kl_is_variant(kl v, const char *variant);
int kl_is_none(kl v);
int kl_is_some(kl v);             /* not none */
kl  kl_payload(kl v, int i);
kl  kl_some_val(kl v);            /* unwrap ok(x)→x else x */

/* string interpolation: build with context-aware escaping (C5) */
typedef struct { char *p; int n, cap; } kl_sb;
void kl_sb_init(kl_sb *b);
void kl_sb_str(kl_sb *b, kl v);            /* append v stringified (raw) */
void kl_sb_esc(kl_sb *b, const char *ctx, kl v);  /* append escaped for ctx */
void kl_sb_lit(kl_sb *b, const char *lit); /* append literal */
kl   kl_sb_finish(kl_sb *b, const char *ctx);    /* ctx=NULL → plain string */

/* ---- effects: one-shot, in-scope Fail handlers (mirrors the interpreter) ---- */
typedef struct KlHandler {
    int is_boundary;          /* function boundary: turn Fail into a fail value */
    jmp_buf abort;            /* longjmp here to terminate the execute/boundary */
    kl result;                /* value produced by the handler/boundary */
    struct KlHandler *prev;
} KlHandler;

typedef struct KlPerform {
    jmp_buf resume;           /* longjmp here on `resume v` */
    kl resume_val;
    struct KlPerform *prev;
} KlPerform;

void kl_push_handler(KlHandler *h, int is_boundary);
void kl_pop_handler(KlHandler *h);
kl   kl_perform_fail(kl err);       /* find handler; run clause or resume */
kl   kl_resume(kl v);               /* condition/restart */
kl   kl_try(kl v);                  /* `e?` : ok→inner, fail→perform, else passthrough */
KlPerform *kl_perform_top(void);    /* for codegen's resume scaffolding */
void kl_set_resume(kl v);

/* ---- builtins, methods, capabilities ---- */
kl kl_builtin(const char *name, kl *args, int n);
kl kl_method(kl recv, const char *name, kl *args, int n);
kl kl_system(int argc, char **argv);     /* the one root capability */

/* ---- diagnostics / traps ---- */
void kl_panic(const char *code, const char *fmt, ...);
void kl_main_done(kl ret);               /* report an unhandled fail from main */

/* refinement check at a binding/validation boundary (runtime; SMT in Part IV) */
kl kl_refine_int(kl v, const char *pred, int64_t bound, const char *what);
kl kl_coerce(kl v, const char *type);    /* numeric coercion toward a type */

#endif /* KEELRT_H */
