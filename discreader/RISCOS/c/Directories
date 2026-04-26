/*******************************************************************************
*                                                                              *
* Project       : DiscReader                                                   *
* Filename      : Directories.c                                                *
* Version       : 1.12 (11-Apr-2017)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Reads E & E+ discs using code from DiscKnight                *
* Information   : Directory checking and manipulation function                 *
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
*                                                                              *
*******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "oslib/osfile.h"

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

osbool ReadEntry(s_dirinfo *dir, bits *entry, bits *offset);
osbool ReadSharedObject(s_dirinfo *dir, char *name, bits sin, bits size, bits isdir, bits64 *allocated);
osbool ReadNonSharedObject(s_dirinfo *dir, char *name, bits sin, bits size, bits isdir, bits64 *allocated);
osbool ReadSubDir(s_dirinfo *dir, char *name, bits sin);

/* --- Checking and fixing directory structure ------------------------------ */

/*
 * Description: Readss a directory
 *            : called recursively
 * Parameters : directiory info, containing self & parent SIN's and path
 * Returns    : TRUE if ok
 */
osbool ReadDirectory(s_dirinfo *dir)
{
    char   dirname[FILENAME_LEN];
    osbool ok      = TRUE;
    osbool done    = FALSE;
    bits   entry   = 0;
    bits   offset  = 0;

    dir->size          = DIRECTORY_NORMAL_SIZE;
    dir->data          = malloc(dir->size);
    dir->entries       = 0;

    if(dir->data==NULL)
    {
        Error("Unable to allocate memory for directory\r\n");
        safe_exit(9);
    }

    if(!LoadObject(dir->data, dir->size, dir->sin.self))
    {
        Error("Unable to load Directory %s\r\n", dir->path);
        safe_exit(10);
    }

    /* handle big format directory larger than min size */
    if(info.disc_record.filecore_ver>0)
    {
        dir->size = *(bits*)(dir->data+BIGDIR_SIZE);

        if((dir->data=realloc(dir->data,dir->size)) == NULL)
        {
            Error("\nUnable to reallocate memory for directory %s\r\n", dir->path);
            safe_exit(11);
        }
        if(!LoadObject(dir->data, dir->size, dir->sin.self))
        {
            Error("\nUnable to load Big Directory %s\r\n", dir->path);
            safe_exit(12);
        }
    } /* info.disc_record.filecore_ver>0 */

    if(info.opts.extract)
        ExtractObject(dir->data, dir->size, dir->sin.self);

    /* get directory contents for each format */
    if(info.disc_record.filecore_ver==0)
    {
        dir->first     = DIR_FIRSTENTRY;
        GetS(dir->data, dir->size+DIR_NEWDIRNAME, DIR_FILENAMELEN, dirname);
    }
    else
    {
        bits namelen   = *(bits*)(dir->data+BIGDIR_NAMELEN);
        dir->entries   = *(bits*)(dir->data+BIGDIR_ENTRIES);
        dir->namessize = *(bits*)(dir->data+BIGDIR_NAMESSIZE);
        dir->first     = BIGDIR_NAME+WHOLEWORDS(namelen+1);
        dir->nameheap  = dir->first+(dir->entries*BIGDIR_ENTRYSIZE);
        dir->backups   = dir->size+BIGDIR_ENDNAME-(dir->entries<<2);
        GetS(dir->data, BIGDIR_NAME, namelen, dirname);
    }

    if(dir->flags.backup)
    {
        char filename[PATHNAME_LEN];
            os_error *err;

        sprintf(filename, "%s%s", info.backup.dest, dir->path+info.backup.matchlen);
        if((err=xosfile_create_dir(filename, dir->entries))!=NULL)
            Error("Error creating directory '%s' (%s)\r\n", filename, err->errmess);
    }

    /* Read the entries */
    done    = FALSE;
    entry   = 0;
    offset  = dir->first;

    do
    {
        if(info.disc_record.filecore_ver==0)
            done = offset>=dir->size+DIR_LASTMARK || dir->data[offset]==0;
        else
            done = entry>=dir->entries;

        if(!done)
        {
             if(!ReadEntry(dir, &entry, &offset))
                ok = FALSE;
        }
    } while(!done);


    /* progress information */
    if(!info.opts.show.files && !(info.opts.backup && info.opts.verbose))
    {
        static bits count = 0;
        if(++count == 10)
        {
            putchar('.');
            count = 0;
        }
    }

    return ok;
}


/*
 * Description: Checks a directory entry
 * Parameters : directory structure,
 *            : pointer to entry number - updated on exit
 *            : pointer to entry offset - updated on exit
 * Returns    : TRUE if ok
 */
