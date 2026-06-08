/* ============================================================================
 * keelrt.c — runtime for compiled Keel programs (the codegen target).
 * Mirrors the Stage-0 interpreter's value semantics so compiled binaries are
 * observably equivalent to `keel run` (Tier 4.3). Targets the self-hosting
 * subset (Keel-Core). Allocator is the documented interim arena (Part IV
 * replaces it with the ownership-directed allocator).
 * ==========================================================================*/
#include "keelrt.h"
#include <stdarg.h>
#include <math.h>
#include <unistd.h>

/* ----------------------------------------------------------------- arena */
typedef struct KAlloc { struct KAlloc *next; } KAlloc;
static KAlloc *g_allocs = NULL;
void *kl_alloc(size_t n){
    KAlloc *a = (KAlloc*)calloc(1, sizeof(KAlloc)+n);
    if(!a){ fprintf(stderr,"keel: out of memory\n"); exit(70); }
    a->next=g_allocs; g_allocs=a; return (void*)(a+1);
}
char *kl_strndup(const char *s, size_t n){ char *p=(char*)kl_alloc(n+1); memcpy(p,s,n); p[n]=0; return p; }
void kl_free_all(void){ KAlloc *a=g_allocs; while(a){ KAlloc *n=a->next; free(a); a=n; } g_allocs=NULL; }

static kl mk(KKind k){ kl v=(kl)kl_alloc(sizeof(kl_obj)); v->kind=k; return v; }

/* ---- constructors ---- */
static kl g_none=NULL, g_true=NULL, g_false=NULL;
kl kl_none(void){ if(!g_none) g_none=mk(K_NONE); return g_none; }
kl kl_int(int64_t i){ kl v=mk(K_INT); v->as.i=i; return v; }
kl kl_float(double f){ kl v=mk(K_FLOAT); v->as.f=f; return v; }
kl kl_bool(int b){ if(!g_true){g_true=mk(K_BOOL);g_true->as.b=1;g_false=mk(K_BOOL);g_false->as.b=0;} return b?g_true:g_false; }
kl kl_str_n(const char *s,int n){ kl v=mk(K_STR); v->as.str.s=kl_strndup(s,n); v->as.str.len=n; return v; }
kl kl_str(const char *s){ return kl_str_n(s,(int)strlen(s)); }
kl kl_ok(kl x){ kl v=mk(K_OK); v->as.inner=x; return v; }
kl kl_fail(kl e){ kl v=mk(K_FAIL); v->as.inner=e?e:kl_none(); return v; }
kl kl_secret(kl x){ kl v=mk(K_SECRET); v->as.inner=x; return v; }
kl kl_untrusted(kl x){ kl v=mk(K_UNTRUSTED); v->as.inner=x; return v; }

kl kl_arr_new(void){ kl v=mk(K_ARR); v->as.arr.items=NULL; v->as.arr.len=0; v->as.arr.cap=0; return v; }
static void arr_push(kl a, kl x){
    if(a->as.arr.len==a->as.arr.cap){ int nc=a->as.arr.cap?a->as.arr.cap*2:4;
        kl *ni=(kl*)kl_alloc(sizeof(kl)*nc);
        for(int i=0;i<a->as.arr.len;i++) ni[i]=a->as.arr.items[i];
        a->as.arr.items=ni; a->as.arr.cap=nc; }
    a->as.arr.items[a->as.arr.len++]=x;
}
kl kl_arr_lit(int n, ...){ kl a=kl_arr_new(); va_list ap; va_start(ap,n);
    for(int i=0;i<n;i++) arr_push(a, va_arg(ap,kl)); va_end(ap); return a; }

kl kl_rec_new(const char *type){ kl v=mk(K_REC); v->as.rec.type=kl_strndup(type,strlen(type));
    v->as.rec.fields=NULL; v->as.rec.nf=0; v->as.rec.cap=0; return v; }
