/*******************************************************************************
*                                                                              *
* Project       : DiscKnight                                                   *
* Filename      : BigFile.c                                                    *
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "oslib/osgbpb.h"
#include "oslib/osargs.h"

#include "BigFile.h"


/* --- literals ------------------------------------------------------------- */

#ifdef __stdint_ll
#define Bit64Ptr(addr) addr;
#else
#ifdef _WIN32
#define Bit64Ptr(addr) ((__int64)addr.high << 32 | (__int64)addr.low)
#else
#define Bit64Ptr(addr) ((off_t)addr.high << 32 | (off_t)addr.low)
#endif
#endif

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

/* --- statics -------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

/* --- externs -------------------------------------------------------------- */

#ifdef _WIN32
extern int _fseeki64(FILE* f, __int64 s, int o);
#endif

/* --- private functions ---------------------------------------------------- */

extern FILE* osfw2FILE(os_fw f);

/* --- public functions ----------------------------------------------------- */

/*
 * Description: opens a big file
 * Parameters : filename, pointer to file handle
 * Returns    : error block or NULL for sucess
 */
os_error* BigFile_Open(char *filename, os_fw *f)
{
    return xosfind_openupw(osfind_NO_PATH,
                           filename,
                           NULL,
                           f);
}

/*
 * Description: closes a big file
 * Parameters : file handle
 * Returns    : error block or NULL for sucess
 */
os_error* BigFile_Close(os_fw f)
{
    return xosfind_closew(f);
}

/*
 * Description: read data from a big file
 * Parameters : file handle, 64 bit address, size, data buffer
 * Returns    : error block or NULL for sucess
 */
os_error* BigFile_Read(os_fw f, bits64 addr, bits size, byte *data)
{
    os_error *err = NULL;

#ifdef __riscos
    int unread;
    err = xosgbpb_read_atw(f, data, size, addr.low, &unread);
#else
#ifdef _WIN32
    _fseeki64(osfw2FILE(f), Bit64Ptr(addr), SEEK_SET);
#else
    fseek(osfw2FILE(f), Bit64Ptr(addr), SEEK_SET);
#endif
    fread(data, 1, size, osfw2FILE(f));
#endif

    return err;
}

/*
 * Description: write a to a big file
 * Parameters : file handle, 64 bit address, size, data buffer
 * Returns    : error block or NULL for sucess
 */
os_error* BigFile_Write(os_fw f, bits64 addr, bits size, byte *data)
{
    os_error *err = NULL;

#ifdef __riscos
    int unwritten;
    err = xosgbpb_write_atw(f, data, size, addr.low, &unwritten);
#else
#ifdef _WIN32
    _fseeki64(osfw2FILE(f), Bit64Ptr(addr), SEEK_SET);
#else
    fseek(osfw2FILE(f), Bit64Ptr(addr), SEEK_SET);
#endif
    fwrite(data, 1, size, osfw2FILE(f));
#endif

    return err;
}

/* EOF                                                                                            */