osbool ReadEntry(s_dirinfo *dir, bits *entry, bits *offset)
{
    char       name[FILENAME_LEN];
    osbool     ok      = TRUE;
    s_objinfo *objinf  = NULL;
    bits       size    = 0;
    bits       sin     = 0;
    bits       attrs   = 0;
    osbool     isdir   = FALSE;

    if(info.disc_record.filecore_ver==0)
    {
        GetS(dir->data, (*offset)+DIR_OBNAME, DIR_FILENAMELEN, name);
        size  = GetI(dir->data, (*offset)+DIR_LEN,         4);
        sin   = GetI(dir->data, (*offset)+DIR_INDDISCADDR, 3);
        attrs = GetI(dir->data, (*offset)+DIR_NEWDIRATTS,  1);
    }
    else
    {
        bits namelen = *(bits*)(dir->data+(*offset)+BIGDIR_OBNAMELEN);
        bits namept  = *(bits*)(dir->data+(*offset)+BIGDIR_OBNAMEPT);
        size         = *(bits*)(dir->data+(*offset)+BIGDIR_LEN);
        sin          = *(bits*)(dir->data+(*offset)+BIGDIR_INDDISCADDR);
        attrs        = *(bits*)(dir->data+(*offset)+BIGDIR_ATTS);
        GetS(dir->data, dir->nameheap+namept, namelen, name);
    }

    isdir = (attrs & 8);

    /* check object is in the map - no sin of 1 - means empty file */
    if(sin!=1 && ((sin>>8)>=info.stats.size.ids || (objinf=info.objects[sin>>8])==NULL))
    {
        printf("\n* %s %s.%s SIN=&%08X length=&%08X=%s not found in the map\n", isdir ? "Directory":"File",
               dir->path, name, sin, size, Size32(size));

        ok  = FALSE;
    }
    else
    {
        if(sin==1)
        {
            /* zero length file, only in directory, not in map */
            if(info.opts.show.files)
            {
                printf("\n  %s %s.%s SIN=&%08X length=&%08X=%s\n", isdir ? "Directory":"File",
                       dir->path, name, sin, size, Size32(size));
            }
        }
        else
        {
            bits64 allocated;
            ZERO64(allocated);
            objinf->status.used = TRUE;

            printf("\n  %s %s.%s SIN=&%08X length=&%08X=%s\n", isdir ? "Directory":"File",
                   dir->path, name, sin, size, Size32(size));

            if((sin&0xFF)==0 || (isdir && size>(objinf->chunks->len<<info.disc_record.log2bpmb)))
            {
                if(!ReadNonSharedObject(dir, name, sin, size, isdir, &allocated))
                    ok = FALSE;
            }
            else
            {
                if(!ReadSharedObject(dir, name, sin, size, isdir, &allocated))
                    ok = FALSE;
            }

            if(isdir)
            {
                /* recurse if directory */
                if(!ReadSubDir(dir, name, sin))
                    ok = FALSE;
            }
        }

        if(info.opts.show.stats)
            AddObjectStat(size, isdir);

        if(dir->flags.backup && !isdir)
        {
            char filename[FILENAME_LEN];
            bits loadaddr;
            bits execaddr;
            snprintf(filename, FILENAME_LEN, "%s%s.%s", info.backup.dest, dir->path+info.backup.matchlen, name);

            if(info.disc_record.filecore_ver==0)
            {
                loadaddr = GetI(dir->data, (*offset)+DIR_LOAD, 4);
                execaddr = GetI(dir->data, (*offset)+DIR_EXEC, 4);
            }
            else
            {
                loadaddr = *(bits*)(dir->data+(*offset)+BIGDIR_LOAD);
                execaddr = *(bits*)(dir->data+(*offset)+BIGDIR_EXEC);
            }

            if(sin!=0xFFFFFFFF)
            {
                /* copy if valid object */
                CopyObject(sin, filename, size, loadaddr, execaddr);
            }
            else
            {
                /* just create if an empty file */
                os_error *err = NULL;
                if((err=xosfile_create(filename, loadaddr, execaddr, size))!=NULL)
                    Error("Unable to create file '%s': %s\r\n", filename, err->errmess);
            }
        }
    } /* endif if sin in map */

    /* increment callers index and offset */
    (*entry)++;
    *offset += info.disc_record.filecore_ver==0 ? DIR_ENTRYSIZE : BIGDIR_ENTRYSIZE;

    return ok;
}

/*
 * Description: Reads shared object infomation
 * Parameters : directory structure
 *            : sin
 *            : size of object
 *            : pointer to allocated size - updated on exit
 * Returns    : TRUE if ok
 */