void kl_rec_set(kl r, const char *field, kl val){
    for(int i=0;i<r->as.rec.nf;i++) if(!strcmp(r->as.rec.fields[i].name,field)){ r->as.rec.fields[i].val=val; return; }
    if(r->as.rec.nf==r->as.rec.cap){ int nc=r->as.rec.cap?r->as.rec.cap*2:4;
        KField *nf=(KField*)kl_alloc(sizeof(KField)*nc);
        for(int i=0;i<r->as.rec.nf;i++) nf[i]=r->as.rec.fields[i];
        r->as.rec.fields=nf; r->as.rec.cap=nc; }
    r->as.rec.fields[r->as.rec.nf].name=kl_strndup(field,strlen(field));
    r->as.rec.fields[r->as.rec.nf].val=val; r->as.rec.nf++;
}
kl kl_rec_get(kl r, const char *field){
    if(r->kind==K_REC) for(int i=0;i<r->as.rec.nf;i++) if(!strcmp(r->as.rec.fields[i].name,field)) return r->as.rec.fields[i].val;
    return NULL;
}
kl kl_enum0(const char *variant){ kl v=mk(K_ENUM); v->as.en.type=NULL; v->as.en.variant=kl_strndup(variant,strlen(variant)); v->as.en.payload=NULL; v->as.en.np=0; return v; }
kl kl_enumN(const char *variant, int n, ...){ kl v=mk(K_ENUM); v->as.en.type=NULL;
    v->as.en.variant=kl_strndup(variant,strlen(variant)); v->as.en.np=n;
    v->as.en.payload= n? (kl*)kl_alloc(sizeof(kl)*n):NULL;
    va_list ap; va_start(ap,n); for(int i=0;i<n;i++) v->as.en.payload[i]=va_arg(ap,kl); va_end(ap); return v; }

/* ----------------------------------------------------------- formatting */
static void to_sb(kl_sb *b, kl v, int repr);
static void sb_putn(kl_sb *b, const char *s, int n){
    if(b->n+n+1>b->cap){ if(!b->cap)b->cap=16; while(b->n+n+1>b->cap)b->cap*=2; b->p=(char*)realloc(b->p,b->cap); }
    memcpy(b->p+b->n,s,n); b->n+=n; b->p[b->n]=0;
}
static void sb_puts(kl_sb *b,const char *s){ sb_putn(b,s,(int)strlen(s)); }
void kl_sb_init(kl_sb *b){ b->cap=16; b->p=(char*)malloc(16); b->n=0; b->p[0]=0; }

static void to_sb(kl_sb *b, kl v, int repr){
    char t[64];
    switch(v->kind){
        case K_NONE: sb_puts(b,"none"); break;
        case K_INT: snprintf(t,sizeof t,"%lld",(long long)v->as.i); sb_puts(b,t); break;
        case K_FLOAT: snprintf(t,sizeof t,"%g",v->as.f); sb_puts(b,t); break;
        case K_BOOL: sb_puts(b,v->as.b?"true":"false"); break;
        case K_STR:
            if(repr){ sb_puts(b,"\""); sb_putn(b,v->as.str.s,v->as.str.len); sb_puts(b,"\""); }
            else sb_putn(b,v->as.str.s,v->as.str.len);
            break;
        case K_CTXSTR: sb_puts(b,v->as.ctxstr.ctx); sb_puts(b,"\""); sb_puts(b,v->as.ctxstr.s); sb_puts(b,"\""); break;
        case K_SECRET: sb_puts(b,"Secret(<redacted>)"); break;
        case K_UNTRUSTED: sb_puts(b,"Untrusted("); to_sb(b,v->as.inner,1); sb_puts(b,")"); break;
        case K_ARR: sb_puts(b,"["); for(int i=0;i<v->as.arr.len;i++){ if(i)sb_puts(b,", "); to_sb(b,v->as.arr.items[i],1);} sb_puts(b,"]"); break;
        case K_OK: sb_puts(b,"ok("); to_sb(b,v->as.inner,1); sb_puts(b,")"); break;
        case K_FAIL: sb_puts(b,"fail("); to_sb(b,v->as.inner,1); sb_puts(b,")"); break;
        case K_REC: sb_puts(b,v->as.rec.type[0]?v->as.rec.type:""); sb_puts(b,"{");
            for(int i=0;i<v->as.rec.nf;i++){ if(i)sb_puts(b,", "); sb_puts(b,v->as.rec.fields[i].name); sb_puts(b,": "); to_sb(b,v->as.rec.fields[i].val,1);} sb_puts(b,"}"); break;
        case K_ENUM: sb_puts(b,v->as.en.variant);
            if(v->as.en.np){ sb_puts(b,"("); for(int i=0;i<v->as.en.np;i++){ if(i)sb_puts(b,", "); to_sb(b,v->as.en.payload[i],1);} sb_puts(b,")"); } break;
        default: sb_puts(b,"<value>");
    }
}
static char *to_cstr(kl v, int repr){ kl_sb b; kl_sb_init(&b); to_sb(&b,v,repr); return b.p; }

