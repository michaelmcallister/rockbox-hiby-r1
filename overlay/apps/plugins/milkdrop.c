/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * "milkdrop" - a Milkdrop-style music visualiser for the HiBy R1 that parses
 * real .milk presets. There is no GPU on this device, so this is not projectM:
 * it ships a small NS-EEL expression engine (Milkdrop's per_frame equation
 * language) and drives a hand-written fixed-point software feedback renderer
 * from the preset's parameters.
 *
 *   Each frame: run per_frame_init (once) + per_frame, then evaluate per_pixel
 *   over a warp mesh (per-vertex zoom/rot/warp/dx/dy/cx/cy/sx/sy) and warp the
 *   feedback buffer by the interpolated source field -- Milkdrop's affine warp
 *   plus the time-animated sinusoidal warp term. Adds video echo, the nWaveMode
 *   waveforms (additive-capable), and a crossfade between presets.
 *
 * Presets are read from the /.rockbox/milkdrop directory (.milk files); with
 * none present a built-in default is used. Tap = next preset, hold = quit.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 ****************************************************************************/

#include "plugin.h"
#include "lib/helper.h"

/* ============================ EEL ENGINE ============================== *
 * Developed and unit-tested standalone with host gcc against libm. Pure
 * scalar C, no Rockbox or libc dependency.                               */

static double m_fabs(double x){ return x<0?-x:x; }
static double m_sin(double x)
{
    const double TWO_PI = 6.283185307179586, INV_TWO_PI = 0.15915494309189535;
    double k = x * INV_TWO_PI;
    long n = (long)(k >= 0 ? k + 0.5 : k - 0.5);
    double r = x - n * TWO_PI, r2 = r * r;
    return r * (0.9999966 + r2 * (-0.16664824 + r2 * (0.00830629 + r2 * (-0.00018363))));
}
static double m_cos(double x){ return m_sin(x + 1.5707963267948966); }
static double m_tan(double x){ double c = m_cos(x); return c == 0 ? 0 : m_sin(x) / c; }
static double m_sqrt(double x)
{
    double g; int i;
    if (x <= 0) return 0;
    g = x > 1.0 ? x * 0.5 : 1.0;
    for (i = 0; i < 8; i++) g = 0.5 * (g + x / g);
    return g;
}
static double m_ldexp2(double v, long k){ while(k>0){v*=2.0;k--;} while(k<0){v*=0.5;k++;} return v; }
static double m_exp(double x)
{
    double t, r, er; long k;
    if (x > 80.0) x = 80.0;
    if (x < -80.0) x = -80.0;
    t = x * 1.4426950408889634;
    k = (long)(t >= 0 ? t + 0.5 : t - 0.5);
    r = x - k * 0.6931471805599453;
    er = 1.0 + r*(1.0 + r*(0.5 + r*(0.16666666667 + r*(0.041666667 + r*0.0083333333))));
    return m_ldexp2(er, k);
}
static double m_log(double x)
{
    long e = 0; double y, y2, s;
    if (x <= 0) return -1e30;
    while (x >= 1.0) { x *= 0.5; e++; }
    while (x < 0.5) { x *= 2.0; e--; }
    y = (x - 1.0) / (x + 1.0); y2 = y * y;
    s = y * (2.0 + y2*(0.6666667 + y2*(0.4 + y2*(0.2857143 + y2*(0.2222222 + y2*0.1818)))));
    return s + e * 0.6931471805599453;
}
static double m_pow(double a, double b){ return a <= 0 ? 0 : m_exp(b * m_log(a)); }
static double m_atan(double x)
{
    double a = m_fabs(x), r;
    if (a > 1.0) r = 1.5707963267949 - a / (a * a + 0.28);
    else         r = a / (1.0 + 0.28 * a * a);
    return x < 0 ? -r : r;
}
static double m_atan2(double y, double x)
{
    const double PI = 3.14159265358979;
    if (x > 0) return m_atan(y / x);
    if (x < 0) return m_atan(y / x) + (y >= 0 ? PI : -PI);
    return y > 0 ? PI/2 : (y < 0 ? -PI/2 : 0);
}
static double m_asin(double x){ if(x<=-1)return -1.5707963; if(x>=1)return 1.5707963; return m_atan(x/m_sqrt(1-x*x)); }
static double m_acos(double x){ return 1.5707963267949 - m_asin(x); }
static unsigned int eel_rng = 0x12345678u;
static double m_rand(double n){ eel_rng = eel_rng*1103515245u + 12345u; return ((eel_rng>>8)&0xFFFFFF)/16777216.0 * (n<=0?1.0:n); }

#define EEL_MAXVARS  512
#define EEL_MAXCONST 1024
#define EEL_MAXCODE  16384

static char   eel_vname[EEL_MAXVARS][24];
static double eel_vval[EEL_MAXVARS];
static int    eel_vcount;
static double eel_const[EEL_MAXCONST];
static int    eel_ccount;

enum { OP_PUSHC, OP_PUSHV, OP_STORE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
       OP_NEG, OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE, OP_AND, OP_OR, OP_NOT,
       OP_CALL, OP_POP };
struct eel_instr { short op; short a; short b; };
static struct eel_instr eel_code[EEL_MAXCODE];
static int eel_codef;

enum { F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_ATAN2, F_ABS, F_SQRT,
       F_POW, F_EXP, F_LOG, F_LOG10, F_INT, F_SIGN, F_MIN, F_MAX, F_IF,
       F_ABOVE, F_BELOW, F_EQUAL, F_RAND, F_SQR, F_BNOT, F_BAND, F_BOR, F_NFUNCS };
static const char *eel_fnames[F_NFUNCS] = {
    "sin","cos","tan","asin","acos","atan","atan2","abs","sqrt",
    "pow","exp","log","log10","int","sign","min","max","if",
    "above","below","equal","rand","sqr","bnot","band","bor" };

static int eel_lower(int c){ return (c>='A'&&c<='Z')?c+32:c; }
static int eel_streq_ci(const char *a, const char *b, int blen)
{
    int i;
    for (i = 0; i < blen; i++)
        if (eel_lower((unsigned char)a[i]) != eel_lower((unsigned char)b[i]) || a[i] == 0) return 0;
    return a[blen] == 0;
}
static int eel_var(const char *name, int len)
{
    int i, j;
    if (len > 23) len = 23;
    for (i = 0; i < eel_vcount; i++)
        if (eel_streq_ci(eel_vname[i], name, len)) return i;
    if (eel_vcount >= EEL_MAXVARS) return 0;
    i = eel_vcount++;
    for (j = 0; j < len; j++) eel_vname[i][j] = (char)eel_lower((unsigned char)name[j]);
    eel_vname[i][len] = 0; eel_vval[i] = 0;
    return i;
}
static int eel_func(const char *name, int len)
{
    int i;
    for (i = 0; i < F_NFUNCS; i++)
        if (eel_streq_ci(eel_fnames[i], name, len)) return i;
    return -1;
}
static int eel_addconst(double v){ if(eel_ccount>=EEL_MAXCONST) return 0; eel_const[eel_ccount]=v; return eel_ccount++; }

