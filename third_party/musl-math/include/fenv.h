#ifndef _FENV_H
#define _FENV_H
// Aegis: exceptions and rounding modes are not exposed; the defaults apply.
#define FE_INVALID    1
#define FE_DIVBYZERO  4
#define FE_OVERFLOW   8
#define FE_UNDERFLOW  16
#define FE_INEXACT    32
#define FE_ALL_EXCEPT 63
#define FE_TONEAREST  0
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00
static inline int feclearexcept(int e) { (void)e; return 0; }
static inline int feraiseexcept(int e) { (void)e; return 0; }
static inline int fetestexcept(int e) { (void)e; return 0; }
static inline int fegetround(void) { return FE_TONEAREST; }
static inline int fesetround(int r) { return r == FE_TONEAREST ? 0 : -1; }
#endif
