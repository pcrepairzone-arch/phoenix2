/*******************************************************************************
*                                                                              *
* Project       : DiscReader                                                   *
* Filename      : Map.c                                                        *
* Version       : 1.13 (20-Aug-2017)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Reads E & E+ discs using code from DiscKnight                *
* Information   : Map checking functions                                       *
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

/* reading */
osbool ReadMapRecord(void);
osbool ReadMapContents(void);

/* utility */
bits   MapZoneCheck(byte *map, bits zone);
void   AddID(bits id, bits zone, bits start, bits len);
s_objinfo *NewObject(s_usedinfo *chunk);
void   AddFree(bits zone, bits start, bits len);
bits   FindUnusedID(bits zone);
void   SetMapBits(bits zone, bits start, bits len, bits value);

/* --- Map checking & fixing functions ------------------------------------- */

/*
 * Description: Reads map
 * Parameters : (none)
 * Returns    : TRUE if ok
 */
osbool ReadMap(void)
{
    osbool  ok      = TRUE;
    bits    secsize = 1u<<info.disc_record.log2secsize;

    /* calculate location of maps - first map is middle zone rounded down
     * zone size is allocation bits (sector size - spare) * LFAU
     * first zone is reduced by the size of the disc record 60*8 bits
     */
    info.stats.size.map     = info.disc_record.nzones<<info.disc_record.log2secsize;
    info.stats.size.zone    = (1u<<(info.disc_record.log2secsize+3)) - info.disc_record.zone_spare;
    info.stats.size.ids     = 1u<<info.disc_record.idlen;
    info.stats.size.ids_pz  = ((8<<info.disc_record.log2secsize)-info.disc_record.zone_spare)/(info.disc_record.idlen+1);
    info.stats.size.lfau    = 1u<<(info.disc_record.log2bpmb>info.disc_record.log2secsize ? info.disc_record.log2bpmb : info.disc_record.log2secsize);
    info.stats.size.l2share = info.disc_record.log2secsize+info.disc_record.share_size;
    info.stats.size.share   = 1u<<info.stats.size.l2share;
    info.stats.size.minimum = (((info.disc_record.idlen+1)<<info.disc_record.log2bpmb)+secsize-1) & ~(secsize-1);
    info.locations.map[0]   = CalcMapLocation(&info.disc_record);
    info.locations.map[1]   = info.locations.map[0] + info.disc_record.nzones;
    info.locations.map_zone = info.disc_record.nzones/2;

    Info("Disc Map\n");
    Info("  Copy 1 at &%s\n", SecAddrToString(info.locations.map[0]));
    Info("  Copy 2 at &%s\n", SecAddrToString(info.locations.map[1]));
    Info("  Size %s\n",       Size32(info.stats.size.map));

    /* allocate memory for the maps and the object structures */
    if((info.map[0]=malloc(info.stats.size.map)) == NULL)
    {
        Error("Unable to allocate memory for disc map 1\r\n");
        safe_exit(3);
    }
    if((info.map[1]=malloc(info.stats.size.map)) == NULL)
    {
        Error("Unable to allocate memory for disc map 2\r\n");
        safe_exit(4);
    }

    if((info.objects=malloc(info.stats.size.ids*sizeof(s_objinfo*))) == NULL)
    {
        Error("Unable to allocate memory for map structure\r\n");
        safe_exit(5);
    }
    memset(info.objects, 0, info.stats.size.ids*sizeof(s_objinfo*));

    /* read the maps */
    ReadSectors(info.map[0], info.locations.map[0], info.stats.size.map);
    ReadSectors(info.map[1], info.locations.map[1], info.stats.size.map);

    if(info.opts.extract)
    {
        Extract(info.map[0], TRUE, info.locations.map[0], info.stats.size.map);
        Extract(info.map[1], TRUE, info.locations.map[1], info.stats.size.map);
    }

    /* discard 2nd copy of map */
    free(info.map[1]);
    info.map[1] = NULL;

    ReadMapRecord();
    ReadMapContents();

    printf("\n");

    return ok;
}