enum { T_NUM, T_ID, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT, T_LP, T_RP,
       T_COMMA, T_SEMI, T_ASSIGN, T_LT, T_GT, T_LE, T_GE, T_EQ, T_NE,
       T_AND, T_OR, T_NOT, T_END, T_ERR };
static const char *eel_p;
static int    eel_tok, eel_tidlen;
static double eel_tnum;
static const char *eel_tid;
static int eel_isalpha(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int eel_isdigit(int c){ return c>='0'&&c<='9'; }
static int eel_isalnum(int c){ return eel_isalpha(c)||eel_isdigit(c); }
static void eel_next(void)
{
    const char *p = eel_p;
    while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
    while (p[0]=='/' && p[1]=='/') { while(*p&&*p!='\n')p++; while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++; }
    if (*p == 0) { eel_tok = T_END; eel_p = p; return; }
    if (eel_isdigit(*p) || (*p=='.' && eel_isdigit(p[1])))
    {
        double v = 0, frac;
        while (eel_isdigit(*p)) { v = v*10 + (*p-'0'); p++; }
        if (*p=='.') { p++; frac=0.1; while (eel_isdigit(*p)) { v += (*p-'0')*frac; frac*=0.1; p++; } }
        if (*p=='e'||*p=='E') { int sgn=1,ex=0,kk; p++; if(*p=='+')p++; else if(*p=='-'){sgn=-1;p++;} while(eel_isdigit(*p)){ex=ex*10+(*p-'0');p++;} if(sgn>0)for(kk=0;kk<ex;kk++)v*=10; else for(kk=0;kk<ex;kk++)v*=0.1; }
        eel_tnum = v; eel_tok = T_NUM; eel_p = p; return;
    }
    if (eel_isalpha(*p))
    {
        eel_tid = p; while (eel_isalnum(*p)) p++;
        eel_tidlen = (int)(p - eel_tid); eel_tok = T_ID; eel_p = p; return;
    }
    switch (*p)
    {
        case '+': eel_tok=T_PLUS; p++; break;
        case '-': eel_tok=T_MINUS; p++; break;
        case '*': eel_tok=T_STAR; p++; break;
        case '/': eel_tok=T_SLASH; p++; break;
        case '%': eel_tok=T_PCT; p++; break;
        case '(': eel_tok=T_LP; p++; break;
        case ')': eel_tok=T_RP; p++; break;
        case ',': eel_tok=T_COMMA; p++; break;
        case ';': eel_tok=T_SEMI; p++; break;
        case '=': if(p[1]=='='){eel_tok=T_EQ;p+=2;} else {eel_tok=T_ASSIGN;p++;} break;
        case '<': if(p[1]=='='){eel_tok=T_LE;p+=2;} else {eel_tok=T_LT;p++;} break;
        case '>': if(p[1]=='='){eel_tok=T_GE;p+=2;} else {eel_tok=T_GT;p++;} break;
        case '!': if(p[1]=='='){eel_tok=T_NE;p+=2;} else {eel_tok=T_NOT;p++;} break;
        case '&': eel_tok=T_AND; p++; if(*p=='&')p++; break;
        case '|': eel_tok=T_OR; p++; if(*p=='|')p++; break;
        default:  eel_tok=T_ERR; p++; break;
    }
    eel_p = p;
}
static int eel_ok;
static void eel_emit(int op,int a,int b){ if(eel_codef<EEL_MAXCODE){ eel_code[eel_codef].op=(short)op; eel_code[eel_codef].a=(short)a; eel_code[eel_codef].b=(short)b; eel_codef++; } }
static void eel_expr(void);
static void eel_primary(void)
{
    if (eel_tok == T_NUM) { eel_emit(OP_PUSHC, eel_addconst(eel_tnum), 0); eel_next(); }
    else if (eel_tok == T_LP) { eel_next(); eel_expr(); if(eel_tok==T_RP)eel_next(); else eel_ok=0; }
    else if (eel_tok == T_ID)
    {
        const char *name = eel_tid; int len = eel_tidlen;
        eel_next();
        if (eel_tok == T_LP)
        {
            int argc=0, fid=eel_func(name,len);
            eel_next();
            if (eel_tok != T_RP) { eel_expr(); argc++; while(eel_tok==T_COMMA){eel_next();eel_expr();argc++;} }
            if (eel_tok == T_RP) eel_next(); else eel_ok=0;
            if (fid < 0) eel_emit(OP_PUSHC, eel_addconst(0), 0);
            else eel_emit(OP_CALL, fid, argc);
        }
        else eel_emit(OP_PUSHV, eel_var(name,len), 0);
    }
    else { eel_emit(OP_PUSHC, eel_addconst(0), 0); eel_ok = 0; }
}
static void eel_unary(void)
{
    if (eel_tok==T_MINUS){ eel_next(); eel_unary(); eel_emit(OP_NEG,0,0); }
    else if (eel_tok==T_NOT){ eel_next(); eel_unary(); eel_emit(OP_NOT,0,0); }
    else if (eel_tok==T_PLUS){ eel_next(); eel_unary(); }
    else eel_primary();
}
static void eel_mul(void){ eel_unary(); while(eel_tok==T_STAR||eel_tok==T_SLASH||eel_tok==T_PCT){int o=eel_tok;eel_next();eel_unary();eel_emit(o==T_STAR?OP_MUL:o==T_SLASH?OP_DIV:OP_MOD,0,0);} }
static void eel_add(void){ eel_mul(); while(eel_tok==T_PLUS||eel_tok==T_MINUS){int o=eel_tok;eel_next();eel_mul();eel_emit(o==T_PLUS?OP_ADD:OP_SUB,0,0);} }
static void eel_rel(void){ eel_add(); while(eel_tok==T_LT||eel_tok==T_GT||eel_tok==T_LE||eel_tok==T_GE){int o=eel_tok;eel_next();eel_add();eel_emit(o==T_LT?OP_LT:o==T_GT?OP_GT:o==T_LE?OP_LE:OP_GE,0,0);} }
static void eel_eq(void){ eel_rel(); while(eel_tok==T_EQ||eel_tok==T_NE){int o=eel_tok;eel_next();eel_rel();eel_emit(o==T_EQ?OP_EQ:OP_NE,0,0);} }
static void eel_and_(void){ eel_eq(); while(eel_tok==T_AND){eel_next();eel_eq();eel_emit(OP_AND,0,0);} }
static void eel_or_(void){ eel_and_(); while(eel_tok==T_OR){eel_next();eel_and_();eel_emit(OP_OR,0,0);} }
static void eel_expr(void)
{
    eel_or_();
    if (eel_tok == T_ASSIGN)
    {
        if (eel_codef>0 && eel_code[eel_codef-1].op==OP_PUSHV)
        {
            int vi = eel_code[eel_codef-1].a;
            eel_codef--; eel_next(); eel_expr(); eel_emit(OP_STORE, vi, 0);
        }
        else { eel_ok=0; eel_next(); eel_expr(); }
    }
}
static int eel_compile(const char *src, int *start)
{
    *start = eel_codef; eel_ok = 1; eel_p = src; eel_next();
    while (eel_tok != T_END && eel_tok != T_ERR)
    {
        eel_expr(); eel_emit(OP_POP,0,0);
        if (eel_tok == T_SEMI) eel_next();
        else if (eel_tok == T_END) break;
        else eel_next();
    }
    return eel_codef - *start;
}
static double eel_call(int fid, double *a, int argc)
{
    double x = argc>0?a[0]:0, y = argc>1?a[1]:0, z = argc>2?a[2]:0;
    switch (fid)
    {
        case F_SIN:return m_sin(x); case F_COS:return m_cos(x); case F_TAN:return m_tan(x);
        case F_ASIN:return m_asin(x); case F_ACOS:return m_acos(x); case F_ATAN:return m_atan(x);
        case F_ATAN2:return m_atan2(x,y); case F_ABS:return m_fabs(x); case F_SQRT:return m_sqrt(x);
        case F_POW:return m_pow(x,y); case F_EXP:return m_exp(x); case F_LOG:return m_log(x);
        case F_LOG10:return m_log(x)*0.4342944819; case F_INT:return (double)(long)x;
        case F_SIGN:return x>0?1:(x<0?-1:0); case F_MIN:return x<y?x:y; case F_MAX:return x>y?x:y;
        case F_IF:return x!=0?y:z; case F_ABOVE:return x>y?1:0; case F_BELOW:return x<y?1:0;
        case F_EQUAL:return x==y?1:0; case F_RAND:return m_rand(x); case F_SQR:return x*x;
        case F_BNOT:return x!=0?0:1;
        case F_BAND:return (x!=0&&y!=0)?1:0; case F_BOR:return (x!=0||y!=0)?1:0;
    }
    return 0;
}
static void eel_run(int start, int len)
{
    static double st[256];
    double args[8];
    int sp = 0, pc;
    for (pc = start; pc < start + len; pc++)
    {
        struct eel_instr *in = &eel_code[pc];
        switch (in->op)
        {
            case OP_PUSHC: st[sp++]=eel_const[in->a]; break;
            case OP_PUSHV: st[sp++]=eel_vval[in->a]; break;
            case OP_STORE: eel_vval[in->a]=st[sp-1]; break;
            case OP_ADD: sp--; st[sp-1]+=st[sp]; break;
            case OP_SUB: sp--; st[sp-1]-=st[sp]; break;
            case OP_MUL: sp--; st[sp-1]*=st[sp]; break;
            case OP_DIV: sp--; st[sp-1]=st[sp]!=0?st[sp-1]/st[sp]:0; break;
            case OP_MOD: sp--; { long d=(long)st[sp]; st[sp-1]=d?(double)((long)st[sp-1]%d):0; } break;
            case OP_NEG: st[sp-1]=-st[sp-1]; break;
            case OP_LT: sp--; st[sp-1]=st[sp-1]< st[sp]?1:0; break;
            case OP_GT: sp--; st[sp-1]=st[sp-1]> st[sp]?1:0; break;
            case OP_LE: sp--; st[sp-1]=st[sp-1]<=st[sp]?1:0; break;
            case OP_GE: sp--; st[sp-1]=st[sp-1]>=st[sp]?1:0; break;
            case OP_EQ: sp--; st[sp-1]=st[sp-1]==st[sp]?1:0; break;
            case OP_NE: sp--; st[sp-1]=st[sp-1]!=st[sp]?1:0; break;
            case OP_AND: sp--; st[sp-1]=(st[sp-1]!=0&&st[sp]!=0)?1:0; break;
            case OP_OR:  sp--; st[sp-1]=(st[sp-1]!=0||st[sp]!=0)?1:0; break;
            case OP_NOT: st[sp-1]=st[sp-1]!=0?0:1; break;
            case OP_CALL: { int n=in->b,i; for(i=n-1;i>=0;i--)args[i]=st[--sp]; st[sp++]=eel_call(in->a,args,n); } break;
            case OP_POP: if(sp>0)sp--; break;
        }
        if (sp < 0) sp = 0;
        if (sp > 250) sp = 250;
    }
}
static void eel_reset(void){ eel_vcount=0; eel_ccount=0; eel_codef=0; }

/* ---- .milk parsing --------------------------------------------------- */
#define MILK_SRCMAX 8192
static char milk_pf[MILK_SRCMAX], milk_pp[MILK_SRCMAX], milk_pfi[2048];
static struct { short vi; double val; } milk_base[320];
static int milk_base_n, milk_pf_s, milk_pf_n, milk_pp_s, milk_pp_n;
static int VX_zoom,VX_rot,VX_cx,VX_cy,VX_dx,VX_dy,VX_sx,VX_sy,VX_warp,VX_decay,
           VX_gamma,VX_wmode,VX_wr,VX_wg,VX_wb,VX_wa,VX_wx,VX_wy,
           VX_time,VX_frame,VX_fps,VX_progress,VX_bass,VX_mid,VX_treb,
           VX_bass_att,VX_mid_att,VX_treb_att,
           VX_zoomexp,VX_warpanim,VX_warpscale,VX_x,VX_y,VX_rad,VX_ang,
           VX_echo_zoom,VX_echo_alpha,VX_echo_orient,VX_additive,
           VX_q1,VX_q2,VX_q3;
static double milk_atof(const char *s, const char *e)
{
    double v=0, sign=1, frac; int k;
    while (s<e && (*s==' '||*s=='\t')) s++;
    if (s<e && *s=='-'){sign=-1;s++;} else if(s<e&&*s=='+')s++;
    while (s<e && *s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;}
    if (s<e && *s=='.'){s++;frac=0.1;while(s<e&&*s>='0'&&*s<='9'){v+=(*s-'0')*frac;frac*=0.1;s++;}}
    if (s<e && (*s=='e'||*s=='E')){int es=1,ex=0;s++;if(s<e&&*s=='+')s++;else if(s<e&&*s=='-'){es=-1;s++;}while(s<e&&*s>='0'&&*s<='9'){ex=ex*10+(*s-'0');s++;}if(es>0)for(k=0;k<ex;k++)v*=10;else for(k=0;k<ex;k++)v*=0.1;}
    return v*sign;
}
static int milk_keyeq(const char *k,int kl,const char *lit){ int i; for(i=0;i<kl;i++) if(!lit[i]||eel_lower((unsigned char)k[i])!=eel_lower((unsigned char)lit[i]))return 0; return lit[kl]==0; }
static int milk_prefix(const char *k,int kl,const char *pre){ int i; for(i=0;pre[i];i++) if(i>=kl||eel_lower((unsigned char)k[i])!=eel_lower((unsigned char)pre[i]))return 0; return 1; }
static void milk_addbase(const char *name,int len,double v)
{
    int vi=eel_var(name,len), i;
    for (i=0;i<milk_base_n;i++) if(milk_base[i].vi==vi){milk_base[i].val=v;return;}
    if (milk_base_n<(int)(sizeof(milk_base)/sizeof(milk_base[0]))){ milk_base[milk_base_n].vi=(short)vi; milk_base[milk_base_n].val=v; milk_base_n++; }
}
static void milk_apply_base(void){ int i; for(i=0;i<milk_base_n;i++) eel_vval[milk_base[i].vi]=milk_base[i].val; }
static void milk_cache_vix(void)
{
    VX_zoom=eel_var("zoom",4); VX_rot=eel_var("rot",3); VX_cx=eel_var("cx",2); VX_cy=eel_var("cy",2);
    VX_dx=eel_var("dx",2); VX_dy=eel_var("dy",2); VX_sx=eel_var("sx",2); VX_sy=eel_var("sy",2);
    VX_warp=eel_var("warp",4); VX_decay=eel_var("decay",5); VX_gamma=eel_var("gamma",5); VX_wmode=eel_var("wave_mode",9);
    VX_wr=eel_var("wave_r",6); VX_wg=eel_var("wave_g",6); VX_wb=eel_var("wave_b",6); VX_wa=eel_var("wave_a",6);
    VX_wx=eel_var("wave_x",6); VX_wy=eel_var("wave_y",6);
    VX_time=eel_var("time",4); VX_frame=eel_var("frame",5); VX_fps=eel_var("fps",3); VX_progress=eel_var("progress",8);
    VX_bass=eel_var("bass",4); VX_mid=eel_var("mid",3); VX_treb=eel_var("treb",4);
    VX_bass_att=eel_var("bass_att",8); VX_mid_att=eel_var("mid_att",7); VX_treb_att=eel_var("treb_att",8);
    VX_zoomexp=eel_var("zoomexp",7); VX_warpanim=eel_var("warpanim",8); VX_warpscale=eel_var("warpscale",9);
    VX_x=eel_var("x",1); VX_y=eel_var("y",1); VX_rad=eel_var("rad",3); VX_ang=eel_var("ang",3);
    VX_echo_zoom=eel_var("echo_zoom",9); VX_echo_alpha=eel_var("echo_alpha",10);
    VX_echo_orient=eel_var("echo_orient",11); VX_additive=eel_var("additivewaves",13);
    VX_q1=eel_var("q1",2); VX_q2=eel_var("q2",2); VX_q3=eel_var("q3",2);
}
static void milk_set_defaults(void)
{
    milk_base_n=0;
    milk_addbase("zoom",4,1.0); milk_addbase("rot",3,0.0); milk_addbase("cx",2,0.5); milk_addbase("cy",2,0.5);
    milk_addbase("dx",2,0.0); milk_addbase("dy",2,0.0); milk_addbase("sx",2,1.0); milk_addbase("sy",2,1.0);
    milk_addbase("warp",4,1.0); milk_addbase("decay",5,0.98); milk_addbase("gamma",5,1.0); milk_addbase("wave_mode",9,0.0);
    milk_addbase("wave_r",6,0.5); milk_addbase("wave_g",6,0.5); milk_addbase("wave_b",6,0.5);
    milk_addbase("wave_a",6,1.0); milk_addbase("wave_x",6,0.5); milk_addbase("wave_y",6,0.5);
    milk_addbase("zoomexp",7,1.0); milk_addbase("warpanim",8,1.0); milk_addbase("warpscale",9,1.0);
    milk_addbase("echo_zoom",9,1.0); milk_addbase("echo_alpha",10,0.0);
    milk_addbase("echo_orient",11,0.0); milk_addbase("additivewaves",13,0.0);
}
static void milk_append(char *buf,int *len,const char *vs,const char *ve){ int n=*len; while(vs<ve&&n<MILK_SRCMAX-2)buf[n++]=*vs++; buf[n++]='\n'; buf[n]=0; *len=n; }
static void milk_load(const char *text)
{
    const char *p = text;
    int pfL=0, ppL=0, pfiL=0, s;
    eel_reset(); milk_cache_vix(); milk_set_defaults();
    milk_pf[0]=milk_pp[0]=milk_pfi[0]=0;
    while (*p)
    {
        const char *ls=p, *le, *k, *eq, *ke, *vs; int kl;
        while (*p && *p!='\n') p++;
        le=p; if(*p)p++;
        k=ls; while(k<le&&(*k==' '||*k=='\t'))k++;
        if (k>=le||*k=='['||*k==';'||(*k=='/'&&k+1<le&&k[1]=='/')) continue;
        eq=k; while(eq<le&&*eq!='=')eq++;
        if (eq>=le) continue;
        ke=eq; while(ke>k&&(ke[-1]==' '||ke[-1]=='\t'))ke--;
        kl=(int)(ke-k); vs=eq+1;
        if (kl<=0) continue;
        if (milk_prefix(k,kl,"per_frame_init_")) milk_append(milk_pfi,&pfiL,vs,le);
        else if (milk_prefix(k,kl,"per_frame_"))  milk_append(milk_pf,&pfL,vs,le);
        else if (milk_prefix(k,kl,"per_pixel_"))  milk_append(milk_pp,&ppL,vs,le);
        else if (milk_prefix(k,kl,"warp_")||milk_prefix(k,kl,"comp_")||milk_prefix(k,kl,"sampler_")) { /* skip HLSL */ }
        else
        {
            double v=milk_atof(vs,le);
            const char *cn=k; int cl=kl;
            if      (milk_keyeq(k,kl,"fdecay"))                {cn="decay";cl=5;}
            else if (milk_keyeq(k,kl,"fgammaadj"))             {cn="gamma";cl=5;}
            else if (milk_keyeq(k,kl,"nwavemode"))             {cn="wave_mode";cl=9;}
            else if (milk_keyeq(k,kl,"fvideoechozoom"))        {cn="echo_zoom";cl=9;}
            else if (milk_keyeq(k,kl,"fvideoechoalpha"))       {cn="echo_alpha";cl=10;}
            else if (milk_keyeq(k,kl,"nvideoechoorientation")) {cn="echo_orient";cl=11;}
            else if (milk_keyeq(k,kl,"bdarkencenter"))         {cn="darken_center";cl=13;}
            else if (milk_keyeq(k,kl,"fwavealpha"))            {cn="wave_a";cl=6;}
            else if (milk_keyeq(k,kl,"fwarpanimspeed"))        {cn="warpanim";cl=8;}
            else if (milk_keyeq(k,kl,"fwarpscale"))            {cn="warpscale";cl=9;}
            else if (milk_keyeq(k,kl,"fzoomexponent"))         {cn="zoomexp";cl=7;}
            else if (milk_keyeq(k,kl,"badditivewaves"))        {cn="additivewaves";cl=13;}
            milk_addbase(cn,cl,v);
        }
    }
    milk_apply_base();
    if (pfiL) { eel_compile(milk_pfi,&s); eel_run(s, eel_codef-s); }
    milk_pf_n = eel_compile(milk_pf,&milk_pf_s);
    milk_pp_n = eel_compile(milk_pp,&milk_pp_s);
}
static void milk_run_frame(void){ milk_apply_base(); eel_run(milk_pf_s, milk_pf_n); }

/* ========================== RENDERER ================================= */

#define VIS_SCALE 2
#define IW (LCD_WIDTH  / VIS_SCALE)
#define IH (LCD_HEIGHT / VIS_SCALE)
#define FFT_N     512
#define FFT_BANDS 24
#define PRESET_AUTO_TICKS (20 * HZ)
#define MAX_PRESETS 128
#define FILEBUF_MAX 65536

#define SL_SIN(i) (slut[(i) & 511])
#define SL_COS(i) (slut[((i) + 128) & 511])

static fb_data        outfb[LCD_WIDTH * LCD_HEIGHT];
static unsigned short bufA[IW * IH];
static unsigned short bufB[IW * IH];
static unsigned short xfade_buf[IW * IH];  /* frozen old frame for crossfade */
static unsigned short pres[IW * IH];       /* blended present buffer         */
#define XFADE_DUR (HZ*3/2)                 /* preset crossfade length        */
static short slut[512];                       /* render Q14 sine LUT */
static float rwin[FFT_N];                     /* Hann window */
static float rfft_re[FFT_N], rfft_im[FFT_N];
static float wave_pcm[FFT_N];                 /* raw mono, ~[-1,1] for scope */
static int   band_lo[FFT_BANDS], band_hi[FFT_BANDS], band_smooth[FFT_BANDS];
static float bandf[FFT_BANDS], ragc = 1.0f;
static float avg_bass = 1, avg_mid = 1, avg_treb = 1;
/* warp mesh: per-vertex source coordinates (Q16 source-pixel units) */
#define MX 20
#define MY 25
#define MESH_VERTS ((MX+1)*(MY+1))
static long vsu[MESH_VERTS], vsv[MESH_VERTS];
static const unsigned char bayer[16] =
{ 0,128,32,160, 192,64,224,96, 48,176,16,144, 240,112,208,80 };

static char filebuf[FILEBUF_MAX];
static char preset_names[MAX_PRESETS][64];
static int  preset_count, preset_cur;
static char cur_name[40];

static const char DEFAULT_PRESET[] =
    "nWaveMode=0\n" "fDecay=0.96\n"
    "zoom=1.012\n" "rot=0.0\n" "cx=0.5\n" "cy=0.5\n"
    "wave_r=0.7\n" "wave_g=0.5\n" "wave_b=0.9\n"
    "per_frame_1=wave_r = 0.5 + 0.45*sin(time*1.3);\n"
    "per_frame_2=wave_g = 0.5 + 0.45*sin(time*1.7 + 2);\n"
    "per_frame_3=wave_b = 0.5 + 0.45*sin(time*1.1 + 4);\n"
    "per_frame_4=zoom = 1.01 + 0.03*bass;\n"
    "per_frame_5=rot = 0.02*sin(time*0.3) + 0.02*treb;\n"
    "per_frame_6=cx = 0.5 + 0.1*sin(time*0.33);\n"
    "per_frame_7=cy = 0.5 + 0.1*cos(time*0.41);\n";

static inline int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }

