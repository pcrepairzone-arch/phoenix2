/*******************************************************************************
*                                                                              *
* Project       : DiscReader                                                   *
* Filename      : DiscReader.c                                                 *
* Version       : 1.13 (20-Aug-2017)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Reads E & E+ discs using code from DiscKnight                *
* Information   : Main C code file                                             *
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "oslib/osfile.h"
#include "oslib/osfind.h"

#include "DiscReader.h"
#include "BigFile.h"
#include "SparceFile.h"

/* --- literals ------------------------------------------------------------- */

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

/* --- statics -------------------------------------------------------------- */

/* --- globals -------------------------------------------------------------- */

s_info info;

/* --- functions ------------------------------------------------------------ */

void Finalise(void);
void Version(void);
void Syntax(void);
void ShowTables(void);
void ShowStats(void);

/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    int arg;

    info.opts.help                        = (argc==1);
    info.opts.verbose                     = FALSE;
    info.opts.file                        = FALSE;
    info.opts.extract                     = FALSE;
    info.opts.show.map                    = FALSE;
    info.opts.show.files                  = FALSE;
    info.opts.show.tables                 = FALSE;
    info.opts.show.stats                  = FALSE;

    info.file.name                        = NULL;
    info.file.sparce                      = FALSE;
    info.file.h                           = 0;

    info.backup.source                    = NULL;
    info.backup.dest                      = NULL;
    info.backup.matchlen                  = 0;

    info.disc.fs_name                     = NULL;
    info.disc.drive                       = -1;
    info.disc.byteSWI                     = 0;
    info.disc.sectorSWI                   = 0;
    info.disc.byte64SWI                   = 0;

    info.extract_file                     = NULL;

    memset(&info.disc_record, 0, DISC_RECORD_SIZE);

    info.stats.size.zone                  = 0;
    info.stats.size.ids                   = 0;
    info.stats.size.ids_pz                = 0;
    info.stats.size.lfau                  = 0;
    info.stats.size.l2share               = 0;
    info.stats.size.share                 = 0;
    info.stats.size.minimum               = 0;
    info.stats.size.map                   = 0;
    info.stats.count.objects              = 0;
    info.stats.count.free                 = 0;
    info.stats.count.files                = 0;
    info.stats.count.directories          = 0;
    info.stats.count.defects              = 0;
    info.stats.count.excess               = 0;
    info.stats.count.fragmented.all       = 0;
    info.stats.count.fragmented.necessary = 0;
    ZERO64(info.stats.space.used);
    ZERO64(info.stats.space.free);
    ZERO64(info.stats.space.files);
    ZERO64(info.stats.space.directories);
    ZERO64(info.stats.space.defects);
    ZERO64(info.stats.space.rounding);
    ZERO64(info.stats.space.excess);

    info.lba0                             = NULL;
    info.bootblock                        = NULL;
    info.map[0]                           = NULL;
    info.map[1]                           = NULL;
    info.objects                          = NULL;
    info.freelist                         = NULL;


    /* print version number and arguments */
    Version();

    printf("Arguments: ");
    for(arg=1; arg<argc; arg++)
        printf("%s ", argv[arg]);
    printf("\n");

    for(arg=1; arg<argc; arg++)
    {
        if(argv[arg][0] == '-')
        {
            switch(argv[arg][1])
            {
                case 'b':
                    info.opts.backup     = TRUE;
                    info.backup.source   = strchr(argv[++arg], '$');
                    info.backup.dest     = argv[++arg];
                    info.backup.matchlen = (bits)strlen(info.backup.source);
                    break;

                case 'd':
                    info.opts.file = TRUE;
                    info.file.name = argv[++arg];
                    break;

                case 'h':
                case '?':
                    info.opts.help = TRUE;
                    break;

                case 'l':
                    info.opts.show.files = TRUE;
                    break;

                case 'm':
                    info.opts.show.map = TRUE;
                    break;

                case 's':
                    info.opts.show.stats = TRUE;
                    break;

                case 't':
                    info.opts.show.tables = TRUE;
                    break;

                case 'v':
                    info.opts.verbose = TRUE;
                    break;

                case 'x':
                    info.opts.extract = TRUE;
                    info.extract_file = argv[++arg];
                    break;

                default:
                    break;
            }
        }
        else
        {
            /*  unflagged parameter */
            if(info.disc.fs_name==NULL)
                info.disc.fs_name = argv[arg];
            else
                info.disc.drive   = atoi(argv[arg]);
        }
    }

    if(info.opts.help ||
      ((info.disc.fs_name==NULL || info.disc.drive==-1) && !info.opts.file))
    {
        Syntax();
        return 0;
    }

    if(!Initialise())
        return 1;

    ReadBootBlock();
    ReadMap();
    ReadObjects();
    if(info.opts.show.tables) ShowTables();
    if(info.opts.show.stats)  ShowStats();

    printf("\n---------------------------------------------\n");

    /* tidy up */
    Finalise();

    return 0;
}