/*
 * Description: Reads map disc record
 * Parameters : pointer to write flag - updated on exit
 * Returns    : TRUE if ok
 */
osbool ReadMapRecord(void)
{
    osbool         ok = TRUE;
    s_disc_record *dr = &info.disc_record;

    printf("Map - Disc Record\n");

    /* use the map copy from now on*/
    GetDiscRecord(dr, info.map[0]+MAP_BITS_OFFSET);

    Info(  "  %-19s : %u\n",    DESC_FILECOREVER, dr->filecore_ver);
    Info(  "  %-19s : '%s'\n",  DESC_NAME,        dr->name);
    Info(  "  %-19s : &%03X\n", DESC_DISCTYPE,    dr->disc_type);
    Info(  "  %-19s : &%X\n",   DESC_ROOTDIRSIZE, dr->root_dir_size);

    return ok;
}

/*
 * Description: Reads map contents
 * Parameters : updated on exit
 * Returns    : TRUE if ok
 */
osbool ReadMapContents(void)
{
    bits   lfau_total = ADDR64SEC(info.disc_record.disc_size, info.disc_record.log2bpmb);
    bits   last_zone  = (info.disc_record.nzones-1) * info.stats.size.zone - DISC_RECORD_BITS;
    bits   disc_end   = lfau_total-last_zone;
    osbool ok         = TRUE;
    bits   zone;

    for(zone=0; zone<info.disc_record.nzones; zone++)
    {
        byte *mapzone      = info.map[0] + (zone << info.disc_record.log2secsize);
        byte *mapzonebits  = mapzone+MAP_BITS_OFFSET;
        bits  allocbit     = 0;
        bits  allocend     = info.stats.size.zone;
        bits  freeoff      = mapzone[1]+(mapzone[2]<<8);

        freeoff = (freeoff & 0x7FFF)-MAP_FREECHAIN_ZERO;

        if(zone == 0)
            allocbit += DISC_RECORD_BITS;

        if(info.opts.show.map)
        {
            printf("\nMap - Zone %u\n", zone);

            if(freeoff == MAP_FREECHAIN_EMPTY)
                printf("  Free Offset : (none)\n");
            else
                printf("  Free Offset : &%04X\n", freeoff);
        }

        do
        {
            bits        start = allocbit;
            bits        id    = 0;
            bits        last  = 0;
            bits        bitno = 0;
            bits        size  = 0;
            const char *type   = "File number";

            /* Get ID, read idlen bits of file number */
            for(bitno=0; bitno<info.disc_record.idlen; bitno++)
            {
                id |= ((mapzonebits[allocbit>>3] >> (allocbit & 7)) & 1) << bitno;
                allocbit++;
            }

            /* skip 0 bits */
            do
            {
                last = mapzonebits[allocbit>>3] & (1u<<(allocbit & 7));
                allocbit++;
            } while(last==0 && allocbit<allocend);

            /* check if there is room for another ID, if not extend this one to end */
            if(allocbit!=allocend && allocend-allocbit<info.disc_record.idlen+1)
            {
                allocbit = allocend;
                last     = 0;
            }

            size = (allocbit-start) << info.disc_record.log2bpmb;

            /* check terminating bit */
            if(last==0 && !(zone==info.disc_record.nzones-1 && allocbit==allocend))
            {
                ok   = FALSE;
                type = "BAD ENTRY  ";
            }

            /* check for free space */
            if(start == freeoff)
            {
                /* add to free list */
                freeoff += id;
                type     = id==0 ? "Last Free  " : "Free block ";

                AddFree(zone, start, allocbit-start);
            }
            else
            {
                /* valid ID, add to store unless end of disc*/
                if(id>1 || start!=disc_end)
                    AddID(id, zone, start, allocbit-start);
            } /* if start == freeoff */

            if(id==1)
            {
                if(zone==info.disc_record.nzones-1 && start==disc_end)
                {
                    type = "Disc End   ";
                }
                else
                {
                    type = "Defect     ";
                    info.stats.count.defects++;
                    INC64(info.stats.space.defects, size);
                }
            }
            else if(id == 2)
            {
                type = "System file";
            }

            if(info.opts.show.map)
            {
                bits secaddr = MapAddr(zone, start);

                printf("  &%04X-&%04X : &%s +&%08X %s &%06X\n",
                       start, allocbit-1,
                       SecAddrToString(secaddr),
                       size, type, id);
            }
        } while(allocbit < allocend);
    } /* for zone */

    return ok;
}

