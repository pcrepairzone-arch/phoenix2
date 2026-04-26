/*******************************************************************************
*                                                                              *
* Project       : DiscKnight                                                   *
* Filename      : SparceFile.h                                                 *
* Version       : 1.52 (10-Apr-2017)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Checks and fixes E & E+ format filecore discs                *
* Information   : Sparce file support                                          *
*                                                                              *
********************************************************************************
* Change Log:                                                                  *
*                                                                              *
* Ver   Date         Description of change                                     *
* ----  -----------  ---------------------                                     *
* 0.01  13-Jul-2000  Initial revision                                          *
* 0.05  04-Aug-2000  Second alpha - released as read only                      *
* 1.00  08-Oct-2000  First release build                                       *
* 1.15  07-Feb-2001  Function to free cache memory on exit added               *
* 1.28  26-Jun-2001  SparceFile_AppendBlock() made public                      *
* 1.52  10-Apr-2017  Exposing SparceFile_Find                                  *
*                                                                              *
*******************************************************************************/

#ifndef _SparceFile_h
#define _SparceFile_h

#include "oslib/types.h"
#include "oslib/osfind.h"

#include "Bits64.h"

/* --- macros --------------------------------------------------------------- */

/* --- enums ---------------------------------------------------------------- */

#define SparceFile_MAGIC        "SPARCE\x00\x01"
#define SparceFile_MAGIC_LEN    8u

/* --- types ---------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

/* --- functions ------------------------------------------------------------ */

os_error* SparceFile_Create(char *filename);
osbool    SparceFile_Check(os_fw f);
os_error* SparceFile_Find(os_fw f, bits64 addr, bits size, bits *pos);
os_error* SparceFile_Append(char *filename, bits64 addr, bits size, byte *data);
os_error* SparceFile_AppendBlock(os_fw f, bits64 addr, bits size, byte *data);
os_error* SparceFile_Write(os_fw f, bits64 addr, bits size, byte *data);
os_error* SparceFile_Read(os_fw f, bits64 addr, bits size, byte *data);
os_error* SparceFile_ReadNext(os_fw f, bits64 *addr, bits *size, byte **buffer);
void      SparceFile_FreeCache(void);

#endif
/* EOF */
