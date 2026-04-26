/*******************************************************************************
*                                                                              *
* Filename      : DiscReader.h                                                 *
* Filename      : DiscOps.c                                                    *
* Version       : 1.11 (23-Apr-2007)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Reads E & E+ discs using code from DiscKnight                *
* Information   : Disc operations                                              *
*                                                                              *
********************************************************************************
* Change Log:                                                                  *
*                                                                              *
* Ver   Date         Description of change                                     *
* ----  -----------  ---------------------                                     *
* 1.00  11-Jun-2001  Initial revision based on DiscKnight 1.28 (09-Jun-2001)   *
* 1.10  02-Dec-2001  Updated with changes from DiscKnight 1.34 (23-Nov-2001)   *
* 1.11  23-Apr-2007  Updated with changes from DiscKnight 1.48 (15-Jun-2005)   *
*                                                                              *
*******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "kernel.h"
#include "oslib/osfind.h"
#include "oslib/osfile.h"
#include "oslib/osgbpb.h"
#include "oslib/osargs.h"
#include "oslib/messagetrans.h"

#include "BigFile.h"
#include "SparceFile.h"
#include "DiscReader.h"

#if defined(_MSC_VER) && (_MSC_VER < 1900)
#define snprintf _snprintf
#endif

/* --- literals ------------------------------------------------------------- */

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

/* --- statics -------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

/* --- functions ------------------------------------------------------------ */

osbool DiscOp(int swi_no, _kernel_swi_regs *in_regs, _kernel_swi_regs *out_regs);
osbool SectorDiscOp(bits rw, byte *buffer, bits start, bits size);

/* -------------------------------------------------------------------------- */

/*
 * Description: Get the DiscOP SWI or open the file
 * Parameters : (in globals)
 * Returns    : TRUE if sucessfull
 */
osbool Initialise(void)
{
    if(info.opts.file)
    {
        os_error *err = BigFile_Open(info.file.name, &info.file.h);

        if(err || info.file.h==0)
        {
            Error("Unable to open file '%s'\r\n", info.file.name);
            return FALSE;
        }

        /* check for a space file */
        info.file.sparce = SparceFile_Check(info.file.h);
    }
    else
    {
        char fsname[STRING_LEN];
        char swiname[STRING_LEN];
        int  swinum;
        int  i,j;

        /* try possible SWI name combinations
         * xxxx[FS]_[Sector][Disc]OP[64]
         * then upper case versions of the  fs name
         */
        for(j=0; j<2 && info.disc.byteSWI==0; j++)
        {
            if(j==0)
            {
                strcpy(fsname, info.disc.fs_name);
            }
            else
            {
                int c=0;

                while(info.disc.fs_name[c]!=0)
                {
                    fsname[c] = (char)toupper(info.disc.fs_name[c]);
                    c++;
                }
                fsname[c] = 0;
            }

            for(i=2; i<16; i++)
            {
                snprintf(swiname, STRING_LEN,
                         "%s%s_%s%sOp%s",
                         fsname,
                         (i&1) ? "FS"     : "",
                         (i&4) ? "Sector" : "",
                         (i&2) ? "Disc"   : "",
                         (i&8) ? "64"     : "");

                if(xos_swi_number_from_string(swiname, &swinum)==NULL)
                {
                    if(i&8)
                        info.disc.byte64SWI = swinum;
                    else if(i&4)
                        info.disc.sectorSWI = swinum;
                    else
                        info.disc.byteSWI   = swinum;

                    // Info("Using %s for %s%s SWI\n", swiname, i&4 ? "sector":"byte", i&8 ? "64":"");
                }
            }
        }

        if(info.disc.byteSWI==0)
        {
            Error("Unable to determine SWI call for filing system %s\r\n",
                    info.disc.fs_name);
            return FALSE;
        }
    }

    if(info.opts.extract)
    {
        os_error *err = SparceFile_Create(info.extract_file);

        if(err)
        {
            Error("Unable to open extract file (%s)\r\n", err->errmess);
            return FALSE;
        }
    }

    return TRUE;
}

/*
 * Checks return from DiscOp SWI's
 * Returns true if succeeds
 */