/* --- Map utility functions ------------------------------------------------ */

/*
 * Description: calculates zone check for map zone
 * Parameters : map buffer, zone number
 * Returns    : zone_check
 */
bits MapZoneCheck(byte *map, bits zone)
{
    bits zone_start  = zone << info.disc_record.log2secsize;
    bits sum_vector0 = 0;
    bits sum_vector1 = 0;
    bits sum_vector2 = 0;
    bits sum_vector3 = 0;
    bits rover;

    for(rover=zone_start+(1u<<info.disc_record.log2secsize)-4;
        rover>zone_start; rover-=4 )
    {
        sum_vector0 += map[rover+0] + (sum_vector3>>8);
        sum_vector3 &= 0xff;
        sum_vector1 += map[rover+1] + (sum_vector0>>8);
        sum_vector0 &= 0xff;
        sum_vector2 += map[rover+2] + (sum_vector1>>8);
        sum_vector1 &= 0xff;
        sum_vector3 += map[rover+3] + (sum_vector2>>8);
        sum_vector2 &= 0xff;
    }

    sum_vector0 +=                (sum_vector3>>8);
    sum_vector1 += map[rover+1] + (sum_vector0>>8);
    sum_vector2 += map[rover+2] + (sum_vector1>>8);
    sum_vector3 += map[rover+3] + (sum_vector2>>8);

    return (sum_vector0^sum_vector1^sum_vector2^sum_vector3) & 0xFF;
}

/*
 * Description: Writes both maps (from copy 1) with correct check bytes
 * Parameters : (none)
 * Returns    : TRUE if successful
 */
// cppcheck-suppress unusedFunction
osbool WriteMap(void)
{
    bits map_no     = 0;
    byte cross_check[2];
    bits zone;

    cross_check[map_no] = 0;

    /* fix up all check bytes for modified map */
    for(zone=0; zone<info.disc_record.nzones; zone++)
    {
        bits zone_start = zone << info.disc_record.log2secsize;

        if(zone==info.disc_record.nzones-1)
            info.map[map_no][zone_start+3] = (byte)(cross_check[map_no] ^ 0xFF);
        else
            cross_check[map_no]            = (byte)(cross_check[map_no] ^ info.map[map_no][zone_start+3]);

        info.map[map_no][zone_start] = (byte)MapZoneCheck(info.map[map_no], zone);;
    }

    /* write 1st map to both copies */
    WriteSectors(info.map[0], info.locations.map[0], info.stats.size.map);
    WriteSectors(info.map[0], info.locations.map[1], info.stats.size.map);

    return TRUE;
}

/*
 * Description: Adds entry to map structure
 * Parameters : id, zone number, start bit offset, length in bits
 * Returns    : (none)
 */