static inline unsigned short decay_dith(unsigned short c, int dith, int decay)
{
    int r=(c>>11)&0x1F, g=(c>>5)&0x3F, b=c&0x1F;
    r=(r*decay+dith)>>8; g=(g*decay+dith)>>8; b=(b*decay+dith)>>8;
    if(r>31)r=31;
    if(g>63)g=63;
    if(b>31)b=31;
    return (unsigned short)((r<<11)|(g<<5)|b);
}
static inline void set_px(unsigned short *buf,int x,int y,fb_data col){ if((unsigned)x<(unsigned)IW&&(unsigned)y<(unsigned)IH) buf[y*IW+x]=col; }

static void build_tables(void)
{
    int i, b;
    const double dc=0.9999247018391445, ds=0.012271538285719925;
    double c=1.0, s=0.0;
    for (i=0;i<512;i++){ double nc,ns; slut[i]=(short)(s*16384.0); nc=c*dc-s*ds; ns=s*dc+c*ds; c=nc; s=ns; }
    for (i=0;i<FFT_N;i++) rwin[i] = 0.5f - 0.5f*(SL_COS(i*(512/FFT_N))*(1.0f/16384.0f));
    { float e=1.0f; int prev=1;
      for (b=0;b<FFT_BANDS;b++){ int hi; e*=1.2599210498948732f; hi=(int)(e+0.5f); if(hi<=prev)hi=prev+1; if(hi>FFT_N/2)hi=FFT_N/2; band_lo[b]=prev; band_hi[b]=hi; prev=hi; } }
}