osbool DiscOp(int swi_no, _kernel_swi_regs *in_regs, _kernel_swi_regs *out_regs)
{
    _kernel_oserror *err;
    osbool reading = (in_regs->r[1] & 0x0F)==1;

    /* set ignore timout options to prevent remounting problems */
    in_regs->r[1] |= (1u<<7);

    if((err=_kernel_swi(swi_no, in_regs, out_regs))!=NULL)
    {
        osbool cont = TRUE;
        bits   secaddr;

        if(err->errnum>0x10000 && err->errnum<=0x20000)
        {
            switch(err->errnum & 0xFF)
            {
                case 0xA0: /* FileCore in use */
                case 0xA6: /* FileCore workspace corrupt */
                case 0xAB: /* Bad defect list */
                case 0xAC: /* Bad Drive */
                case 0xD3: /* Drive Empty */
                    cont = FALSE;
                    break;

                case 0xC7: /* Disc error */
                default:
                    cont = TRUE;
                    break;
            }
        }
        else if(err->errnum==17)
        {
            cont = FALSE;
        }

        if(swi_no==info.disc.byte64SWI)
        {
            s_discspec64 *discspec64 = (s_discspec64*)in_regs->r[2];
            secaddr                  = ADDR64SEC(discspec64->address, info.disc_record.log2secsize);
        }
        else
        {
            secaddr = in_regs->r[2] & 0x1FFFFFFF;

            if(swi_no==info.disc.byteSWI)
                secaddr = secaddr >> info.disc_record.log2secsize;
        }

        fprintf(stderr, "\n! Error while %s &%s+&%lX : %s - %s\r\n",
                        reading ? "reading":"writing",
                        SecAddrToString(secaddr),
                        (unsigned long)in_regs->r[4],
                        err->errmess,
                        cont ? "Continuing":"Exiting");

        if(!cont)
            safe_exit(23);

        return FALSE;
    }
    return TRUE;
}

/*
 * Calls DiscOp with correct sector SWI and addressing
 */
osbool SectorDiscOp(bits rw, byte *buffer, bits start, bits size)
{
    _kernel_swi_regs regs;
    bits             swi_no;
    s_discspec64     discspec64;
    bits             secsize = info.disc_record.log2secsize==0
                             ? info.params.l2secsize
                             : info.disc_record.log2secsize;

    if(info.disc.byte64SWI)
    {
        swi_no                  = info.disc.byte64SWI;
        discspec64.driveno      = info.disc.drive;
        discspec64.address.low  = start<<secsize;
        discspec64.address.high = start>>(32-secsize);
        regs.r[1]               = rw;
        regs.r[2]               = (size_t)&discspec64;
        regs.r[3]               = (size_t)buffer;
        regs.r[4]               = size;
        regs.r[5]               = 0;
        regs.r[6]               = 0;
    }
    else
    {
        swi_no    = info.disc.sectorSWI ? info.disc.sectorSWI : info.disc.byteSWI;
        regs.r[1] = rw;
        regs.r[2] = (info.disc.sectorSWI ? start : start<<secsize) |
                    (info.disc.drive<<29);
        regs.r[3] = (size_t)buffer;
        regs.r[4] = size;
    }

    return DiscOp(swi_no, &regs, &regs);
}

/*
 * Description: Read bytes from disc
 * Parameters : buffer, start (bytes), size (bytes)
 * Returns    : none
 */
void ReadBytes(byte *buffer, bits start, bits size)
{
    /* add partition offset if less than 512MB, otherwise use sector call */
    if(info.params.startsec<(1u<<20))
    {
        start += info.params.startsec << info.params.l2secsize;
    }
    else
    {
        ReadSectors(buffer, start>>info.params.l2secsize, size);
        return;
    }

    if(info.opts.file)
    {
        os_error *err = NULL;
        bits64 addr;
        addr.low  = start;
        addr.high = 0;

        if(info.file.sparce)
        {
            /* fix up for prior to 1.52 where only bootblook is present when reading LBA0 */
            if(start==LBA0_ADDR)
            {
                bits pos = 0;

                if(SparceFile_Find(info.file.h, addr, size, &pos)==NULL && pos==0)
                {
                    memset(buffer, 0, LBA0_SIZE);
                    addr.low = BOOT_BLOCK_ADDR;
                    size     = BOOT_BLOCK_SIZE;
                    buffer  += BOOT_BLOCK_ADDR;
                }
            }

            err = SparceFile_Read(info.file.h, addr, size, buffer);
        }
        else
        {
            err = BigFile_Read(info.file.h, addr, size, buffer);
        }

        if(err)
            Error("Error reading disc image - '%s' (&%X)\r\n", err->errmess, err->errnum);
    }
    else
    {
        _kernel_swi_regs regs;
        regs.r[1] = 1;
        regs.r[2] = start | (info.disc.drive<<29);
        regs.r[3] = (size_t)buffer;
        regs.r[4] = size;

        memset(buffer, 0, size);
        DiscOp(info.disc.byteSWI, &regs, &regs);

    }
}