void AddID(bits id, bits zone, bits start, bits len)
{
    s_usedinfo *usedinf = malloc(sizeof(s_usedinfo));

    if(usedinf==NULL)
    {
        Error("Unable to allocate memory for map object\r\n");
        safe_exit(6);
    }

    usedinf->zone           = zone;
    usedinf->start          = start;
    usedinf->len            = len;
    usedinf->next           = NULL;

    /* check for existing entry */
    if(info.objects[id] == NULL)
    {
        info.objects[id] = NewObject(usedinf);
        if(id>1)
            info.stats.count.objects++;
    }
    else
    {
        /* if id is the first of the natural start zone
         * insert at head, otherwise insert after highest in
         * list to maintain order even if wraps around disc
         */
        bits zoneid = zone*info.stats.size.ids_pz;

        if(id>=zoneid && id<zoneid+info.stats.size.ids_pz &&
           info.objects[id]->chunks->zone!=zone)
        {
            usedinf->next            = info.objects[id]->chunks;
            info.objects[id]->chunks = usedinf;
        }
        else
        {
            s_usedinfo *usedinf0 = info.objects[id]->chunks;
            s_usedinfo *usedinf2 = info.objects[id]->chunks->next;

            while(usedinf2 && usedinf2->zone>=usedinf0->zone)
            {
                usedinf0 = usedinf2;
                usedinf2 = usedinf2->next;
            }

            usedinf->next  = usedinf2;
            usedinf0->next = usedinf;
        }
    }

    if(id>1)
        INC64(info.stats.space.used, len<<info.disc_record.log2bpmb);
}

/*
 * Description: Makes a new map object info structure
 * Parameters : first used block info
 * Returns    : new objinfo pointer
 */
s_objinfo *NewObject(s_usedinfo *chunk)
{
    s_objinfo *objinf = malloc(sizeof(s_objinfo));

    if(objinf==NULL)
    {
        Error("Not enough memory to create map chunk\r\n");
        safe_exit(24);
    }

    objinf->shared          = NULL;
    objinf->directory       = 0;
    objinf->status.checked  = FALSE;
    objinf->status.shared   = FALSE;
    objinf->status.unshared = FALSE;
    objinf->status.used     = FALSE;
    objinf->chunks          = chunk;

    return objinf;
}

/*
 * Description: Returns start of chunk chain for a file ID
 * Parameters : id
 * Returns    : s_objinfo or NULL
 */
s_usedinfo *GetUsedInfo(bits id)
{
    return id<info.stats.size.ids && info.objects[id] ? info.objects[id]->chunks : NULL;
}

/*
 * Description: Adds entry to free list
 * Parameters : zone number, start bit offset, length in bits
 * Returns    : (none)
 */
void AddFree(bits zone, bits start, bits len)
{
    s_freeinfo *freeinf;

    if((freeinf=malloc(sizeof(s_freeinfo)))!=NULL)
    {
        freeinf->zone  = zone;
        freeinf->start = start;
        freeinf->len   = len;

        if(info.freelist)
            info.freelist->next = freeinf;

        freeinf->prev  = info.freelist;
        freeinf->next  = NULL;

        info.freelist  = freeinf;
    }

    info.stats.count.free++;
    INC64(info.stats.space.free, len<<info.disc_record.log2bpmb);
}

/*
 * Description: find a free id in a zone
 * Parameters : zone
 * Returns    : file id or 0 if non free
 */
bits FindUnusedID(bits zone)
{
    bits first = zone  * info.stats.size.ids_pz;
    bits last  = first + info.stats.size.ids_pz;
    bits id;

    if(first<3)
        first=3;

    /* tick off all used IDs in array */
    for(id=first; id<last; id++)
    {
        if(info.objects[id]==0)
            return id;
    }

    return 0;
}

/*
 * Description: Creates an object from a free space description
 * Parameters : freespace object, size required
 * Returns    : ID or 0 if failed
 */
