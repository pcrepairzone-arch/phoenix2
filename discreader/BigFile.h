/*******************************************************************************
*                                                                              *
* Project       : DiscKnight                                                   *
* Filename      : BigFile.h                                                    *
* Version       : 0.01 (04-Dec-2012)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Checks and fixes E & E+ format filecore discs                *
* Information   : Sparce file support                                          *
*                                                                              *
********************************************************************************
* Change Log:                                                                  *
*                                                                              *
* Ver   Date         Description of change                                     *
* ----  -----------  ---------------------                                     *
* 0.01  04-Dec-2012  Initial revision                                          *
*                                                                              *
*******************************************************************************/

#ifndef _BigFile_h
#define _BigFile_h

#include "oslib/types.h"
#include "oslib/osfind.h"

#include "Bits64.h"

/* --- macros --------------------------------------------------------------- */

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

/* --- functions ------------------------------------------------------------ */

os_error* BigFile_Open(char *filename, os_fw *f);
os_error* BigFile_Close(os_fw f);
os_error* BigFile_Read(os_fw f, bits64 addr, bits size, byte *data);
os_error* BigFile_Write(os_fw f, bits64 addr, bits size, byte *data);

#endif
/* EOF */
