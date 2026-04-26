/*******************************************************************************
*                                                                              *
* Project       : DiscKnight                                                   *
* Filename      : SparceFile.c                                                 *
* Version       : 1.54 (30-May-2018)                                           *
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
* 0.06  08-Aug-2000  binary tree caching added to Find routine                 *
* 0.10  30-Aug-2000  Caching problems corrected                                *
* 0.12  25-Sep-2000  OSLib 6.11 changes (osbool and os_fw used)                *
* 0.20  25-Sep-2000  Find routine simplified, pre-cache and binary chop search *
* 1.00  08-Oct-2000  First release build                                       *
* 1.15  07-Feb-2001  Function to free cache memory on exit added               *
* 1.16  12-Feb-2001  Append adds blocks to cache so they can be re-read        *
* 1.20  24-Feb-2001  Find checks block size in match,                          *
*                    Write uses Append if too big for exisiting allocation     *
* 1.28  26-Jun-2001  SparceFile_AppendBlock() made public                      *
* 1.37  07-Jan-2002  Duplicate blocks removed from sparcefile cache            *
* 1.38  22-Jan-2002  Unecessary final compare removed from SparceFile_Find     *
* 1.52  10-Apr-2017  Exposing SparceFile_Find                                  *
* 1.54  30-May-2018  Unused fields removed from Sparce cache struct            *
*                                                                              *
*******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "oslib/osgbpb.h"
#include "oslib/osargs.h"

#include "SparceFile.h"

/* --- literals ------------------------------------------------------------- */

#define CACHE_EXTEND 1000;

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

typedef struct
{
    bits64                 block_addr;
    bits64                 block_end;
    bits                   file_pos;
} s_cache_entry;

/* --- statics -------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

static s_cache_entry *cache         = NULL;
static bits           cache_size    = 0;
static bits           cache_max     = 0;
static os_fw          cache_file    = 0;

/* --- private functions ---------------------------------------------------- */

void      SparceFile_GrowCache(void);
int       SparceFile_CmpCache(const void* e1, const void *e2);
os_error* SparceFile_GetBlockHdr(os_fw f, bits pos, bits64 *block_addr, bits *block_size);

/*
 * Description: finds a block containing the given address
 *            : if size wont fit in the block, fail
 * Parameters : fiie handle, address, size,
 *            : returned file offset of address, or 0 if fail
 * Returns    : error block or NULL for sucess
 */
os_error* SparceFile_Find(os_fw f, bits64 addr, bits size, bits *pos)
{
    bits64    addr_end = Add64_32(addr, size);

    (void)size;

    if(f != cache_file)
    {
        os_error *err;
        bits extent  = 0;
        bits readpos = SparceFile_MAGIC_LEN;
        bits i;

        /* (re)inintalise the cache */
        cache_file    = f;
        cache_size    = 0;

        if((err=xosargs_read_extw(f, (int*)&extent))!=NULL)
            return err;

        do
        {
            bits64    block_addr;
            bits      block_size;

            /* get block address and size */
            if((err=SparceFile_GetBlockHdr(f, readpos, &block_addr, &block_size))!=NULL)
                return err;

            /* ignore free block */
            if(!(block_addr.low==0xFFFFFFFF && block_addr.high==0xFFFFFFFF))
            {
                /* grow cache if necessary */
                SparceFile_GrowCache();

                /* check for duplicates from early extracts which read directories in two steps */
                if(cache_size>1 && CMP64(block_addr, cache[cache_size-1].block_addr)==0)
                    cache_size--;

                /* add to cache */
                cache[cache_size].block_addr   = block_addr;
                cache[cache_size].block_end    = Add64_32(block_addr,block_size);
                cache[cache_size].file_pos     = readpos;
                cache_size++;
            }

            readpos += sizeof(bits)*3 + block_size;

        } while(readpos<extent);

        /* sort cache for binary chop searches */
        qsort(cache, cache_size, sizeof(s_cache_entry), SparceFile_CmpCache);

        /* remove any duplicate addresses */
        for(i=1; i<cache_size; i++)
        {
            if(CMP64(cache[i].block_addr, cache[i-1].block_addr)==0)
            {
                /* keep the highest file position */
                bits del = cache[i].file_pos>cache[i-1].file_pos ? i-1 : i;
                cache_size--;
                memmove(&cache[del], &cache[del+1], (cache_size-del)*sizeof(s_cache_entry));
                i--;
            }
        }
    }

    /* look for block in the cache, using binary chop */
    if(cache_size > 0)
    {
        bits   first    = 0;
        bits   last     = cache_size;

        do
        {
            bits middle = first + ((last-first)/2);
            int  cmp;

            if((cmp=CMP64(addr, cache[middle].block_addr))>=0 &&
                CMP64(addr_end, cache[middle].block_end)<=0)
            {
                bits64 offset = Sub64(addr, cache[middle].block_addr);
                *pos          = cache[middle].file_pos + sizeof(bits)*3 + offset.low;
                return NULL;
            }
            else if(cmp<0)
            {
                last = middle;
            }
            else
            {
                first = middle+1;
            }

        } while(first!=last);
    }

    /* not found */
    *pos = 0;
    return NULL;
}


