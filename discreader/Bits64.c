/*******************************************************************************
*                                                                              *
* Project       : DiscKnight                                                   *
* Filename      : bits64.c                                                     *
* Version       : 1.48 (01-May-2007)                                           *
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
* 0.06  31-Aug-2000  Peta byte suffix added to Size64()                        *
* 1.00  08-Oct-2000  First release build                                       *
* 1.34  23-Nov-2001  Sub64 & Sub64_32 carry bit bit fixed                      *
*                    __value_in_regs used for 64bit returns                    *
* 1.38  15-Jan-2002  Print64 excess magnetude fixed to use last suffix         *
* 1.48  01-May-2007  Strings made const to elimiate gcc build warnings         *
*                                                                              *
*******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "Bits64.h"

/* --- literals ------------------------------------------------------------- */

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

/* --- statics -------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

/* --- functions ------------------------------------------------------------ */

/* -------------------------------------------------------------------------- */

#ifndef __stdint_ll

/*
 * Description: Zero initialise 64 bit numbers
 * Parameters : pointer to a 64 bit numbers
 * Returns    : (none)
 */
void Zero64(bits64 *a)
{
    a->low  = 0;
    a->high = 0;
}

/*
 * Description: Add two 64 bit numbers
 * Parameters : two 64 bit numbers
 * Returns    : 64 bit result
 */
_BITS64_IN_REGS bits64 Add64(bits64 a, bits64 b)
{
    bits64 c;
    c.low  = a.low  + b.low;
    c.high = a.high + b.high + (c.low<a.low ? 1:0);
    return c;
}

/*
 * Description: Add a 32 bit number to a 64 bit number
 * Parameters : 64 bit and 32 bit numbers
 * Returns    : 64 bit result
 */
_BITS64_IN_REGS bits64 Add64_32(bits64 a, bits b)
{
    bits64 c;
    c.low  = a.low  + b;
    c.high = a.high + (c.low<a.low ? 1:0);
    return c;
}

/*
 * Description: Increments a 64 bit number by a 32 bit number
 * Parameters : poibter to 64 bit number, and 32 bit number
 * Returns    : (none)
 */
void Inc64(bits64 *a, bits b)
{
    bits c   = a->low + b;
    a->high += (c<a->low ? 1:0);
    a->low   = c;
}

/*
 * Description: Subtract two 64 bit numbers
 * Parameters : two 64 bit numbers
 * Returns    : 64 bit result
 */
_BITS64_IN_REGS bits64 Sub64(bits64 a, bits64 b)
{
    bits64 c;
    c.low  = a.low  - b.low;
    c.high = a.high - b.high - ((a.low<b.low) ? 1:0);
    return c;
}

/*
 * Description: Subtract a 32 bit number from a 64 bit number
 * Parameters : 64 bit and 32 bit numbers
 * Returns    : 64 bit result
 */
// cppcheck-suppress unusedFunction
_BITS64_IN_REGS bits64 Sub64_32(bits64 a, bits b)
{
    bits64 c;
    c.low  = a.low  - b;
    c.high = a.high - ((a.low<b) ? 1:0);
    return c;
}

#ifndef BITS64_NOMACROS

/*
 * Description: Decrements a 64 bit number by a 32 bit number
 * Parameters : poibter to 64 bit number, and 32 bit number
 * Returns    : (none)
 */
void Dec64(bits64 *a, bits b)
{
    a->high -= (a->low<b ? 1:0);
    a->low   = a->low-b;
}

/*
 * Description: Compares two 64 bit numbers
 * Parameters : two 64 bit numbers
 * Returns    : -1 if a<b, 0 if same, 1 if a>b
 */
int Cmp64(bits64 a, bits64 b)
{
    if(a.high == b.high)
    {
        if(a.low == b.low)
            return 0;
        else
            return a.low < b.low ? -1 : 1;
    }
    else
    {
        return a.high < b.high ? -1 : 1;
    }
}
#endif /* !BITS64_NOMACROS */
#endif /* __stdint_ll */

/*
 * Description: Format a 64 bit hex size
 * Parameters : 64 bit value
 * Returns    : string
 */
char* Print64(bits64 val)
{
    static char string[17];
#ifdef __stdint_ll
    sprintf(string, "%016llX", val);
#else
    sprintf(string, "%08X%08X", val.high, val.low);
#endif
    return string;
}

/*
 * Description: Convert a 32 bit value to a file size string
 *            : Wrapper for Size64
 * Parameters : 32 bit size
 * Returns    : string
 */
char* Size32(bits size)
{
#ifdef __stdint_ll
    return Size64(size);
#else
    bits64 size64;
    size64.high = 0;
    size64.low  = size;
    return Size64(size64);
#endif
}

/*
 * Description: Convert a 64 bit value to a file size string
 * Parameters : 64 bit size
 * Returns    : string
 */
char *Size64(bits64 size)
{
    static char string[20];

#ifdef __stdint_ll
    if(size==0)
#else
    if(size.high==0 && size.low==0)
#endif
    {
        strcpy(string, "0 ");
    }
    else
    {
        static const char *suf     = " KMGTPE";
        static       int   suf_max = 6;
#ifdef __stdint_ll
        double       dsize = (double)size;
#else
        double       dsize = (double)size.low + (double)size.high*pow(2,32);
#endif
        int         mag   = (int)(log(dsize)/log(2.0))/10;
        double      val   = dsize / pow(2, (double)(mag*10));

        if(mag > suf_max)
        {
            mag = suf_max+1;
            val = 0;
        }

        if(val<4.0 && mag>0)
            val = dsize / pow(2, (double)(--mag*10));

        sprintf(string, val<100.0 ? "%.1lf%c" : "%.0lf%c", val, suf[mag]);
    }

    return string;
}

/*
 * Description: produce percentage string from two 64bit numbers
 * Parameters : numerator, denominator
 * Returns    : string
 */
char* Percent64(bits64 num, bits64 den)
{
    static char string[20];
#ifdef __stdint_ll
    double dnum = (double)num;
    double dden = (double)den;
#else
    double dnum = (double)num.low + (double)num.high*pow(2,32);
    double dden = (double)den.low + (double)den.high*pow(2,32);
#endif

    if(dden==0)
        strcpy(string, "0.0%");
    else
        sprintf(string, "%.1lf%%", 100.0*dnum/dden);
    return string;
}

/* EOF                                                                                            */