static void fft_run(void)
{
    int i,j,k,len;
    for (i=1,j=0;i<FFT_N;i++){ int bit=FFT_N>>1; for(;j&bit;bit>>=1)j^=bit; j^=bit; if(i<j){float t;t=rfft_re[i];rfft_re[i]=rfft_re[j];rfft_re[j]=t;t=rfft_im[i];rfft_im[i]=rfft_im[j];rfft_im[j]=t;} }
    for (len=2;len<=FFT_N;len<<=1){
        int half=len>>1, step=FFT_N/len;
        for (i=0;i<FFT_N;i+=len)
            for (k=0,j=0;k<half;k++,j+=step){
                float wr=SL_COS(j)*(1.0f/16384.0f), wi=-SL_SIN(j)*(1.0f/16384.0f);
                int a=i+k, bb=i+k+half;
                float vr=rfft_re[bb]*wr-rfft_im[bb]*wi, vi=rfft_re[bb]*wi+rfft_im[bb]*wr;
                rfft_re[bb]=rfft_re[a]-vr; rfft_im[bb]=rfft_im[a]-vi; rfft_re[a]+=vr; rfft_im[a]+=vi;
            }
    }
}

/* grab PCM, FFT, bands, set bass/mid/treb EEL vars. Returns audio present. */
static bool process_audio(void)
{
    int count, i, b;
    const int16_t *v = rb->mixer_channel_get_buffer(PCM_MIXER_CHAN_PLAYBACK, &count);
    float fmax = 1e-6f, bs, md, tr;

    if (!v || count < FFT_N)
    {
        for (b=0;b<FFT_BANDS;b++) band_smooth[b]=(band_smooth[b]*3)>>2;
        for (i=0;i<FFT_N;i++) wave_pcm[i]*=0.9f;
        eel_vval[VX_bass]*=0.92; eel_vval[VX_mid]*=0.92; eel_vval[VX_treb]*=0.92;
        return false;
    }
    for (i=0;i<FFT_N;i++){ int l=*v++, r=*v++; int mono=(l+r)>>1; wave_pcm[i]=mono*(1.0f/32768.0f); rfft_re[i]=mono*rwin[i]; rfft_im[i]=0.0f; }
    rb->yield();
    fft_run();
    for (b=0;b<FFT_BANDS;b++){
        float sum=0; for(i=band_lo[b];i<band_hi[b];i++){ float re=rfft_re[i],im=rfft_im[i]; float ar=re<0?-re:re, ai=im<0?-im:im; float mx=ar>ai?ar:ai, mn=ar>ai?ai:ar; sum+=mx+0.375f*mn; }
        bandf[b]=sum; if(sum>fmax)fmax=sum;
    }
    ragc += (fmax-ragc)*0.05f; if(ragc<1e-6f)ragc=1e-6f;
    for (b=0;b<FFT_BANDS;b++){ int nv=(int)(bandf[b]*256.0f/ragc); nv=clampi(nv,0,256); if(nv>band_smooth[b])band_smooth[b]=nv; else band_smooth[b]=(band_smooth[b]*3+nv)>>2; }

    /* bass/mid/treb normalised around ~1.0 (Milkdrop convention) */
    bs=md=tr=0;
    for (b=0;b<4;b++)  bs+=band_smooth[b];
    for (b=8;b<16;b++) md+=band_smooth[b];
    for (b=18;b<FFT_BANDS;b++) tr+=band_smooth[b];
    bs/=4; md/=8; tr/=6;
    avg_bass+=(bs-avg_bass)*0.02f; avg_mid+=(md-avg_mid)*0.02f; avg_treb+=(tr-avg_treb)*0.02f;
    if(avg_bass<1)avg_bass=1;
    if(avg_mid<1)avg_mid=1;
    if(avg_treb<1)avg_treb=1;
    eel_vval[VX_bass]=bs/avg_bass; eel_vval[VX_mid]=md/avg_mid; eel_vval[VX_treb]=tr/avg_treb;
    eel_vval[VX_bass_att]+=(eel_vval[VX_bass]-eel_vval[VX_bass_att])*0.2;
    eel_vval[VX_mid_att]+=(eel_vval[VX_mid]-eel_vval[VX_mid_att])*0.2;
    eel_vval[VX_treb_att]+=(eel_vval[VX_treb]-eel_vval[VX_treb_att])*0.2;
    return true;
}

