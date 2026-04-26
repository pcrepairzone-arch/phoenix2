/*******************************************************************************
*                                                                              *
* Project       : DiscReader                                                   *
* Filename      : BootRecord.h                                                 *
* Version       : 1.13 (24-Jun-2017)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Reads E & E+ discs using code from DiscKnight                *
* Information   : Boot Block & Record functions                                *
*                                                                              *
********************************************************************************
* Change Log:                                                                  *
*                                                                              *
* Ver   Date         Description of change                                     *
* ----  -----------  ---------------------                                     *
* 1.00  11-Jun-2001  Initial revision based on DiscKnight 1.28 (09-Jun-2001)   *
* 1.10  02-Dec-2001  Updated with changes from DiscKnight 1.34 (23-Nov-2001)   *
* 1.11  23-Apr-2007  Updated with changes from DiscKnight 1.48 (15-Jun-2005)   *
* 1.12  11-Apr-2017  Updated with changes from DiscKnight 1.52 (10-Apr-2017)   *
* 1.13  24-Jun-2017  Updated with changes from DiscKnight 1.54 (24-Jun-2017)   *
*                                                                              *
*******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "oslib/osfile.h"

#include "DiscReader.h"

/* --- literals ------------------------------------------------------------- */

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

/* --- statics -------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

/* --- functions ------------------------------------------------------------ */

osbool ReadDefectLists(osbool big_flag);
byte   BootBlockChecksum(byte *bootblock);

/* -------------------------------------------------------------------------- */

/*
 * Description: Reads boot block structures
 * Parameters : (in globals)
 * Returns    : TRUE if ok
 */
osbool ReadBootBlock(void)
{
    osbool ok  = TRUE;

    /* Allocate buffer to read 4K LBA0 */
    if((info.lba0=malloc(LBA0_SIZE))==NULL)
    {
        Error("Unable to allocate memory for the boot block\r\n");
        safe_exit(2);
    }

    /* Set bootblock to point location within LBA0 */
    info.bootblock = info.lba0 + BOOT_BLOCK_ADDR;

    /* read LBA0 assuming 4K disc */
    ReadBytes(info.lba0, LBA0_ADDR, LBA0_SIZE);
    GetDiscRecord(&info.disc_record, info.bootblock+BOOT_BLOCK_RECORD_OFFSET);

    /* boot record */
    if(info.opts.verbose)
    {
        printf("Boot block - Boot Record\n");
        DisplayRecord(&info.disc_record, FALSE);
    }

    /* check the disc record parameters are sensible */
    if(!ReadDefectLists(info.disc_record.big_flag))
        ok = FALSE;

    if(info.opts.extract)
    {
        /* always save boot record in undo file for sector size info */
        Extract(info.bootblock, FALSE, BOOT_BLOCK_ADDR, BOOT_BLOCK_SIZE);
    }

    /* check disc size requires sector SWI */
    if((info.disc_record.disc_size.high>0 || info.disc_record.disc_size.low>=(1u<<29)))
    {
        /* >512MB so check to see sector was found SWI */
        if(!info.opts.file && info.disc.sectorSWI==0)
        {
            Error("Big disc but unable to find SectorOP SWI\n");
            safe_exit(25);
        }
    }
    else
    {
        /*
         * <512MB so dont use sector SWI incase of a FS which provides it,
         * but is running on RO 3.1
         */
        info.disc.sectorSWI = 0;
        info.disc.byte64SWI = 0;
    }

    printf("\n");

    return ok;
}

/*
 * Description: read boot block defect lists
 * Parameters : big flag
 * Returns    : TRUE if ok to continue
 */