osbool ReadSharedObject(s_dirinfo *dir, char *name, bits sin, bits size, bits isdir, bits64 *allocated)
{
    /* note objinf will be valid from earlier checks */
    s_objinfo  *objinf  = info.objects[sin>>8];
    s_usedinfo *usedinf = objinf->chunks;
    bits   chunksize    = 0;
    bits   share_start  = (sin&0xFF)-1;
    bits   share_end    = share_start + ((size-1) >> info.stats.size.l2share);
    bits   excess       = 0;
    osbool ok           = TRUE;

    UNUSED(name);
    UNUSED(dir);
    UNUSED(isdir);

    /* special case for map/dir object, dont use bootblock chunk */
    if((sin>>8)==2 && usedinf->next)
        usedinf = usedinf->next;

    chunksize = usedinf->len << (info.disc_record.log2bpmb);

    /* mark object as shared */
    objinf->status.shared = TRUE;

    /* shared chunk so remaining area of allocation */
    if((share_start<<info.stats.size.l2share)<chunksize)
        allocated->low = chunksize - (share_start<<info.stats.size.l2share);

    if(info.opts.show.files)
    {
        printf("    Chunk at &%s +&%08X sharing sectors %u to %u\n",
               SecAddrToString(MapAddr(usedinf->zone, usedinf->start)),
               chunksize,
               share_start    << info.disc_record.share_size,
               ((share_end+1) << info.disc_record.share_size)-1);
    }

    /* any additional chunks in a shared object are excess */
    usedinf = usedinf->next;

    while(usedinf)
    {
        chunksize = usedinf->len << info.disc_record.log2bpmb;
        excess   += chunksize;

        if(info.opts.show.files)
        {
            printf("    Chunk at &%s +&%08X (excess)\n",
                   SecAddrToString(MapAddr(usedinf->zone, usedinf->start)), chunksize);
        }

        usedinf = usedinf->next;
    }

    if(info.opts.show.stats && excess>0)
    {
        info.stats.count.excess++;
        INC64(info.stats.space.excess, excess);
    }

    if(info.opts.show.stats)
    {
        bits rounded = ((size+info.stats.size.share-1) & ~(info.stats.size.share-1))-size;
        INC64(info.stats.space.rounding, rounded);
    }

    return ok;
}

/*
 * Description: Reads shared object information
 * Parameters : directory structure
 *            : sin
 *            : size of object
 *            : pointer to allocated size - updated on exit
 * Returns    : TRUE if ok
 */
osbool ReadNonSharedObject(s_dirinfo *dir, char *name, bits sin, bits size, bits isdir, bits64 *allocated)
{
    /* note objinf will be valid from earlier checks */
    s_objinfo  *objinf  = info.objects[sin>>8];
    s_usedinfo *usedinf = objinf->chunks;
    bits        excess  = 0;
    osbool      ok      = TRUE;

    UNUSED(name);
    UNUSED(dir);
    UNUSED(isdir);

    /* specical case for map/dir object, dont use bootblock chunk */
    if((sin>>8)==2 && usedinf->next)
        usedinf = usedinf->next;

    /* mark object as unshared */
    objinf->status.unshared = TRUE;

    if(info.opts.show.stats)
    {
        bits rounded = ((size+info.stats.size.lfau-1) & ~(info.stats.size.lfau-1))-size;
        INC64(info.stats.space.rounding, rounded);

        if(usedinf->next)
        {
            info.stats.count.fragmented.all++;
            /* check for fragmented and larger than zone size */
            if(size>(info.stats.size.zone<<info.disc_record.log2bpmb))
                info.stats.count.fragmented.necessary++;
        }
    }

    /* add together all fragments */
    while(usedinf)
    {
        bits chunksize = usedinf->len << info.disc_record.log2bpmb;

        /* check for excess fragments */
        if((allocated->high>0 || allocated->low>size) && (sin&0xFF)==0)
            excess    += chunksize;

        Inc64(allocated, chunksize);

        if(info.opts.show.files)
        {
            printf("    Chunk at &%s +&%08X%s\n",
                   SecAddrToString(MapAddr(usedinf->zone, usedinf->start)), chunksize,
                   excess>0 ? " (excess)":"");
        }

        usedinf = usedinf->next;
    }

    if(info.opts.show.stats && excess>0)
    {
        info.stats.count.excess++;
        INC64(info.stats.space.excess, excess);
    }

    return ok;
}

/*
 * Description: Read a sub directory entry
 * Parameters : directory structure
 *            : entry number
 *            : offset to entry
 *            : sin of entry
 *            : name of entry
 * Returns    : TRUE if ok
 */
osbool ReadSubDir(s_dirinfo *dir, char *name, bits sin)
{
    osbool    ok   = TRUE;
    s_dirinfo dir2;

    dir2.data          = NULL;
    dir2.sin.self      = sin;
    dir2.sin.parent    = dir->sin.self;
    dir2.flags.backup  = dir->flags.backup;

    snprintf(dir2.path, PATHNAME_LEN, "%s.%s", dir->path, name);

    if(info.opts.backup && !dir->flags.backup)
        dir2.flags.backup = stricoll(dir2.path, info.backup.source)==0 && strlen(dir2.path)<FILENAME_LEN;

    if(!ReadDirectory(&dir2))
        ok = FALSE;

    free(dir2.data);

    return ok;
}

/* EOF */