void kl_sb_lit(kl_sb *b, const char *lit){ sb_puts(b,lit); }
void kl_sb_str(kl_sb *b, kl v){ to_sb(b,v,0); }
void kl_sb_esc(kl_sb *b, const char *ctx, kl v){ char *raw=to_cstr(v,0); char *e=kesc_for(ctx,raw); sb_puts(b,e); free(e); free(raw); }
kl kl_sb_finish(kl_sb *b, const char *ctx){
    kl r;
    if(ctx){ r=mk(K_CTXSTR); r->as.ctxstr.ctx=kl_strndup(ctx,strlen(ctx)); r->as.ctxstr.s=kl_strndup(b->p,b->n); }
    else r=kl_str_n(b->p,b->n);
    free(b->p); return r;
}

/* ------------------------------------------------------------ equality */
int kl_eq(kl a, kl b){
    if(a->kind!=b->kind){
        if(a->kind==K_INT&&b->kind==K_FLOAT) return (double)a->as.i==b->as.f;
        if(a->kind==K_FLOAT&&b->kind==K_INT) return a->as.f==(double)b->as.i;
        return 0;
    }
    switch(a->kind){
        case K_NONE: return 1;
        case K_INT: return a->as.i==b->as.i;
        case K_FLOAT: return a->as.f==b->as.f;
        case K_BOOL: return a->as.b==b->as.b;
        case K_STR: return a->as.str.len==b->as.str.len && memcmp(a->as.str.s,b->as.str.s,a->as.str.len)==0;
        case K_ARR: if(a->as.arr.len!=b->as.arr.len) return 0;
            for(int i=0;i<a->as.arr.len;i++) if(!kl_eq(a->as.arr.items[i],b->as.arr.items[i])) return 0; return 1;
        case K_ENUM: if(strcmp(a->as.en.variant,b->as.en.variant)||a->as.en.np!=b->as.en.np) return 0;
            for(int i=0;i<a->as.en.np;i++) if(!kl_eq(a->as.en.payload[i],b->as.en.payload[i])) return 0; return 1;
        case K_REC: if(a->as.rec.nf!=b->as.rec.nf) return 0;
            for(int i=0;i<a->as.rec.nf;i++){ if(strcmp(a->as.rec.fields[i].name,b->as.rec.fields[i].name)) return 0;
                if(!kl_eq(a->as.rec.fields[i].val,b->as.rec.fields[i].val)) return 0; } return 1;
        default: return a==b;
    }
}
int kl_truthy(kl v){
    switch(v->kind){ case K_NONE: case K_FAIL: return 0; case K_BOOL: return v->as.b;
        case K_INT: return v->as.i!=0; case K_STR: return v->as.str.len>0; case K_ARR: return v->as.arr.len>0;
        case K_OK: return 1; default: return 1; }
}
int kl_len(kl v){ if(v->kind==K_ARR) return v->as.arr.len; if(v->kind==K_STR) return v->as.str.len; return 0; }