/* build the fixed-point affine warp source tables from per_frame motion */
/* Evaluate per_pixel on the warp mesh: each vertex gets x/y/rad/ang, runs the
 * preset's per_pixel equations (starting from the per_frame motion values), and
 * its source sample coordinate is computed with the same warp formula as
 * Milkdrop -- affine zoom/rot/stretch/translate plus the time-animated
 * sinusoidal warp term. Returns the frame decay. */
static int mesh_setup(void)
{
    double pz=eel_vval[VX_zoom], pr=eel_vval[VX_rot], pw=eel_vval[VX_warp];
    double pcx=eel_vval[VX_cx], pcy=eel_vval[VX_cy], pdx=eel_vval[VX_dx], pdy=eel_vval[VX_dy];
    double psx=eel_vval[VX_sx], psy=eel_vval[VX_sy], pze=eel_vval[VX_zoomexp];
    double wscl=eel_vval[VX_warpscale];
    double wsi=1.0/((wscl>0.01||wscl<-0.01)?wscl:1.0);
    double wt=eel_vval[VX_time]*eel_vval[VX_warpanim];
    double f0=11.68+4.0*m_cos(wt*1.413+10.0), f1=8.77+3.0*m_cos(wt*1.113+7.0);
    double f2=10.54+3.0*m_cos(wt*1.233+3.0),  f3=11.49+4.0*m_cos(wt*0.933+5.0);
    int decay=clampi((int)(eel_vval[VX_decay]*256.0),0,256);
    int gi, gj, idx=0;
    for (gj=0; gj<=MY; gj++){
        double y=(double)gj/MY;
        for (gi=0; gi<=MX; gi++, idx++){
            double x=(double)gi/MX, cxd=2*x-1, cyd=2*y-1;
            double rad=m_sqrt(cxd*cxd+cyd*cyd)*0.70710678, ang=m_atan2(cyd,cxd);
            double zoomv, rotv, warpv, cxv, cyv, dxv, dyv, sxv, syv, zexp;
            double zoom2, iz, fx, fy, cr, sr, u, vv2;
            eel_vval[VX_x]=x; eel_vval[VX_y]=y; eel_vval[VX_rad]=rad; eel_vval[VX_ang]=ang;
            eel_vval[VX_zoom]=pz; eel_vval[VX_rot]=pr; eel_vval[VX_warp]=pw;
            eel_vval[VX_cx]=pcx; eel_vval[VX_cy]=pcy; eel_vval[VX_dx]=pdx; eel_vval[VX_dy]=pdy;
            eel_vval[VX_sx]=psx; eel_vval[VX_sy]=psy; eel_vval[VX_zoomexp]=pze;
            if (milk_pp_n) eel_run(milk_pp_s, milk_pp_n);
            zoomv=eel_vval[VX_zoom]; rotv=eel_vval[VX_rot]; warpv=eel_vval[VX_warp];
            cxv=eel_vval[VX_cx]; cyv=eel_vval[VX_cy]; dxv=eel_vval[VX_dx]; dyv=eel_vval[VX_dy];
            sxv=eel_vval[VX_sx]; syv=eel_vval[VX_sy]; zexp=eel_vval[VX_zoomexp];
            if (zoomv<0.01&&zoomv>-0.01) zoomv=1.0;
            if (sxv<0.01&&sxv>-0.01) sxv=1.0;
            if (syv<0.01&&syv>-0.01) syv=1.0;
            if (zexp>0.999 && zexp<1.001) zoom2=zoomv;
            else { zoom2=m_pow(zoomv, m_pow(zexp, rad*2.0-1.0)); if(zoom2<0.01&&zoom2>-0.01)zoom2=1.0; }
            iz=1.0/zoom2;
            fx=(x-cxv)*iz/sxv; fy=(y-cyv)*iz/syv;
            cr=m_cos(rotv); sr=m_sin(rotv);
            u   = fx*cr - fy*sr + cxv - dxv;
            vv2 = fx*sr + fy*cr + cyv - dyv;
            u   += warpv*0.0035*( m_sin(wt*0.333 + wsi*(x*f0 - y*f3)) + m_cos(wt*0.753 - wsi*(x*f1 - y*f2)) );
            vv2 += warpv*0.0035*( m_cos(wt*0.375 - wsi*(x*f2 + y*f1)) + m_sin(wt*0.825 + wsi*(x*f3 + y*f0)) );
            if (u >  4.0) u =  4.0;
            if (u < -4.0) u = -4.0;
            if (vv2 >  4.0) vv2 =  4.0;
            if (vv2 < -4.0) vv2 = -4.0;
            vsu[idx]=(long)(u*IW*65536.0);
            vsv[idx]=(long)(vv2*IH*65536.0);
        }
    }
    eel_vval[VX_zoom]=pz; eel_vval[VX_rot]=pr; eel_vval[VX_cx]=pcx; eel_vval[VX_cy]=pcy;
    return decay;
}
/* Warp the feedback buffer by the mesh: each cell's pixels sample the previous
 * frame at the bilinearly-interpolated source coordinate (marched incrementally
 * so the hot loop is just two adds per pixel), then decay. */