/*
 * Description: grows the cache if necessary
 * Parameters : none
 * Returns    : nine
 */
void SparceFile_GrowCache(void)
{
    if(cache_size == cache_max)
    {
        cache_max += CACHE_EXTEND;
        // cppcheck-suppress memleakOnRealloc
        if((cache = realloc(cache, cache_max*sizeof(s_cache_entry)))==NULL)
        {
            fprintf(stderr, "No memory to extend SparceFile cache\r\n");
            exit(7);
        }
    }
}

/*
 * Description: qsort comparison routine for cache entries
 * Parameters : cache entry pointers
 * Returns    : <0, 0, >0
 */
int SparceFile_CmpCache(const void* e1, const void *e2)
{
    const s_cache_entry *c1 =(const s_cache_entry*)e1;
    const s_cache_entry *c2 =(const s_cache_entry*)e2;
    return CMP64(c1->block_addr, c2->block_addr);
}

/*
 * Description: get a block header
 * Parameters : file handle, file position,
 *            : pointer 64 bit address, pointer to size - updated in exit
 * Returns    : error block or NULL
 */
os_error* SparceFile_GetBlockHdr(os_fw f, bits pos, bits64 *block_addr, bits *block_size)
{
    os_error *err;
    int       unread;

    if((err=xosgbpb_read_atw(f, (byte*)&block_addr->low, sizeof(bits), pos, &unread))!=NULL)
        return err;
    if((err=xosgbpb_readw(f, (byte*)&block_addr->high, sizeof(bits), &unread))!=NULL)
        return err;
    if((err=xosgbpb_readw(f, (byte*)block_size, sizeof(bits), &unread))!=NULL)
        return err;

    return NULL;
}

/* --- public functions ----------------------------------------------------- */

/*
 * Description: write a raw block to the end of the sparce file
 * Parameters : file handle, 64 bit address, size, data buffer
 * Returns    : error block or NULL for sucess
 */
os_error* SparceFile_AppendBlock(os_fw f, bits64 addr, bits size, byte *data)
{
    os_error *err = NULL;
    int       unwritten;
    bits      pos = 0;

    /* find end postition of the file */
    if((err=xosargs_read_extw(f, (int*)&pos))!=NULL)
        return err;

    /* add block to cache, if set to this file */
    if(f == cache_file)
    {
        bits   first    = 0;
        bits   last     = cache_size;
        int    cmp      = 0;

        /* find insertion point using binary cut */
        do
        {
            bits middle = first + ((last-first)/2);

            if((cmp=CMP64(addr, cache[middle].block_addr))==0)
            {
                first = last = middle;
            }
            else if(cmp<0)
            {
                last = middle;
            }
            else
            {
                first = middle+1;
            }

        } while(first!=last);

        /* if not exact match make room */
        if(cmp!=0)
        {
            /* grow cache if necessary */
            SparceFile_GrowCache();

            /* insert into array */
            memmove(&cache[first+1], &cache[first], (cache_size-first)*sizeof(s_cache_entry));
            cache_size++;
        }
        else
        {
            /* mark old block in file as free */

            /* when used by write, find has been used so block must be too small */
            /* #### when used by append could check size and reuse space in file */
            bits64 unused;
            unused.low  = 0xFFFFFFFF;
            unused.high = 0xFFFFFFFF;

            if((err=xosargs_set_ptrw(f, cache[first].file_pos))!=NULL                      ||
               (err=xosgbpb_writew(f, (byte*)&unused.low, sizeof(bits), &unwritten))!=NULL ||
               (err=xosgbpb_writew(f, (byte*)&unused.high,sizeof(bits), &unwritten))!=NULL)
            {
                return err;
            }
        }

        /* update cache entry */
        cache[first].block_addr   = addr;
        cache[first].block_end    = Add64_32(addr, size);
        cache[first].file_pos     = pos;
    }

    /* write block to the end of the file */
    if((err=xosargs_set_ptrw(f, pos))!=NULL)
        return err;

    if((err=xosgbpb_writew(f, (byte*)&addr.low, sizeof(bits), &unwritten))!=NULL ||
       (err=xosgbpb_writew(f, (byte*)&addr.high,sizeof(bits), &unwritten))!=NULL ||
       (err=xosgbpb_writew(f, (byte*)&size,     sizeof(bits), &unwritten))!=NULL)
    {
        return err;
    }

    return xosgbpb_writew(f,  data, size, &unwritten);
}