/* ------------------------------------------------------------ arithmetic */
static double as_dbl(kl v){ return v->kind==K_FLOAT?v->as.f:(double)v->as.i; }
kl kl_binop(const char *op, kl a, kl b){
    /* array broadcasting */
    if((a->kind==K_ARR||b->kind==K_ARR) && (op[0]=='+'||op[0]=='-'||op[0]=='*'||op[0]=='/')&&op[1]==0){
        if(a->kind==K_ARR&&b->kind==K_ARR){
            if(a->as.arr.len!=b->as.arr.len) kl_panic("runtime.panic","broadcast shape mismatch");
            kl o=kl_arr_new(); for(int i=0;i<a->as.arr.len;i++) arr_push(o,kl_binop(op,a->as.arr.items[i],b->as.arr.items[i])); return o; }
        kl arr=a->kind==K_ARR?a:b; kl o=kl_arr_new();
        for(int i=0;i<arr->as.arr.len;i++) o=(a->kind==K_ARR)? (arr_push(o,kl_binop(op,arr->as.arr.items[i],b)),o) : (arr_push(o,kl_binop(op,a,arr->as.arr.items[i])),o);
        return o;
    }
    if(!strcmp(op,"==")) return kl_bool(kl_eq(a,b));
    if(!strcmp(op,"!=")) return kl_bool(!kl_eq(a,b));
    if(a->kind==K_STR && b->kind==K_STR && !strcmp(op,"+")){
        kl_sb sb; kl_sb_init(&sb); sb_putn(&sb,a->as.str.s,a->as.str.len); sb_putn(&sb,b->as.str.s,b->as.str.len);
        return kl_str_n(sb.p,sb.n);
    }
    if(a->kind==K_STR && b->kind==K_STR){
        int m=a->as.str.len<b->as.str.len?a->as.str.len:b->as.str.len;
        int c=memcmp(a->as.str.s,b->as.str.s,m);
        if(c==0) c=(a->as.str.len<b->as.str.len)?-1:(a->as.str.len>b->as.str.len?1:0);
        if(!strcmp(op,"<")) return kl_bool(c<0); if(!strcmp(op,"<=")) return kl_bool(c<=0);
        if(!strcmp(op,">")) return kl_bool(c>0); if(!strcmp(op,">=")) return kl_bool(c>=0);
    }
    if(a->kind==K_CTXSTR||b->kind==K_CTXSTR){
        if(!strcmp(op,"+")) kl_panic("type.sink","a plain string cannot be concatenated into a typed-string context");
    }
    if(a->kind==K_FLOAT||b->kind==K_FLOAT){
        double x=as_dbl(a),y=as_dbl(b);
        if(!strcmp(op,"+"))return kl_float(x+y); if(!strcmp(op,"-"))return kl_float(x-y);
        if(!strcmp(op,"*"))return kl_float(x*y); if(!strcmp(op,"/")){ if(y==0)kl_panic("runtime.panic","division by zero"); return kl_float(x/y); }
        if(!strcmp(op,"%"))return kl_float(fmod(x,y)); if(!strcmp(op,"**"))return kl_float(pow(x,y));
        if(!strcmp(op,"<"))return kl_bool(x<y); if(!strcmp(op,"<="))return kl_bool(x<=y);
        if(!strcmp(op,">"))return kl_bool(x>y); if(!strcmp(op,">="))return kl_bool(x>=y);
    }
    if(a->kind==K_INT&&b->kind==K_INT){
        int64_t x=a->as.i,y=b->as.i,r;
        if(!strcmp(op,"+")){ if(kchk_add(x,y,&r))kl_panic("runtime.panic","integer overflow in +"); return kl_int(r); }
        if(!strcmp(op,"-")){ if(kchk_sub(x,y,&r))kl_panic("runtime.panic","integer overflow in -"); return kl_int(r); }
        if(!strcmp(op,"*")){ if(kchk_mul(x,y,&r))kl_panic("runtime.panic","integer overflow in *"); return kl_int(r); }
        if(!strcmp(op,"/")){ if(y==0)kl_panic("runtime.panic","division by zero"); return kl_int(x/y); }
        if(!strcmp(op,"%")){ if(y==0)kl_panic("runtime.panic","division by zero"); return kl_int(x%y); }
        if(!strcmp(op,"**")){ int64_t rr=1; for(int64_t k=0;k<y;k++){ if(kchk_mul(rr,x,&rr))kl_panic("runtime.panic","overflow in **"); } return kl_int(rr); }
        if(!strcmp(op,"&"))return kl_int(x&y); if(!strcmp(op,"|"))return kl_int(x|y); if(!strcmp(op,"^"))return kl_int(x^y);
        if(!strcmp(op,"<<"))return kl_int(x<<y); if(!strcmp(op,">>"))return kl_int(x>>y);
        if(!strcmp(op,"<"))return kl_bool(x<y); if(!strcmp(op,"<="))return kl_bool(x<=y);
        if(!strcmp(op,">"))return kl_bool(x>y); if(!strcmp(op,">="))return kl_bool(x>=y);
    }
    kl_panic("runtime.panic","unsupported operands for '%s'",op); return kl_none();
}
kl kl_neg(kl a){ if(a->kind==K_INT)return kl_int(-a->as.i); if(a->kind==K_FLOAT)return kl_float(-a->as.f); kl_panic("runtime.panic","cannot negate"); return kl_none(); }
kl kl_bnot(kl a){ if(a->kind==K_INT)return kl_int(~a->as.i); kl_panic("runtime.panic","~ requires int"); return kl_none(); }
kl kl_qq(kl a, kl b){ return a->kind==K_NONE? b : a; }
kl kl_range(kl lo, kl hi){ int64_t l=lo->as.i,h=hi->as.i; if(h<l)h=l; kl a=kl_arr_new(); for(int64_t i=l;i<h;i++) arr_push(a,kl_int(i)); return a; }