static void mesh_warp(const unsigned short *prev, unsigned short *cur, int decay)
{
    int gi, gj;
    for (gj=0; gj<MY; gj++){
        int y0=gj*IH/MY, y1=(gj+1)*IH/MY, ch=y1-y0;
        int rowbase=gj*(MX+1);
        for (gi=0; gi<MX; gi++){
            int x0=gi*IW/MX, x1=(gi+1)*IW/MX, cw=x1-x0;
            int tl=rowbase+gi, tr=tl+1, bl=tl+(MX+1), br=bl+1;
            long u00=vsu[tl], v00=vsv[tl], u10=vsu[tr], v10=vsv[tr];
            long u01=vsu[bl], v01=vsv[bl], u11=vsu[br], v11=vsv[br];
            long dul, dvl, dur, dvr, ul, vl, du, dv, ddu, ddv;
            int px, py;
            if (cw<1) cw=1;
            if (ch<1) ch=1;
            dul=(u01-u00)/ch; dvl=(v01-v00)/ch;
            dur=(u11-u10)/ch; dvr=(v11-v10)/ch;
            du=(u10-u00)/cw;  dv=(v10-v00)/cw;
            ddu=(dur-dul)/cw; ddv=(dvr-dvl)/cw;
            ul=u00; vl=v00;
            for (py=y0; py<y1; py++){
                long uu=ul, vv=vl;
                unsigned short *drow=cur+py*IW;
                int row=(py&3)*4;
                for (px=x0; px<x1; px++){
                    int sx=(int)(uu>>16), sy=(int)(vv>>16);
                    sx=clampi(sx,0,IW-1); sy=clampi(sy,0,IH-1);
                    drow[px]=decay_dith(prev[sy*IW+sx], bayer[row+(px&3)], decay);
                    uu+=du; vv+=dv;
                }
                ul+=dul; vl+=dvl; du+=ddu; dv+=ddv;
            }
        }
    }
}