/*
 * Description: exit cleanly
 * Parameters : none
 * Returns    : none
 */
void safe_exit(int code)
{
    Finalise();
    exit(code);
}

/*
 * Description: Tidies up after program
 * Parameters : none
 * Returns    : none
 */
void Finalise(void)
{
    if(info.opts.file)
        BigFile_Close(info.file.h);

    SparceFile_FreeCache();

    free(info.lba0);
    free(info.map[0]);
    free(info.map[1]);

    if(info.objects)
    {
        bits id;

        for(id=0; id<info.stats.size.ids; id++)
        {
            s_objinfo  *objinf  = info.objects[id];

            if(objinf)
            {
                while(objinf->chunks)
                {
                    s_usedinfo *usedinf = objinf->chunks->next;
                    free(objinf->chunks);
                    objinf->chunks      = usedinf;
                }
                free(objinf->shared);
                free(objinf);
            }
        }
        free(info.objects);
    }

    while(info.freelist)
    {
        s_freeinfo *freeinf = info.freelist->prev;
        free(info.freelist);
        info.freelist       = freeinf;
    }
}

/*
 * Description: Displays program version
 * Parameters : none
 * Returns    : none
 */
void Version(void)
{
    printf(PROGNAME" "VERSION"\n\n");
}

/*
 * Description: Displays program syntax and help message
 * Parameters : none
 * Returns    : none
 */
void Syntax(void)
{
    printf("Copyright (C) 2001 The ARM Club \n\n");

    printf("Reads FileCore E and E+ format hard discs, directly or from raw devices\n\n");

    printf("Usage: "PROGNAME" [options] <file system name> <disc number>\n\n");

    printf("-h or -?        Output this information\n");
    printf("-v              Verbose output\n");
    printf("-m              Show the map contents\n");
    printf("-l              Show the file list\n");
    printf("-t              Show files and free space tables\n");
    printf("-s              Show disc statistics\n");
    printf("-d <filename>   Work from raw/extracted disc image\n");
    printf("-b <src> <dest> Copy files from below source directory\n");
    printf("                to destination directory on a different disc\n");
}

/*
 * Description: Displays information depending in verbose option
 * Parameters : as for printf
 * Returns    : none
 */
void Info(const char *string, ...)
{
    if(info.opts.verbose)
    {
        va_list valist;
        va_start(valist, string);
        vprintf(string, valist);
        va_end(valist);
    }
}

/*
 * Description: Displays error information
 * Parameters : as for printf
 * Returns    : none
 */
void Error(const char *string, ...)
{
    va_list valist;
    va_start(valist, string);
    vfprintf(stderr, string, valist);
    va_end(valist);
}

/*
 * Description: Conver sector addresss to hex string
 * Parameters : sector address
 * Returns    : formatted string
 */
char* SecAddrToString(bits secaddr)
{
    static char string[17];
    sprintf(string, "%08X%08X", secaddr >> (32-info.disc_record.log2secsize),
                                secaddr << info.disc_record.log2secsize);
    return string;
}

/*
 * Description: Displays free space in tabular form
 * Parameters : none
 * Returns    : none
 */