/* ------------------------------------------------------------ index/field */
kl kl_index(kl base, kl idx){
    if(base->kind==K_ARR){ int64_t i=idx->as.i; if(i<0)i+=base->as.arr.len;
        if(i<0||i>=base->as.arr.len) kl_panic("runtime.panic","index %lld out of bounds (len %d)",(long long)idx->as.i,base->as.arr.len);
        return base->as.arr.items[i]; }
    if(base->kind==K_STR){ int64_t i=idx->as.i; if(i<0)i+=base->as.str.len;
        if(i<0||i>=base->as.str.len) kl_panic("runtime.panic","string index out of bounds");
        return kl_str_n(base->as.str.s+i,1); }
    if(base->kind==K_REC){ kl f=kl_rec_get(base,to_cstr(idx,0)); return f?f:kl_none(); }
    kl_panic("runtime.panic","cannot index value"); return kl_none();
}
kl kl_slice(kl base, kl lo, kl hi){
    int64_t l = lo?lo->as.i:0;
    int64_t len = base->kind==K_ARR?base->as.arr.len:base->as.str.len;
    int64_t h = hi?hi->as.i:len;
    if(l<0)l=0; if(h>len)h=len; if(h<l)h=l;
    if(base->kind==K_ARR){ kl o=kl_arr_new(); for(int64_t i=l;i<h;i++) arr_push(o,base->as.arr.items[i]); return o; }
    return kl_str_n(base->as.str.s+l,(int)(h-l));
}
void kl_setindex(kl base, kl idx, kl v){
    if(base->kind!=K_ARR) kl_panic("runtime.panic","cannot index-assign");
    int64_t i=idx->as.i; if(i<0)i+=base->as.arr.len;
    if(i<0||i>=base->as.arr.len) kl_panic("runtime.panic","index out of bounds");
    base->as.arr.items[i]=v;
}
kl kl_field(kl base, const char *name){
    if(base->kind==K_UNTRUSTED) base=base->as.inner;
    if(base->kind==K_REC){ kl f=kl_rec_get(base,name); if(f)return f; kl_panic("type.field","no field `%s` on %s",name,base->as.rec.type[0]?base->as.rec.type:"record"); }
    if(base->kind==K_ENUM && !strcmp(name,"name")) return kl_str(base->as.en.variant);
    if(base->kind==K_NONE) return kl_none();
    if(base->kind==K_ARR && (!strcmp(name,"len")||!strcmp(name,"size")||!strcmp(name,"count"))) return kl_int(base->as.arr.len);
    if(base->kind==K_STR && (!strcmp(name,"len")||!strcmp(name,"size"))) return kl_int(base->as.str.len);
    kl_panic("type.field","cannot access field `%s`",name); return kl_none();
}
void kl_setfield(kl base, const char *name, kl v){
    if(base->kind!=K_REC) kl_panic("runtime.panic","cannot assign field of non-record");
    kl_rec_set(base,name,v);
}

/* pattern matching helpers */
int kl_is_variant(kl v, const char *variant){
    if(v->kind==K_ENUM) return strcmp(v->as.en.variant,variant)==0;
    if(v->kind==K_OK && !strcmp(variant,"ok")) return 1;
    if(v->kind==K_FAIL && !strcmp(variant,"fail")) return 1;
    return 0;
}
int kl_is_none(kl v){ return v->kind==K_NONE; }
int kl_is_some(kl v){ return v->kind!=K_NONE; }
kl  kl_payload(kl v, int i){ if(v->kind==K_ENUM&&i<v->as.en.np) return v->as.en.payload[i];
    if((v->kind==K_OK||v->kind==K_FAIL)&&i==0) return v->as.inner; return kl_none(); }
kl  kl_some_val(kl v){ return (v->kind==K_OK)? v->as.inner : v; }

/* --------------------------------------------------------------- effects */
static KlHandler *g_handlers=NULL;
static KlPerform *g_perform=NULL;
void kl_push_handler(KlHandler *h, int is_boundary){ h->is_boundary=is_boundary; h->result=NULL; h->prev=g_handlers; g_handlers=h; }
void kl_pop_handler(KlHandler *h){ if(g_handlers==h) g_handlers=h->prev; }
KlPerform *kl_perform_top(void){ return g_perform; }
void kl_set_resume(kl v){ if(g_perform) g_perform->resume_val=v; }

kl kl_perform_fail(kl err){
    KlHandler *h=g_handlers;
    if(!h){ kl_panic("runtime.panic","unhandled failure: %s", to_cstr(err,0)); }
    /* function boundary: turn the Fail into a fail() value and abort to it */
    h->result=kl_fail(err);
    longjmp(h->abort,1);
    return kl_none();
}
kl kl_resume(kl v){
    if(!g_perform) kl_panic("runtime.panic","`resume` used outside a handler");
    g_perform->resume_val=v;
    longjmp(g_perform->resume,1);
    return kl_none();
}
kl kl_try(kl v){
    if(v->kind==K_FAIL) return kl_perform_fail(v->as.inner);
    if(v->kind==K_OK) return v->as.inner;
    return v;
}

