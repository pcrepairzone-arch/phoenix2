/*******************************************************************************
*                                                                              *
* Project       : DiscKnight                                                   *
* Filename      : bits64.h                                                     *
* Version       : 1.34 (24-Nov-2001)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Checks and fixes E & E+ format filecore discs                *
* Information   : 64 bit arthimetic support                                    *
*                                                                              *
********************************************************************************
* Change Log:                                                                  *
*                                                                              *
* Ver   Date         Description of change                                     *
* ----  -----------  ---------------------                                     *
* 0.01  13-Jul-2000  Initial revision                                          *
* 0.05  04-Aug-2000  Second alpha - released as read only                      *
* 1.00  08-Oct-2000  First release build                                       *
* 1.34  24-Nov-2001  __value_in_regs used for 64bit returns                    *
*                                                                              *
*******************************************************************************/

#ifndef _Bits64_h
#define _Bits64_h

//#include <stdint.h>
#include "oslib/types.h"

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

#ifdef __stdint_ll
typedef unsigned long long bits64;
#define _BITS64_IN_REGS
#else
typedef struct                        /* 64 bit unsigned integer class */
{
    bits    low;
    bits    high;
} bits64;

#ifdef __CC_NORCROFT
#define _BITS64_IN_REGS /*__value_in_regs */
#else
#define _BITS64_IN_REGS
#endif

#endif

/* --- macros --------------------------------------------------------------- */

#ifdef __stdint_ll

#define ZERO64(a)       a = 0
#define Add64(a, b)     a + b
#define Add64_32(a, b)  a + b
#define Sub64(a, b)     a - b
#define Sub64_32(a, b)  a - b
#define INC64(a, b)     a += b
#define DEC64(a, b)     a -= b
#define CMP32(a, b)     (a==b ? 0 : (a>b ? 1:-1))
#define CMP64(a, b)     (a==b ? 0 : (a>b ? 1:-1))

#else /* __stdint_ll */
#ifdef BITS64_NOMACROS

#define ZERO64(a)  Zero64(&a)
#define INC64(a,b) Inc64(&a,b)
#define DEC64(a,b) Dec64(&a,b)
#define CMP64(a,b) Cmp64(a,b)

#else /* BITS64_NOMACROS */

/*
 * Description: Zero initialise 64 bit numbers
 * Parameters : pointer to a 64 bit numbers
 * Returns    : (none)
 */
#define ZERO64(a) { a.low  = 0; a.high = 0; }

/*
 * Description: Increments a 64 bit number by a 32 bit number
 * Parameters : poibter to 64 bit number, and 32 bit number
 * Returns    : (none)
 */
#define INC64(a, b) { bits c = a.low + (b);  a.high +=(c<a.low ? 1:0); a.low = c; }

/*
 * Description: Decrements a 64 bit number by a 32 bit number
 * Parameters : poibter to 64 bit number, and 32 bit number
 * Returns    : (none)
 */
#define DEC64(a, b) { a.high -= (a.low<(b) ? 1:0); a.low   = a.low-(b); }


/*
 * Description: Compares two 32 and 64 bit numbers
 * Parameters : two 32 or 64 bit numbers
 * Returns    : -1 if a<b, 0 if same, 1 if a>b
 */
#define CMP32(a, b) (a==b ? 0 : (a>b ? 1:-1))
#define CMP64(a, b) (a.high==b.high ? CMP32(a.low,b.low) : CMP32(a.high, b.high))

#endif /* BITS64_NOMACROS */
#endif /* __stdint_ll */

/* --- globals -------------------------------------------------------------- */

/* --- functions ------------------------------------------------------------ */

#ifndef __stdint_ll
void   Zero64(bits64 *a);
_BITS64_IN_REGS bits64 Add64(bits64 a, bits64 b);
_BITS64_IN_REGS bits64 Add64_32(bits64 a, bits b);
_BITS64_IN_REGS bits64 Sub64(bits64 a, bits64 b);
_BITS64_IN_REGS bits64 Sub64_32(bits64 a, bits b);
void   Inc64(bits64 *a, bits b);
void   Dec64(bits64 *a, bits b);
int    Cmp64(bits64 a, bits64 b);
#endif /* __stdint_ll */

char*  Print64(bits64 val);
char*  Size32(bits size);
char*  Size64(bits64 size);
char*  Percent64(bits64 num, bits64 den);

#endif
/* EOF */