/*
 * Description: Creates a sparce file read for use by SparceFile_Append()
 * Parameters : filename
 * Returns    : error block or NULL for sucess
 */
os_error* SparceFile_Create(char *filename)
{
    os_fw     f   = 0;
    os_error *err = NULL;
    int       unwritten;

    if((err=xosfind_openoutw(osfind_NO_PATH, filename, NULL, &f))!=NULL)
        return err;

    err = xosgbpb_writew(f, (byte*)SparceFile_MAGIC,  SparceFile_MAGIC_LEN, &unwritten);
    xosfind_closew(f);
    return err;
}

/*
 * Description: Checks if open file is a space file
 * Parameters : file handle
 * Returns    : true/false
 */
osbool SparceFile_Check(os_fw f)
{
    byte     buffer[SparceFile_MAGIC_LEN];
    int      unread = 0;
    os_error *err   = xosgbpb_read_atw(f, (byte*)&buffer,
                                       SparceFile_MAGIC_LEN,
                                       0, &unread);

    return err==NULL && unread==0 && memcmp(buffer, SparceFile_MAGIC, SparceFile_MAGIC_LEN)==0;
}

/*
 * Description: Adds a block to a previously created sparce file
 * Parameters : filename, 64 bit address, size, data buffer, overwrite
 * Returns    : error block or NULL for sucess
 */
os_error* SparceFile_Append(char *filename, bits64 addr, bits size, byte *data)
{
    os_error *err       = NULL;
    os_fw     f         = 0;

    if((err=xosfind_openupw(osfind_NO_PATH, filename, NULL, &f))!=NULL || f==0)
        return err;

    err = SparceFile_AppendBlock(f, addr, size, data);
    xosfind_closew(f);
    return err;
}

/*
 * Description: write a to a sparce file
 *            : writes a block if new, otherwise updates contents of
 *            : whole or part of an existing block
 * Parameters : file handle, 64 bit address, size, data buffer
 * Returns    : error block or NULL for sucess
 */
os_error* SparceFile_Write(os_fw f, bits64 addr, bits size, byte *data)
{
    int       unwritten;
    bits      pos;
    os_error *err;

    if((err = SparceFile_Find(f, addr, size, &pos))!=NULL)
        return err;

    if(pos)
        return xosgbpb_write_atw(f, data, size, pos, &unwritten);
    else
        return SparceFile_AppendBlock(f, addr, size, data);
}

/*
 * Description: read data from a sparce file
 *            : can read range from with in a previously written block
 *            : any data outside valid range is zeroed
 * Parameters : file handle, 64 bit address, size, data buffer
 * Returns    : error block or NULL for sucess
 */
os_error* SparceFile_Read(os_fw f, bits64 addr, bits size, byte *data)
{
    int       unread;
    bits      pos;
    os_error *err;

    /* zero data, incase unable to read */
    memset(data, 0, size);

    if((err=SparceFile_Find(f, addr, size, &pos))!=NULL)
        return err;

    if(pos==0)
    {
        printf("(&%s +&%08X not in SparceFile)\n", Print64(addr), size);
        return NULL;
    }

    return xosgbpb_read_atw(f, data, size, pos, &unread);
}

/*
 * Description: read next block a sparce file
 *            : NOTE a buffer is created, caller must release
 * Parameters : file handle, pointer to 64 bit address, poibter to size,
 *            : pointer to pointer data buffer
 * Returns    : error block or NULL for sucess
 */
os_error* SparceFile_ReadNext(os_fw f, bits64 *addr, bits *size, byte **buffer)
{
    int       unread;
    os_error *err = NULL;

    *size   = 0;
    *buffer = NULL;

    if((err=xosgbpb_readw(f, (byte*)&addr->low, sizeof(bits), &unread))!=NULL)
    {
        return err;
    }

    if(unread==0 && !osargs_read_eof_statusw(f))
    {
        if((err=xosgbpb_readw(f, (byte*)&addr->high,
                             sizeof(bits), &unread))!=NULL)
        {
            return err;
        }
        if((err=xosgbpb_readw(f, (byte*)size, sizeof(bits), &unread))!=NULL)
        {
            return err;
        }

        if((*buffer = malloc(*size))==NULL)
            return NULL;

        return xosgbpb_readw(f, *buffer, *size, &unread);
    }
    return NULL;
}

/*
 * Description: frees the sparefile cache
 * Parameters : none
 * Returns    : none
 */
void SparceFile_FreeCache(void)
{
    free(cache);
    cache = NULL;
}

/* EOF                                                                                            */