osbool ReadDefectLists(osbool big_flag)
{
    osbool ok          = TRUE;
    bits   i           = 0;
    bits   last        = 0;
    osbool end         = FALSE;
    bits   *bootblockw = (bits*)info.bootblock;

    printf("Boot Block - Defect List\n");

    /* primary defect list */
    last = ((BOOT_BLOCK_DEFECTS_END-8)>>2);
    do
    {
        end = bootblockw[i]==0 || bootblockw[i]>=DEFECT_LIST_END1;

        if(!end)
        {
            Info("  %3u &%08X (bytes)\n",i,bootblockw[i]);
            i++;
        }
    } while(!end && i<last);

    if((bootblockw[i]&0xFFFFFF00)==DEFECT_LIST_END1)
        Info("  End1 &%08X\n", bootblockw[i]);

    /* 2nd defect list for big discs */
    if(big_flag)
    {
        i++;
        last = ((BOOT_BLOCK_DEFECTS_END-8)>>2);
        do
        {
            end = bootblockw[i]==0 || bootblockw[i]>=DEFECT_LIST_END2;

            if(!end)
            {
                Info("  %3u &%08X (sectors)\n",i,bootblockw[i]);
                i++;
            }
        } while(!end && i<last);

        if((bootblockw[i]&0xFFFFFF00)==DEFECT_LIST_END2)
            Info("  End2 &%08X\n", bootblockw[i]);
    }
    return ok;
}

/*
 * Description: Calculates the boot block checksum
 * Parameters : bootblock sector
 * Returns    : checksum value
 */
byte BootBlockChecksum(byte *bootblock)
{
    bits crc   = 0;
    bits carry = 0;
    int  i;

    for(i=BOOT_BLOCK_CRC-1; i>=0; i--)
    {
        crc  += bootblock[i] + carry;
        carry = crc >> 8;
        crc  &= 0xFF;
    }

    return (byte)crc;
}

/*
 * Description: Writes the bootblock with the correct CRC
 * Parameters : (none)
 * Returns    : TRUE
 */
osbool WriteBootBlock(void)
{
    info.bootblock[BOOT_BLOCK_CRC] = BootBlockChecksum(info.bootblock);
    WriteBytes(info.lba0, LBA0_ADDR, LBA0_SIZE);
    return(TRUE);
}

/*
 * Description: Gets the disc record from memory block
 * Parameters : pointer to record structure, pointer to record
 * Returns    : (none)
 */
void GetDiscRecord(s_disc_record *dr, byte *br)
{
    int i;

    /* get info from boot record */
    dr->log2secsize     = br[DISC_RECORD_LOG2SECSIZE];
    dr->secspertrack    = br[DISC_RECORD_SECSPERTRACK];
    dr->heads           = br[DISC_RECORD_HEADS];
    dr->density         = br[DISC_RECORD_DENSITY];
    dr->idlen           = br[DISC_RECORD_IDLEN];
    dr->log2bpmb        = br[DISC_RECORD_LOG2BPMB];
    dr->skew            = br[DISC_RECORD_SKEW];
    dr->bootoption      = br[DISC_RECORD_BOOTOPTION];
    dr->lowsector       = br[DISC_RECORD_LOWSECTOR];
    dr->nzones          = br[DISC_RECORD_NZONES]+(br[DISC_RECORD_NZONES2]<<8);
    dr->zone_spare      = *(half*)(br+DISC_RECORD_ZONE_SPARE);
    dr->root            = *(bits*)(br+DISC_RECORD_ROOT);
    dr->disc_size.low   = *(bits*)(br+DISC_RECORD_DISC_SIZE_LOW);
    dr->share_size      = br[DISC_RECORD_SHARE_SIZE];
    dr->big_flag        = br[DISC_RECORD_BIG_FLAG];
    dr->disc_size.high  = dr->big_flag ? *(bits*)(br+DISC_RECORD_DISC_SIZE_HIGH) : 0;
    dr->cycle_id        = *(half*)(br+DISC_RECORD_CYCLE_ID);
    dr->disc_type       = *(bits*)(br+DISC_RECORD_DISC_TYPE);
    dr->filecore_ver    = *(bits*)(br+DISC_RECORD_FILECORE_VER);
    dr->root_dir_size   = *(bits*)(br+DISC_RECORD_ROOT_DIR_SIZE);


    for(i=0; i<DISC_RECORD_NAME_SIZE; i++)
    {
        char c = br[DISC_RECORD_NAME+i];
        if(c<=' ')
            dr->name[i] = 0;
        else
            dr->name[i] = c;
    }
    dr->name[10] = 0;
}

/*
 * Description: Display disc record
 * Parameters : pointer to record structure,
 *            : full - true for map disc record
 *            :      - false for limited boot disc record
 * Returns    : TRUE if ok
 */