/*
 * Description: Write bytes to disc
 * Parameters : buffer, start (bytes), size (bytes)
 * Returns    : none
 */
void WriteBytes(byte *buffer, bits start, bits size)
{
    // add partition offset if less than 512MB, otherwise use sector call
    if(info.params.startsec<(1u<<20))
    {
        start += info.params.startsec << info.params.l2secsize;
    }
    else
    {
        WriteSectors(buffer, start>>info.params.l2secsize, size);
        return;
    }

    if(info.opts.file)
    {
        os_error *err = NULL;
        bits64 addr;
        addr.low  = start;
        addr.high = 0;

        if(info.file.sparce)
            err = SparceFile_Write(info.file.h, addr, size, buffer);
        else
            err = BigFile_Write(info.file.h, addr, size, buffer);

        if(err)
            Error("Error writing disc image - '%s' (&%X)\r\n", err->errmess, err->errnum);
    }
    else
    {
        _kernel_swi_regs regs;
        regs.r[1] = 2;
        regs.r[2] = start | (info.disc.drive<<29);
        regs.r[3] = (size_t)buffer;
        regs.r[4] = size;

        DiscOp(info.disc.byteSWI, &regs, &regs);
    }
}

/*
 * Description: Read sectors from disc
 * Parameters : buffer, start (sectors), size (bytes)
 * Returns    : none
 */
void ReadSectors(byte *buffer, bits start, bits size)
{
    bits secsize = info.disc_record.log2secsize==0 ? info.params.l2secsize : info.disc_record.log2secsize;

    start += info.params.startsec;

    if(info.opts.file)
    {
        os_error *err = NULL;
        bits64 addr;
        addr.low  = start<<secsize;
        addr.high = start>>(32-secsize);

        if(info.file.sparce)
            err = SparceFile_Read(info.file.h, addr, size, buffer);
        else
             err = BigFile_Read(info.file.h, addr, size, buffer);

        if(err)
            Error("Error reading disc image - '%s' (&%X)\r\n", err->errmess, err->errnum);
    }
    else
    {
        /* attemp to read in one operation */
        if(!SectorDiscOp(1, buffer, start, size))
        {
            /* if failed, do single sector reads */
            memset(buffer, 0, size);
            secsize = 1u<<secsize;

            while(size)
            {
                SectorDiscOp(1, buffer, start, secsize);
                buffer += secsize;
                size   -= secsize;
                start++;
            }
        }
    }
}

/*
 * Description: Write sectors to disc
 * Parameters : buffer, start (sectors), size (bytes)
 * Returns    : none
 */
void WriteSectors(byte *buffer, bits start, bits size)
{
    bits secsize = info.disc_record.log2secsize==0 ? info.params.l2secsize : info.disc_record.log2secsize;

    start += info.params.startsec;

    if(info.opts.file)
    {
        os_error *err = NULL;
        bits64 addr;
        addr.low  = start<<secsize;
        addr.high = start>>(32-secsize);

        if(info.file.sparce)
            err = SparceFile_Write(info.file.h, addr, size, buffer);
        else
            err = BigFile_Write(info.file.h, addr, size, buffer);
 
        if(err)
            Error("Error writing disc image - '%s' (&%X)\r\n", err->errmess, err->errnum);
    }
    else
    {
        SectorDiscOp(2, buffer, start, size);
    }
}

/*
 * Description: Save buffer to undo file
 * Parameters : buffer, sectors/byte flag, start (sectors/bytes), size (bytes)
 * Returns    : none
 */
void Extract(byte *buffer, osbool sectors, bits start, bits size)
{
    bits64    addr;
    os_error *err;
    bits      secsize = info.disc_record.log2secsize;

    if(sectors)
    {
        addr.low  = start<<secsize;
        addr.high = start>>(32-secsize);
    }
    else
    {
        addr.low  = start;
        addr.high = 0;
    }

    if((err=SparceFile_Append(info.extract_file, addr, size, buffer))!=NULL)
        Error("Unable to write undo file (%s)\r\n", err->errmess);
}

/* EOF */
