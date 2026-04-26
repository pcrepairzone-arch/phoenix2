/*******************************************************************************
*                                                                              *
* Project       : DiscReader                                                   *
* Filename      : Objects.c                                                    *
* Version       : 1.11 (23-Apr-2007)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Reads E & E+ discs using code from DiscKnight                *
* Information   : Disc object checking functions                               *
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
#include "oslib/osfind.h"
#include "oslib/osfile.h"
#include "oslib/osgbpb.h"
#include "oslib/osword.h"

#include "DiscReader.h"

/* --- literals ------------------------------------------------------------- */

#define CO_BUFFER_MAX   (16*1024*1024)          /* CopyObject max buffer size */

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

/* --- statics -------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

static byte *CO_Buffer     = NULL;                  /* CopyObject buffer      */
static bits  CO_BufferSize = 0;                     /* CopyObject buffer size */

/* --- functions ------------------------------------------------------------ */

void   ShowChunks(bits sin, bits size);

/* -------------------------------------------------------------------------- */

/*
 * Description: Reads files & directories
 * Parameters : (none)
 * Returns    : TRUE if ok
 */
osbool ReadObjects(void)
{
    osbool     ok        = TRUE;
    bits       sin       = info.disc_record.root;
    bits       id        = sin>>8;
    s_objinfo  *objinf   = info.objects[id];
    s_dirinfo  dir;

    /* find root directory */
    if(objinf==NULL)
    {
        printf("* Root Directory not at given location (SIN=&%08X)", info.disc_record.root);
        safe_exit(24);
    }

    dir.sin.parent    = info.disc_record.root;
    dir.sin.self      = info.disc_record.root;
    dir.flags.backup  = info.opts.backup && strcmp(info.backup.source, "$")==0;
    dir.size          = 0;
    dir.data          = NULL;
    strcpy(dir.path, "$");

    if(objinf != NULL)
    {
        info.locations.root     = SinAddr(info.disc_record.root);
        objinf->directory       = dir.sin.self>>8;
        objinf->status.used     = TRUE;
        objinf->status.shared   = dir.sin.self!=0;
        objinf->status.unshared = dir.sin.self==0;
        objinf->status.checked  = TRUE;

        Info("Root directory at &%s\n", SecAddrToString(info.locations.root));
        printf("Reading directory structure\n");

        if(!ReadDirectory(&dir))
            ok = FALSE;

        printf("\n");

        if(info.opts.show.stats)
            AddObjectStat(dir.size, TRUE);
    }

    /* free root directory */
    free(dir.data);

    return ok;
}

/*
 * Description: return the sector address for a SIN number
 * Parameters : SIN
 * Returns    : sector address
 */
bits SinAddr(bits sin)
{
    bits       id       = sin>>8;
    s_usedinfo *usedinf = GetUsedInfo(id);
    bits       secaddr;

    if(id>=info.stats.size.ids || usedinf==NULL)
        return 0;

    /* special case for sin2 - skip boot block and return map */
    if(id==2 && usedinf->next)
        usedinf = usedinf->next;

    secaddr = MapAddr(usedinf->zone, usedinf->start);

    if((sin & 0xFF) > 1)
    {
        secaddr += ((sin & 0xFF)-1) << info.disc_record.share_size;
    }

    return secaddr;
}

/*
 * Description: Loads an object using sin information
 * Parameters : buffer (of cortrect size), size, sin
 * Returns    : TRUE if ok
 */
osbool LoadObject(byte *buffer, bits size, bits sin)
{
    bits id = sin>>8;

    if(id>=info.stats.size.ids || info.objects[id]==NULL)
        return FALSE;

    if((sin & 0xFF) > 1)
    {
        /* read shared fragment */
        ReadSectors(buffer, SinAddr(sin), size);
        return TRUE;
    }
    else
    {
        s_usedinfo *usedinf = GetUsedInfo(id);

        while(size>0 && usedinf)
        {
            bits len = usedinf->len << info.disc_record.log2bpmb;

            /* limit read to the size data unread */
            if(len > size)
                len = size;

            ReadSectors(buffer, MapAddr(usedinf->zone, usedinf->start), len);

            buffer += len;
            size   -= len;
            usedinf = usedinf->next;
        }
        return size==0;
    }
}

/*
 * Description: Saves an object using sin information
 * Parameters : buffer (of cortrect size), size, sin
 * Returns    : TRUE if ok
 */
osbool SaveObject(byte *buffer, bits size, bits sin)
{
    bits id = sin>>8;

    if(id>=info.stats.size.ids || info.objects[id]==NULL)
        return FALSE;

    if((sin & 0xFF) > 1)
    {
        /* write shared fragment */
        WriteSectors(buffer, SinAddr(sin), size);
    }
    else
    {
        s_usedinfo *usedinf = GetUsedInfo(id);

        while(size>0 && usedinf)
        {
            bits len = usedinf->len<<info.disc_record.log2bpmb;

            /* limit write to the size data unwritten */
            if(len > size)
                len = size;

            WriteSectors(buffer, MapAddr(usedinf->zone, usedinf->start), len);

            buffer += len;
            size   -= len;
            usedinf = usedinf->next;
        }
    }
    return TRUE;
}

/*
 * Description: Extracts an object to the Undo file using sin information
 * Parameters : buffer (of cortrect size), size, sin
 * Returns    : TRUE if ok
 */