// cppcheck-suppress unusedFunction
bits MakeID(s_freeinfo *freeinf, bits size)
{
    bits zone   = freeinf->zone;
    bits id     = FindUnusedID(zone);
    bits start  = freeinf->start;
    bits minlen = info.stats.size.minimum>>info.disc_record.log2bpmb;
    bits len    = (size+info.stats.size.lfau-1)>>info.disc_record.log2bpmb;
    bits del    = TRUE;
    s_freeinfo *nextfree;

    if(id==0)
        return id;

    if(len<info.disc_record.idlen+1)
        len = info.disc_record.idlen+1;

    /* check if free space chunk can be divided */
    if(freeinf->len >= minlen+len)
    {
        del = FALSE;

        /* fix up free block structure */
        nextfree         = freeinf;
        nextfree->start += len;
        nextfree->len   -= len;

        /* mark end bit of split */
        SetMapBits(zone, nextfree->start-1, 1, 1);

        /* fix up map offset if not last free in zone*/
        if(nextfree->next && nextfree->next->zone==zone)
            SetMapBits(zone, nextfree->start, info.disc_record.idlen, nextfree->next->start-nextfree->start);

    }
    else
    {
        /* claim full chunk */
        len                     = freeinf->len;

        /* set next free pointer and fix up double linked list */
        nextfree                = freeinf->next;

        if(nextfree)
            nextfree->prev      = freeinf->prev;
         else
            info.freelist       = freeinf->prev;

        if(freeinf->prev)
            freeinf->prev->next = freeinf->next;

        info.stats.count.free--;
    }
    DEC64(info.stats.space.free, len<<info.disc_record.log2bpmb);

    /* fix up free pointers to this block in the map */
    if(freeinf->prev==NULL || freeinf->prev->zone!=zone)
    {
        byte *mapzone  = info.map[0] + (zone << info.disc_record.log2secsize);

        /* claiming first free space in the zone, so fixup zone pointer */
        if(nextfree && nextfree->zone==zone)
        {
            /* point to new next free space */
            bits freeoff = MAP_FREECHAIN_ZERO + nextfree->start;
            mapzone[1]   = (byte)freeoff;
            mapzone[2]   = (byte)((freeoff>>8) | 0x80);
        }
        else
        {
            /* no more free space in zone */
            mapzone[1] = 0x00;
            mapzone[2] = 0x80;
        }
    }
    else
    {
        /* claiming non-first block, so fix previous */
        SetMapBits(zone, freeinf->prev->start, info.disc_record.idlen,
           (nextfree && nextfree->zone==zone) ? nextfree->start - freeinf->prev->start : 0);
    }

    /* set ID in map, and add to out list */
    SetMapBits(zone, start, info.disc_record.idlen, id);
    AddID(id, zone, start, len);

    if(del) free(freeinf);

    return id;
}

/*
 * Description: Returns the address correspoding to a map bit offset
 * Parameters : map zone and bit offset with in the zone
 * Returns    : sector address
 */
bits MapAddr(bits zone, bits bitoffset)
{
    bits lfauno = (zone * info.stats.size.zone) + bitoffset - DISC_RECORD_BITS;
    if(info.disc_record.log2bpmb > info.disc_record.log2secsize)
        return lfauno << (info.disc_record.log2bpmb - info.disc_record.log2secsize);
    else
        return lfauno >> (info.disc_record.log2secsize - info.disc_record.log2bpmb);
}

/*
 * Description: writes bits into map
 * Parameters : map zone, bit offset with in the zone, length in bits, value
 * Returns    : (none)
 */
void SetMapBits(bits zone, bits start, bits len, bits value)
{
    byte *mapzonebits = info.map[0] + (zone << info.disc_record.log2secsize) + MAP_BITS_OFFSET;
    bits  i;

    for(i=0; i<len; i++)
    {
        SetBit(mapzonebits, start++, (value&1));
        value = value>>1;
    }
}

/*
 * Description: Get a bit from a bit array
 * Parameters : array base, bit index
 * Returns    : bit value
 */
/* cppcheck-suppress unusedFunction */
bits GetBit(byte *base, bits bitno)
{
    return (base[bitno>>3] >> (bitno & 7)) & 1;
}

/*
 * Description: Set a bit from a bit array
 * Parameters : array base, bit index
 * Returns    : (none)
 */
void SetBit(byte *base, bits bitno, bits value)
{
    base[bitno>>3] = (byte)((base[bitno>>3] & ~(1u<<(bitno & 7))) | (value<<(bitno & 7)));
}

/* EOF */