void ShowTables(void)
{
    bits min   = info.stats.size.minimum >> info.disc_record.log2bpmb;
    bits max   = info.stats.size.zone;
    bits low   = min;
    bits high  = min+1;
    bits shift = 0;

    /* find nearest power of 2 to minimum size */
    while((1u<<shift)<=high)
        shift+=2;

    printf("\nDisc Object Size Distribution\n");
    printf("---------------------------------------------\n");

    /* loop for each size */
    do
    {
        bits   count   = 0;
        bits64 size;
        bits   id;

        ZERO64(size);

        /* count free space between min and max values */
        for(id=0; id<info.stats.size.ids; id++)
        {
            s_usedinfo *usedinf = GetUsedInfo(id);

            while(usedinf)
            {
                if(usedinf->len>=low && usedinf->len<high)
                {
                    count++;
                    INC64(size, usedinf->len << info.disc_record.log2bpmb);
                }
                usedinf = usedinf->next;
            }
        }

        if(low==min)
        {
            printf("  Minimum  %5s   : ", Size32(low<<info.disc_record.log2bpmb));
        }
        else if(low==max)
        {
            printf("  Maximum  %5s   : ", Size32(low<<info.disc_record.log2bpmb));
        }
        else
        {
            /* NOTE Size32 uses a static string, so cant use twice as func argument) */
            printf("  %5s to ", Size32(low<<info.disc_record.log2bpmb));
            printf("%5s   : ",  Size32(high<<info.disc_record.log2bpmb));
        }

        printf("%6u   %5s  %6s\n", count, Size64(size), Percent64(size, info.stats.space.used));

        /* calculate next bin size, increasing by 4x */
        low    = high;
        high   = 1u<<shift;
        shift += 2;

        if(low!=max && high>max)
            high = max;

    } while(low<=max);

    printf("\nDisc Free Space Size Distribution\n");
    printf("---------------------------------------------\n");

    low   = min;
    high  = min+1;
    shift = 0;

    /* find nearest power of 2 to minimum size */
    while((1u<<shift)<=high)
        shift+=2;

    /* loop for each size */
    do
    {
        s_freeinfo *freeinf = info.freelist;
        bits        count   = 0;
        bits64      size;

        ZERO64(size);

        /* count free space between min and max values */
        while(freeinf)
        {
            if(freeinf->len>=low && freeinf->len<high)
            {
                count++;
                INC64(size, freeinf->len << info.disc_record.log2bpmb);
            }
            freeinf = freeinf->prev;
        }

        if(low==min)
        {
            printf("  Minimum  %5s   : ", Size32(low<<info.disc_record.log2bpmb));
        }
        else if(low==max)
        {
            printf("  Maximum  %5s   : ", Size32(low<<info.disc_record.log2bpmb));
        }
        else
        {
            /* NOTE Size32 uses a static string, so cant use twice as func argument) */
            printf("  %5s to ", Size32(low<<info.disc_record.log2bpmb));
            printf("%5s   : ",  Size32(high<<info.disc_record.log2bpmb));
        }

        printf("%6u   %5s  %6s\n", count, Size64(size), Percent64(size, info.stats.space.free));

        /* calculate next bin size, increasing by 4x */
        low    = high;
        high   = 1u<<shift;
        shift += 2;

        if(low!=max && high>max)
            high = max;

    } while(low<=max);
}


/*
 * Description: Displays disc statistics gathered by fixing process
 * Parameters : none
 * Returns    : none
 */
void ShowStats(void)
{
    bits64 mapsize;
    bits64 wasted;

    mapsize.low  = info.stats.size.map*2;
    mapsize.high = 0;

    wasted = Sub64(Sub64(Sub64(Sub64(info.stats.space.used, info.stats.space.files),
                                                            info.stats.space.directories),
                                                            info.stats.space.defects),
                                                            mapsize);

    /* zero if negative */
    if((wasted.high & 1u<<31) != 0)
        ZERO64(wasted);

    printf("\nDisc Statistics\n");
    printf("---------------------------------------------\n");

    printf("  Disc size        : %6s   %5s  %6s\n",   "", Size64(info.disc_record.disc_size),
                                                      Percent64(info.disc_record.disc_size, info.disc_record.disc_size));

    printf("  Objects          : %6u   %5s  %6s\n",   info.stats.count.objects, Size64(info.stats.space.used),
                                                      Percent64(info.stats.space.used, info.disc_record.disc_size));

    printf("  Free areas       : %6u   %5s  %6s\n",   info.stats.count.free, Size64(info.stats.space.free),
                                                      Percent64(info.stats.space.free, info.disc_record.disc_size));

    printf("  Files            : %6u   %5s  %6s\n",   info.stats.count.files, Size64(info.stats.space.files),
                                                      Percent64(info.stats.space.files, info.disc_record.disc_size));

    printf("  Directories      : %6u   %5s  %6s\n",   info.stats.count.directories, Size64(info.stats.space.directories),
                                                      Percent64(info.stats.space.directories, info.disc_record.disc_size));

    printf("  Maps             : %6u   %5s  %6s\n",   2u, Size64(mapsize),
                                                      Percent64(mapsize, info.disc_record.disc_size));

    printf("  Defects          : %6u   %5s  %6s\n",   info.stats.count.defects, Size64(info.stats.space.defects),
                                                      Percent64(info.stats.space.defects, info.disc_record.disc_size));

    printf("  Fragmented files : %6u   %5s  %5.1f%%\n", info.stats.count.fragmented.all, "", info.stats.count.files==0 ? 0 :
                                                      100.0*(float)info.stats.count.fragmented.all/(float)info.stats.count.files);

    printf("           must be : %6u   %5s  %5.1f%%\n", info.stats.count.fragmented.necessary, "", info.stats.count.files==0 ? 0 :
                                                      100.0*(float)info.stats.count.fragmented.necessary/(float)info.stats.count.files);

    printf("  Excess fragments : %6u   %5s  %6s\n",   info.stats.count.excess, Size64(info.stats.space.excess),
                                                      Percent64(info.stats.space.excess, info.disc_record.disc_size));

    printf("  LFAU Rounding    : %6s   %5s  %6s\n",   "", Size64(info.stats.space.rounding),
                                                      Percent64(info.stats.space.rounding, info.disc_record.disc_size));

    printf("  Map Wastage      : %6s   %5s  %6s\n",   "", Size64(wasted),
                                                      Percent64(wasted, info.disc_record.disc_size));

}

/* EOF */