/* --------------------------------------------------------- refinement/coerce */
kl kl_coerce(kl v, const char *type){
    if(v->kind==K_UNTRUSTED) v=v->as.inner;
    if(type){
        if((!strcmp(type,"f64")||!strcmp(type,"f32")||!strcmp(type,"float")) && v->kind==K_INT) return kl_float((double)v->as.i);
        if(!strcmp(type,"int") && v->kind==K_FLOAT && v->as.f==(double)(int64_t)v->as.f) return kl_int((int64_t)v->as.f);
    }
    return v;
}
kl kl_refine_int(kl v, const char *pred, int64_t bound, const char *what){
    if(v->kind==K_UNTRUSTED) v=v->as.inner;
    int64_t x = v->kind==K_INT? v->as.i : 0;
    int ok=1;
    if(!strcmp(pred,">="))ok=x>=bound; else if(!strcmp(pred,">"))ok=x>bound;
    else if(!strcmp(pred,"<="))ok=x<=bound; else if(!strcmp(pred,"<"))ok=x<bound;
    else if(!strcmp(pred,"=="))ok=x==bound; else if(!strcmp(pred,"!="))ok=x!=bound;
    if(!ok) kl_panic("runtime.panic","refinement violated for %s: %lld fails `where %s %lld`",what,(long long)x,pred,(long long)bound);
    return v;
}

/* ------------------------------------------------------------- diagnostics */
static char g_code[64]="runtime.panic";
void kl_panic(const char *code, const char *fmt, ...){
    char msg[512]; va_list ap; va_start(ap,fmt); vsnprintf(msg,sizeof msg,fmt,ap); va_end(ap);
    snprintf(g_code,sizeof g_code,"%s",code?code:"runtime.panic");
    fprintf(stderr,"runtime error[%s]: %s\n",g_code,msg);
    exit(70);
}
void kl_main_done(kl ret){ if(ret&&ret->kind==K_FAIL){ fprintf(stderr,"main failed: %s\n",to_cstr(ret->as.inner,0)); exit(70);} }

/* --------------------------------------------------------------- builtins */
kl kl_builtin(const char *name, kl *a, int n){
    if(!strcmp(name,"println")||!strcmp(name,"print")){
        kl_sb b; kl_sb_init(&b); for(int i=0;i<n;i++){ if(i)sb_puts(&b," "); to_sb(&b,a[i],0); }
        fputs(b.p,stdout); if(!strcmp(name,"println")) fputc('\n',stdout); free(b.p); return kl_none();
    }
    if(!strcmp(name,"len")){ kl v=a[0]; if(v->kind==K_ARR)return kl_int(v->as.arr.len); if(v->kind==K_STR)return kl_int(v->as.str.len); return kl_int(0); }
    if(!strcmp(name,"str")) return kl_str(to_cstr(a[0],0));
    if(!strcmp(name,"repr")) return kl_str(to_cstr(a[0],1));
    if(!strcmp(name,"int")){ kl v=a[0]; if(v->kind==K_FLOAT)return kl_int((int64_t)v->as.f); if(v->kind==K_STR)return kl_int(strtoll(v->as.str.s,NULL,10)); return v; }
    if(!strcmp(name,"float")) return kl_float(as_dbl(a[0]));
    if(!strcmp(name,"some")) return a[0];
    if(!strcmp(name,"ok")) return kl_ok(n?a[0]:kl_none());
    if(!strcmp(name,"fail")) return kl_fail(n?a[0]:kl_none());
    if(!strcmp(name,"secret")) return kl_secret(a[0]);
    if(!strcmp(name,"reveal")){ kl v=a[0]; return v->kind==K_SECRET? v->as.inner : v; }
    if(!strcmp(name,"untrusted")) return kl_untrusted(a[0]);
    if(!strcmp(name,"range")){ if(n==1)return kl_range(kl_int(0),a[0]); return kl_range(a[0],a[1]); }
    if(!strcmp(name,"abs")){ kl v=a[0]; if(v->kind==K_INT)return kl_int(v->as.i<0?-v->as.i:v->as.i); if(v->kind==K_FLOAT)return kl_float(fabs(v->as.f)); return v; }
    if(!strcmp(name,"min")){ kl m=a[0]; for(int i=1;i<n;i++) if(kl_truthy(kl_binop("<",a[i],m)))m=a[i]; return m; }
    if(!strcmp(name,"max")){ kl m=a[0]; for(int i=1;i<n;i++) if(kl_truthy(kl_binop(">",a[i],m)))m=a[i]; return m; }
    if(!strcmp(name,"abort")) kl_panic("runtime.panic","abort: %s", n?to_cstr(a[0],0):"(no message)");
    if(!strcmp(name,"ord")){ kl v=a[0]; return kl_int(v->kind==K_STR&&v->as.str.len?(unsigned char)v->as.str.s[0]:0); }
    if(!strcmp(name,"chr")){ char c=(char)a[0]->as.i; return kl_str_n(&c,1); }
    if(!strcmp(name,"is_digit")){ kl v=a[0]; return kl_bool(v->kind==K_STR&&v->as.str.len&&isdigit((unsigned char)v->as.str.s[0])); }
    if(!strcmp(name,"is_alpha")){ kl v=a[0]; char c=v->kind==K_STR&&v->as.str.len?v->as.str.s[0]:0; return kl_bool(isalpha((unsigned char)c)||c=='_'); }
    if(!strcmp(name,"is_alnum")){ kl v=a[0]; char c=v->kind==K_STR&&v->as.str.len?v->as.str.s[0]:0; return kl_bool(isalnum((unsigned char)c)||c=='_'); }
    if(!strcmp(name,"is_space")){ kl v=a[0]; return kl_bool(v->kind==K_STR&&v->as.str.len&&isspace((unsigned char)v->as.str.s[0])); }
    if(!strcmp(name,"type_name")){ kl v=a[0]; const char*t; switch(v->kind){case K_INT:t="int";break;case K_FLOAT:t="f64";break;case K_STR:t="string";break;case K_BOOL:t="bool";break;case K_ARR:t="array";break;case K_REC:t=v->as.rec.type;break;case K_ENUM:t="enum";break;case K_NONE:t="none";break;default:t="value";} return kl_str(t); }
    kl_panic("name.undefined","unknown builtin `%s`",name); return kl_none();
}