static inline unsigned short sat_add565(unsigned short a, unsigned short c)
{
    int r=((a>>11)&31)+((c>>11)&31), g=((a>>5)&63)+((c>>5)&63), b=(a&31)+(c&31);
    if(r>31)r=31;
    if(g>63)g=63;
    if(b>31)b=31;
    return (unsigned short)((r<<11)|(g<<5)|b);
}
static inline void wave_px(unsigned short *buf, int x, int y, fb_data col, int add)
{
    if ((unsigned)x>=(unsigned)IW || (unsigned)y>=(unsigned)IH) return;
    if (add) buf[y*IW+x]=sat_add565(buf[y*IW+x], col);
    else     buf[y*IW+x]=col;
}
/* nWaveMode waveforms; bAdditiveWaves -> additive blend (glow) */
static void draw_wave(unsigned short *cur)
{
    int wm=(int)(eel_vval[VX_wmode]+0.5);
    int add=(eel_vval[VX_additive]>0.5);
    int r=clampi((int)(eel_vval[VX_wr]*255),0,255);
    int g=clampi((int)(eel_vval[VX_wg]*255),0,255);
    int b=clampi((int)(eel_vval[VX_wb]*255),0,255);
    fb_data col;
    /* Many MilkDrop-1 presets leave wave_r/g/b at 0 and colour via q1/q2/q3
     * (for custom waves/shapes we don't render). Fall back to those so such
     * presets aren't invisible. */
    if (r+g+b < 6){
        r=clampi((int)(eel_vval[VX_q1]*255),0,255);
        g=clampi((int)(eel_vval[VX_q2]*255),0,255);
        b=clampi((int)(eel_vval[VX_q3]*255),0,255);
    }
    col=LCD_RGBPACK(r,g,b);
    int cxp=clampi((int)(eel_vval[VX_wx]*IW),0,IW-1);
    int cyp=clampi((int)(eel_vval[VX_wy]*IH),0,IH-1);
    int i;
    switch (wm)
    {
    case 1:   /* X-Y oscilloscope (Lissajous ribbon) */
        for (i=0;i<FFT_N-2;i+=2){
            int px=cxp+(int)(wave_pcm[i]*(IW/3));
            int py=cyp+(int)(wave_pcm[i+1]*(IH/3));
            wave_px(cur,px,py,col,add); wave_px(cur,px+1,py,col,add);
        }
        break;
    case 3:   /* vertical scope */
    {
        int amp=IW/4;
        for (i=0;i<IH;i++){
            int si=i*FFT_N/IH, xx=cxp+(int)(wave_pcm[si]*amp);
            wave_px(cur,xx,i,col,add); wave_px(cur,xx+1,i,col,add);
        }
        break;
    }
    case 4:   /* dual horizontal scopes */
    {
        int amp=IH/6;
        for (i=0;i<IW;i++){
            int si=i*FFT_N/IW, d=(int)(wave_pcm[si]*amp);
            wave_px(cur,i,cyp-IH/8+d,col,add);
            wave_px(cur,i,cyp+IH/8-d,col,add);
        }
        break;
    }
    case 2:   /* centered horizontal */
    {
        int amp=IH/4, prevy=cyp;
        for (i=0;i<IW;i++){
            int si=i*FFT_N/IW, yy=cyp+(int)(wave_pcm[si]*amp);
            int y0=yy<prevy?yy:prevy, y1=yy<prevy?prevy:yy, j;
            for (j=y0;j<=y1;j++) wave_px(cur,i,j,col,add);
            prevy=yy;
        }
        break;
    }
    default:  /* 0 (and unimplemented modes): circle */
    {
        int base_r=IH/6, amp=IH/5;
        for (i=0;i<FFT_N;i+=2){
            int ai=i*512/FFT_N, rr=base_r+(int)(wave_pcm[i]*amp);
            int px=cxp+((SL_COS(ai)*rr)>>14), py=cyp+((SL_SIN(ai)*rr)>>14);
            wave_px(cur,px,py,col,add); wave_px(cur,px+1,py,col,add);
        }
        break;
    }
    }
}

/* video echo: blend the feedback toward a zoomed/flipped copy of the previous
 * frame (echo_alpha 0 -> off). Gives the layered tunnel/kaleidoscope look. */
static void echo_blend(unsigned short *cur, const unsigned short *prev)
{
    double ea=eel_vval[VX_echo_alpha], ez=eel_vval[VX_echo_zoom];
    int orient=(int)(eel_vval[VX_echo_orient]+0.5);
    int alpha, izq, cx=IW/2, cy=IH/2, x, y;
    if (ea < 0.01) return;
    if (ez < 0.05 && ez > -0.05) ez=1.0;
    alpha=clampi((int)(ea*256.0),0,256);
    izq=(int)(65536.0/ez);
    for (y=0;y<IH;y++){
        int sy=cy+(((y-cy)*izq)>>16);
        if (orient & 2) sy=IH-1-sy;
        if ((unsigned)sy>=(unsigned)IH) continue;
        for (x=0;x<IW;x++){
            int sx=cx+(((x-cx)*izq)>>16);
            unsigned short e, d;
            int dr,dg,db,er,eg,eb;
            if (orient & 1) sx=IW-1-sx;
            if ((unsigned)sx>=(unsigned)IW) continue;
            e=prev[sy*IW+sx]; d=cur[y*IW+x];
            dr=(d>>11)&31; dg=(d>>5)&63; db=d&31;
            er=(e>>11)&31; eg=(e>>5)&63; eb=e&31;
            dr+=((er-dr)*alpha)>>8; dg+=((eg-dg)*alpha)>>8; db+=((eb-db)*alpha)>>8;
            cur[y*IW+x]=(unsigned short)((dr<<11)|(dg<<5)|db);
        }
    }
}

/* exact 2x bilinear upscale via the no-unpack RGB565 half-blend.
 * Each source 2x2 quad fills a 2x2 output block: corner = exact, edges =
 * half-averages, centre = quarter-average. Same result as general bilinear
 * for a 2x zoom, but ~10x cheaper (relies on VIS_SCALE == 2). */