void DisplayRecord(s_disc_record *dr, osbool full)
{
    /* get info from boot record */
    const char *type    = NULL;
    bits        tracks  = (dr->disc_size.low>>dr->log2secsize)+(dr->disc_size.high<<(32-dr->log2secsize));
    bits        secsize = 1u<<dr->log2secsize;
    bits        minimum = (((dr->idlen+1)<<dr->log2bpmb)+secsize-1) & ~(secsize-1);

    if(dr->secspertrack!=0 && dr->heads!=0)
        tracks /= (dr->secspertrack*dr->heads);
    else
        tracks = 0;

    printf("  %-24s : %u\n", DESC_TRACKS,      tracks);
    printf("  %-24s : %u\n", DESC_HEADS,       dr->heads);
    printf("  %-24s : %u\n", DESC_SECPERTRACK, dr->secspertrack);
    printf("  %-24s : %u\n", DESC_LOG2SECSIZE, 1u<<dr->log2secsize);

    switch(dr->density)
    {
        case 0:  type = "hard disc";       break;
        case 1:  type = "single density";  break;
        case 2:  type = "double density";  break;
        case 3:  type = "double+ density"; break;
        case 4:  type = "quad density";    break;
        case 8:  type = "octal density";   break;
        default: type = NULL;              break;
    }
    if(type==NULL)
        printf("  %-24s : %u?\n", DESC_DENSITY, dr->density);
    else
        printf("  %-24s : %s\n",  DESC_DENSITY, type);

    printf("  %-24s : %u\n",         DESC_IDLEN,       dr->idlen);
    printf("  %-24s : %u\n",         DESC_LOG2BPMB,    1u<<dr->log2bpmb);
    printf("  %-24s : %s\n",         DESC_MINOBJECT,   Size32(minimum));
    printf("  %-24s : %u\n",         DESC_SKEW,        1u<<dr->skew);
    printf("  %-24s : %u\n",         DESC_BOOTOPTION,  dr->bootoption);
    printf("  %-24s : %u\n",         DESC_LOWSECTOR1,  dr->lowsector & 0x1F);
    printf("  %-24s : %s\n",         DESC_LOWSECTOR2, (dr->lowsector & (1u<<6)) ? "sequenced" : "interleaved");
    printf("  %-24s : %s\n",         DESC_LOWSECTOR3, (dr->lowsector & (1u<<7)) ? "Yes" : "No");
    printf("  %-24s : %u\n",         DESC_NZONES,      dr->nzones);
    printf("  %-24s : %u\n",         DESC_ZONESPARE,   dr->zone_spare);
    printf("  %-24s : &%08X\n",      DESC_ROOT,        dr->root);
    printf("  %-24s : &%s (%s)\n",   DESC_DISCSIZE,    Print64(dr->disc_size),
                                                       Size64(dr->disc_size));
    printf("  %-24s : &%04X\n",      DESC_CYCLEID,     dr->cycle_id);
    printf("  %-24s : %u sectors\n", DESC_SHARESIZE,   1u<<dr->share_size);
    printf("  %-24s : &%02X\n",      DESC_BIGFLAG,     dr->big_flag);
    printf("  %-24s : %u\n",         DESC_FILECOREVER, dr->filecore_ver);
    printf("  %-24s : %u\n",         DESC_ROOTDIRSIZE, dr->root_dir_size);
    printf("  %-24s : &%03X\n",      DESC_DISCTYPE,    dr->disc_type);

    if(full)
    {
        printf("  %-24s : '%s'\n",   DESC_NAME,        dr->name);
    }
}

/*
 * Description: Calculate map location from supplied disc record
 * Parameters : disc record
 * Returns    : location of first map copy
 */
bits CalcMapLocation(s_disc_record *dr)
{
    bits map_zone  = dr->nzones/2;
    bits zone_size = (1u<<(dr->log2secsize+3)) - dr->zone_spare;

    if(dr->log2bpmb > dr->log2secsize)
        return ((map_zone*zone_size)-DISC_RECORD_BITS) << (dr->log2bpmb-dr->log2secsize);
    else
        return ((map_zone*zone_size)-DISC_RECORD_BITS) >> (dr->log2secsize-dr->log2bpmb);
}

/* EOF */