osbool ExtractObject(byte *buffer, bits size, bits sin)
{
    bits id = sin>>8;

    if(id>=info.stats.size.ids || info.objects[id]==NULL)
        return FALSE;

    if((sin & 0xFF) > 1)
    {
        /* write shared fragment */
        Extract(buffer, TRUE, SinAddr(sin), size);
    }
    else
    {
        s_usedinfo *usedinf = GetUsedInfo(id);

        while(size>0 && usedinf)
        {
            bits len = usedinf->len<<info.disc_record.log2bpmb;

            /* limit write to the size data unwritten */
            if(len > size)
                len = size;

            Extract(buffer, TRUE, MapAddr(usedinf->zone, usedinf->start), len);

            buffer += len;
            size   -= len;
            usedinf = usedinf->next;
        }
    }
    return TRUE;
}

/*
 * Description: copy object to a file
 * Parameters : sin, destination filename, and object atrributes
 * Returns    : (none)
 */
osbool CopyObject(bits sin, char *filename, bits size, bits loadaddr, bits execaddr)
{
    s_usedinfo *usedinf   = GetUsedInfo(sin>>8);
    os_error   *err       = NULL;
    byte       *buffer    = NULL;
    os_fw       f         = 0;

    if(usedinf==NULL)
        return FALSE;

    if(info.opts.verbose)
        printf("+ Creating %s\n", filename);

   /* check statically allocated buffer exists */
    if(CO_Buffer==NULL)
    {
        CO_BufferSize = info.stats.size.zone<<info.disc_record.log2bpmb;

        if(CO_BufferSize>CO_BUFFER_MAX)
            CO_BufferSize = CO_BUFFER_MAX;

        /*
         * attempt to get large enough buffer for the file or a whole zone
         * which ever is the smaller, if this fails, try 4MB and 1MB
         */
        if((CO_Buffer=(byte*)malloc(CO_BufferSize))==NULL)
        {
            CO_BufferSize = 4096*1024;

            if((CO_Buffer=(byte*)malloc(CO_BufferSize))==NULL)
            {
                CO_BufferSize = 1024*1024;
                CO_Buffer     = (byte*)malloc(CO_BufferSize);
            }
        }
        if(CO_Buffer==NULL)
        {
            Error("Unable to claim memory to backup file %s\r\n", filename);
            return FALSE;
        }
    }

    /* check if buffer is large enough for whole file, and file is not fragmented */
    if(CO_BufferSize>=size && usedinf->next==NULL)
    {
        /* optimised routine for small shared files */
        ReadSectors(CO_Buffer, SinAddr(sin), size);
        if((err=xosfile_save(filename, loadaddr, execaddr, CO_Buffer, CO_Buffer+size))!=NULL)
            Error("Unable to save backup file '%s' (%s)\r\n", filename, err->errmess);

        return err==NULL;
    }

    /* write file in chunks of buffer or zone size */

    if((err=xosfile_create(filename, loadaddr, execaddr, size))!=NULL)
    {
        Error("Unable to create file '%s': %s\r\n", filename, err->errmess);
        return FALSE;
    }

    if((err=xosfind_openupw(osfind_NO_PATH, filename, NULL, &f))!=NULL || f==0)
    {
        Error("Unable to open file '%s': %s\r\n", filename, err? err->errmess : "handle is 0");
        return FALSE;
    }

    do
    {
        bits chunkaddr = MapAddr(usedinf->zone, usedinf->start);
        bits chunksize = usedinf->len << info.disc_record.log2bpmb;
        bits offset    = 0;

        do
        {
            int  unwritten = 0;
            bits len       = chunksize;

            if(len>CO_BufferSize) len = CO_BufferSize;
            if(len>size)          len = size;

            ReadSectors(CO_Buffer, chunkaddr+(offset>>info.disc_record.log2secsize), len);

            if((err=xosgbpb_writew(f, CO_Buffer, len, &unwritten))!=NULL)
            {
                Error("Unable to write file '%s': %s\r\n", filename, err->errmess);
                xosfind_closew(f);
                return FALSE;
            }

            offset    += len;
            chunksize -= len;
            size      -= len;
        } while(chunksize>0 && size>0);

        usedinf = usedinf->next;

    } while(size>0 && usedinf);

    xosfind_closew(f);
    free(buffer);

    return TRUE;
}

/*
 * Description: Adds stats for an object
 * Parameters : object size, is a directory
 * Returns    : (none)
 */
void AddObjectStat(bits size, osbool isdir)
{
    if(isdir)
    {
        info.stats.count.directories++;
        INC64(info.stats.space.directories, size);
    }
    else
    {
        info.stats.count.files++;
        INC64(info.stats.space.files, size);
    }
}

/*
 * Description: Show chunks making up object
 * Parameters : SIN of object, size of object
 * Returns    : (none)
 */
void ShowChunks(bits sin, bits size)
{
    s_usedinfo *usedinf = GetUsedInfo(sin>>8);

    if(usedinf==NULL)
        return;

    if((sin&0xFF)!=0)
    {
        bits share_start   = (sin & 0xFF)-1;
        bits share_end     = share_start + ((size-1) >> info.stats.size.l2share);

        printf("    Chunk at &%s +&%08X sharing sectors %u to %u\n",
               SecAddrToString(MapAddr(usedinf->zone, usedinf->start)),
               usedinf->len << info.disc_record.log2bpmb,
               share_start  << info.disc_record.share_size,
               share_end    << info.disc_record.share_size);

        usedinf = usedinf->next;
    }

    while(usedinf)
    {
        printf("    Chunk at &%s +&%08X\n",
               SecAddrToString(MapAddr(usedinf->zone, usedinf->start)),
               usedinf->len << info.disc_record.log2bpmb);

        usedinf = usedinf->next;
    }
}

/* EOF */