#define AVG2(a,b) (unsigned short)(((a)&(b)) + ((((a)^(b)) & 0xF7DE) >> 1))
static void upscale(const unsigned short *src)
{
    int i, j;
    for (j=0;j<IH;j++){
        const unsigned short *s0=src+j*IW;
        const unsigned short *s1=src+((j<IH-1)?(j+1):j)*IW;
        fb_data *o0=outfb+(2*j)*LCD_WIDTH;
        fb_data *o1=o0+LCD_WIDTH;
        for (i=0;i<IW;i++){
            int i1=(i<IW-1)?i+1:i;
            unsigned short a=s0[i], b=s0[i1], c=s1[i], d=s1[i1];
            unsigned short ab=AVG2(a,b);
            o0[2*i]=a;            o0[2*i+1]=ab;
            o1[2*i]=AVG2(a,c);    o1[2*i+1]=AVG2(ab, AVG2(c,d));
        }
    }
}

static void overlay(int fps, const char *name, bool show_name)
{
    char buf[48];
    rb->lcd_setfont(FONT_SYSFIXED);
    rb->lcd_set_drawmode(DRMODE_FG);
    rb->snprintf(buf, sizeof buf, "%d fps  %s", fps, name);
    rb->lcd_set_foreground(LCD_BLACK); rb->lcd_putsxy(3,3,(unsigned char *)buf);
    rb->lcd_set_foreground(LCD_WHITE); rb->lcd_putsxy(2,2,(unsigned char *)buf);
    if (show_name){
        int w,h; rb->lcd_getstringsize((const unsigned char *)name,&w,&h);
        rb->lcd_set_foreground(LCD_BLACK); rb->lcd_putsxy((LCD_WIDTH-w)/2+1, LCD_HEIGHT/6+1, (const unsigned char *)name);
        rb->lcd_set_foreground(LCD_WHITE); rb->lcd_putsxy((LCD_WIDTH-w)/2, LCD_HEIGHT/6, (const unsigned char *)name);
    }
    rb->lcd_set_drawmode(DRMODE_SOLID);
}

/* ---- preset file management ------------------------------------------ */
#define PRESET_DIR "/.rockbox/milkdrop"

static void scan_presets(void)
{
    struct dirent *e;
    DIR *d = rb->opendir(PRESET_DIR);
    preset_count = 0;
    if (!d) return;
    while ((e = rb->readdir(d)) && preset_count < MAX_PRESETS){
        const char *n=e->d_name; int len=rb->strlen(n);
        if (len>5 && rb->strcasecmp(n+len-5, ".milk")==0){
            rb->strlcpy(preset_names[preset_count], n, sizeof(preset_names[0]));
            preset_count++;
        }
    }
    rb->closedir(d);
}
static void name_for_display(const char *fname)
{
    int i=0; while (fname[i] && fname[i]!='.' && i<(int)sizeof(cur_name)-1){ cur_name[i]=fname[i]; i++; }
    cur_name[i]=0;
}
static void load_cur_preset(void)
{
    if (preset_count==0){ milk_load(DEFAULT_PRESET); rb->strcpy(cur_name,"default"); return; }
    {
        char path[96]; int fd, n=0;
        rb->snprintf(path,sizeof path,"%s/%s",PRESET_DIR,preset_names[preset_cur]);
        fd=rb->open(path,O_RDONLY);
        if (fd<0){ milk_load(DEFAULT_PRESET); rb->strcpy(cur_name,"default"); return; }
        n=rb->read(fd,filebuf,FILEBUF_MAX-1);
        rb->close(fd);
        if (n<=0){ milk_load(DEFAULT_PRESET); rb->strcpy(cur_name,"default"); return; }
        filebuf[n]=0;
        milk_load(filebuf);
        name_for_display(preset_names[preset_cur]);
    }
}

/* ============================ ENTRY ================================== */

enum plugin_status plugin_start(const void *parameter)
{
    unsigned short *cur=bufA, *prev=bufB, *tmp;
    enum plugin_status status=PLUGIN_OK;
    long frames=0, fps=0, fps_tick, preset_tick, name_until, start_tick;
    long framecount=0, xfade_start=0;
    bool xfade_on=false;
    (void)parameter;

    rb->button_clear_queue();
    backlight_force_on();
    build_tables();
    rb->memset(bufA,0,sizeof(bufA));
    rb->memset(bufB,0,sizeof(bufB));
    rb->lcd_set_backdrop(NULL);

    scan_presets();
    preset_cur=0;
    load_cur_preset();

    start_tick = fps_tick = preset_tick = *rb->current_tick;
    name_until = *rb->current_tick + 2*HZ;

    while (1)
    {
        long now=*rb->current_tick;
        int decay, button;

        button=rb->button_get(false);
        if (button!=BUTTON_NONE){
            if (rb->default_event_handler(button)==SYS_USB_CONNECTED){ status=PLUGIN_USB_CONNECTED; break; }
            if (button & BUTTON_REPEAT) break;
            if (!(button & BUTTON_REL)){
                rb->memcpy(xfade_buf, prev, sizeof(xfade_buf));
                xfade_start=now; xfade_on=true;
                preset_cur=(preset_count>0)?(preset_cur+1)%preset_count:0;
                load_cur_preset(); preset_tick=now; name_until=now+2*HZ; continue;
            }
        }
        if (now-preset_tick >= PRESET_AUTO_TICKS && preset_count>1){
            rb->memcpy(xfade_buf, prev, sizeof(xfade_buf));
            xfade_start=now; xfade_on=true;
            preset_cur=(preset_cur+1)%preset_count; load_cur_preset(); preset_tick=now; name_until=now+2*HZ;
        }

        process_audio();

        eel_vval[VX_time]=(now-start_tick)/(double)HZ;
        eel_vval[VX_frame]=(double)framecount;
        eel_vval[VX_fps]=(double)(fps>0?fps:60);
        eel_vval[VX_progress]=(now-preset_tick)/(double)PRESET_AUTO_TICKS;
        milk_run_frame();

        decay = mesh_setup();
        mesh_warp(prev,cur,decay);
        echo_blend(cur, prev);
        draw_wave(cur);

        if (xfade_on){
            long el=now-xfade_start;
            if (el>=XFADE_DUR){ xfade_on=false; upscale(cur); }
            else {
                int wold=256-(int)(el*256/XFADE_DUR), n=IW*IH, k;
                for (k=0;k<n;k++){
                    unsigned short a=cur[k], o=xfade_buf[k];
                    int ar=(a>>11)&31, ag=(a>>5)&63, ab=a&31;
                    int orr=(o>>11)&31, og=(o>>5)&63, ob=o&31;
                    int rr=ar+(((orr-ar)*wold)>>8), rg=ag+(((og-ag)*wold)>>8), rb2=ab+(((ob-ab)*wold)>>8);
                    pres[k]=(unsigned short)((rr<<11)|(rg<<5)|rb2);
                }
                upscale(pres);
            }
        } else upscale(cur);
        rb->lcd_bitmap(outfb,0,0,LCD_WIDTH,LCD_HEIGHT);
        overlay((int)fps, cur_name, now<name_until);
        rb->lcd_update();

        tmp=prev; prev=cur; cur=tmp;
        framecount++; frames++;
        if (now-fps_tick>=HZ){ fps=frames*HZ/(now-fps_tick); frames=0; fps_tick=now; }
        rb->yield();
    }

    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->lcd_setfont(FONT_UI);
    backlight_use_settings();
    return status;
}