/* ------------------------------------------------------------- methods */
kl kl_method(kl recv, const char *m, kl *a, int n){
    if(recv->kind==K_UNTRUSTED) recv=recv->as.inner;
    if(recv->kind==K_ARR){
        int N=recv->as.arr.len; kl *it=recv->as.arr.items;
        if(!strcmp(m,"len")||!strcmp(m,"size")||!strcmp(m,"count")) return kl_int(N);
        if(!strcmp(m,"first")) return N?it[0]:kl_none();
        if(!strcmp(m,"last")) return N?it[N-1]:kl_none();
        if(!strcmp(m,"is_empty")) return kl_bool(N==0);
        if(!strcmp(m,"push")){ arr_push(recv,a[0]); return recv; }
        if(!strcmp(m,"contains")){ for(int i=0;i<N;i++) if(kl_eq(it[i],a[0])) return kl_bool(1); return kl_bool(0); }
        if(!strcmp(m,"reverse")){ kl o=kl_arr_new(); for(int i=N-1;i>=0;i--) arr_push(o,it[i]); return o; }
        if(!strcmp(m,"sum")){ kl acc=kl_int(0); for(int i=0;i<N;i++) acc=kl_binop("+",acc,it[i]); return acc; }
        if(!strcmp(m,"join")){ kl_sb b; kl_sb_init(&b); const char*sep=n?a[0]->as.str.s:""; for(int i=0;i<N;i++){ if(i)sb_puts(&b,sep); char*s=to_cstr(it[i],0); sb_puts(&b,s);} return kl_str_n(b.p,b.n); }
        kl_panic("type.method","no array method `%s`",m);
    }
    if(recv->kind==K_STR){
        const char *s=recv->as.str.s; int L=recv->as.str.len;
        if(!strcmp(m,"len")||!strcmp(m,"size")) return kl_int(L);
        if(!strcmp(m,"upper")){ char*o=kl_strndup(s,L); for(int i=0;i<L;i++)o[i]=toupper((unsigned char)o[i]); return kl_str_n(o,L); }
        if(!strcmp(m,"lower")){ char*o=kl_strndup(s,L); for(int i=0;i<L;i++)o[i]=tolower((unsigned char)o[i]); return kl_str_n(o,L); }
        if(!strcmp(m,"trim")){ int x=0,y=L; while(x<y&&isspace((unsigned char)s[x]))x++; while(y>x&&isspace((unsigned char)s[y-1]))y--; return kl_str_n(s+x,y-x); }
        if(!strcmp(m,"contains")) return kl_bool(strstr(s,a[0]->as.str.s)!=NULL);
        if(!strcmp(m,"starts_with")) return kl_bool(strncmp(s,a[0]->as.str.s,strlen(a[0]->as.str.s))==0);
        if(!strcmp(m,"to_int")){ const char*p=s; while(*p&&isspace((unsigned char)*p))p++; if(!*p)return kl_fail(kl_str("empty input is not an integer")); char*end; long long v=strtoll(s,&end,10); while(*end&&isspace((unsigned char)*end))end++; if(*end)return kl_fail(kl_str("not an integer")); return kl_ok(kl_int(v)); }
        if(!strcmp(m,"split")){ const char*sep=a[0]->as.str.s; kl o=kl_arr_new(); int sl=(int)strlen(sep); const char*p=s,*q;
            if(sl==0){ for(int i=0;i<L;i++) arr_push(o,kl_str_n(s+i,1)); }
            else { while((q=strstr(p,sep))){ arr_push(o,kl_str_n(p,(int)(q-p))); p=q+sl; } arr_push(o,kl_str(p)); } return o; }
        kl_panic("type.method","no string method `%s`",m);
    }
    if(recv->kind==K_ENUM){ if(!strcmp(m,"name")) return kl_str(recv->as.en.variant); }
    if(recv->kind==K_INT){ if(!strcmp(m,"abs"))return kl_int(recv->as.i<0?-recv->as.i:recv->as.i); if(!strcmp(m,"to_f"))return kl_float((double)recv->as.i); }
    if(recv->kind==K_REC){
        const char *t=recv->as.rec.type;
        /* capability methods (Dir/File) — real file I/O within the cap root */
        if(!strcmp(t,"Dir")){
            kl rootv=kl_rec_get(recv,"root"); const char *root=rootv?rootv->as.str.s:".";
            if(!strcmp(m,"subtree")||!strcmp(m,"path")){
                char *res=NULL; { /* path containment, mirrors the interpreter */
                    const char *req=a[0]->as.str.s; int rootslash=root[0]=='/';
                    char *parts[256]; int np=0; char *rc=kl_strndup(root,strlen(root));
                    for(char*p=strtok(rc,"/");p&&np<256;p=strtok(NULL,"/")) parts[np++]=p;
                    int rd=np; char *qc=kl_strndup(req,strlen(req)); int esc=0;
                    for(char*p=strtok(qc,"/");p;p=strtok(NULL,"/")){ if(!strcmp(p,".")||!p[0])continue; if(!strcmp(p,"..")){ if(np>rd)np--; else {esc=1;break;} } else if(np<256) parts[np++]=p; }
                    if(!esc){ kl_sb b; kl_sb_init(&b); if(rootslash||np==0)sb_puts(&b,"/"); for(int i=0;i<np;i++){ if(i)sb_puts(&b,"/"); sb_puts(&b,parts[i]); } res=b.p; }
                }
                if(!res) return kl_perform_fail(kl_str("path traversal rejected: escapes capability root"));
                kl d=kl_rec_new("Dir"); kl_rec_set(d,"root",kl_str(res)); return d;
            }
            if(!strcmp(m,"open")){
                const char *req=a[0]->as.str.s;
                /* contain then read */
                kl sub=kl_method(recv,"subtree",a,1);
                if(sub->kind==K_FAIL) return sub;
                const char *full=kl_rec_get(sub,"root")->as.str.s;
                kl f=kl_rec_new("File"); kl_rec_set(f,"name",kl_str(full));
                if(n>1){ kl_rec_set(f,"content",a[1]); return kl_ok(f); }
                FILE *fp=fopen(full,"rb"); if(!fp) return kl_perform_fail(kl_str("no such file or unreadable within capability"));
                fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET); char *buf=(char*)kl_alloc(sz+1); size_t got=fread(buf,1,sz,fp); buf[got]=0; fclose(fp);
                kl_rec_set(f,"content",kl_str_n(buf,(int)got)); (void)req; return kl_ok(f);
            }
        }
        if(!strcmp(t,"File")){ if(!strcmp(m,"read")){ kl c=kl_rec_get(recv,"content"); return kl_ok(c?c:kl_str("")); }
            if(!strcmp(m,"name")) return kl_rec_get(recv,"name"); }
    }
    kl_panic("type.method","value has no method `%s`",m); return kl_none();
}

/* the one root capability handed to main(sys: System) */
kl kl_system(int argc, char **argv){
    kl sys=kl_rec_new("System");
    char cwd[4096]; if(!getcwd(cwd,sizeof cwd)) strcpy(cwd,"/");
    kl fs=kl_rec_new("Dir"); kl_rec_set(fs,"root",kl_str(cwd)); kl_rec_set(sys,"fs",fs);
    kl net=kl_rec_new("Net"); kl_rec_set(sys,"net",net);
    kl args=kl_arr_new(); for(int i=1;i<argc;i++) arr_push(args,kl_str(argv[i])); kl_rec_set(sys,"args",args);
    return sys;
}
