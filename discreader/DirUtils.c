/*******************************************************************************
*                                                                              *
* Project       : DiscReader                                                   *
* Filename      : DirUtils.c                                                   *
* Version       : 1.11 (23-Apr-2007)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Checks and fixes E & E+ format filecore discs                *
* Information   : Directory checking and manipulation function                 *
*                                                                              *
********************************************************************************
* Change Log:                                                                  *
*   00                                                                           *
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
#include "oslib/osword.h"
#include "oslib/osfile.h"
#include "oslib/territory.h"

#include "DiscReader.h"

/* --- literals ------------------------------------------------------------- */

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

/* --- statics -------------------------------------------------------------- */

/* collation indexes for each char used by filename_cmp */
static int fn_collate[256] =
{
    /* [0] */     -1,
    /* [1] */     0,
    /* [2] */     0,
    /* [3] */     0,
    /* [4] */     0,
    /* [5] */     0,
    /* [6] */     0,
    /* [7] */     0,
    /* [8] */     0,
    /* [9] */     0,
    /* [10] */    0,
    /* [11] */    0,
    /* [12] */    0,
    /* [13] */    0,
    /* [14] */    0,
    /* [15] */    0,
    /* [16] */    0,
    /* [17] */    0,
    /* [18] */    0,
    /* [19] */    0,
    /* [20] */    0,
    /* [21] */    0,
    /* [22] */    0,
    /* [23] */    0,
    /* [24] */    0,
    /* [25] */    0,
    /* [26] */    0,
    /* [27] */    0,
    /* [28] */    0,
    /* [29] */    0,
    /* [30] */    0,
    /* [31] */    0,
    /* [32] */    0,
    /* [33] */    1,      /* '!' */
    /* [34] */    0,      /* '"' */
    /* [35] */    0,      /* '#' */
    /* [36] */    0,      /* '$' */
    /* [37] */    0,      /* '%' */
    /* [38] */    0,      /* '&' */
    /* [39] */    2,      /* ''' */
    /* [40] */    3,      /* '(' */
    /* [41] */    4,      /* ')' */
    /* [42] */    0,      /* '*' */
    /* [43] */    5,      /* '+' */
    /* [44] */    6,      /* ',' */
    /* [45] */    7,      /* '-' */
    /* [46] */    0,      /* '.' */
    /* [47] */    8,      /* '/' */
    /* [48] */    9,      /* '0' */
    /* [49] */    10,     /* '1' */
    /* [50] */    11,     /* '2' */
    /* [51] */    12,     /* '3' */
    /* [52] */    13,     /* '4' */
    /* [53] */    14,     /* '5' */
    /* [54] */    15,     /* '6' */
    /* [55] */    16,     /* '7' */
    /* [56] */    17,     /* '8' */
    /* [57] */    18,     /* '9' */
    /* [58] */    0,      /* ':' */
    /* [59] */    19,     /* ';' */
    /* [60] */    20,     /* '<' */
    /* [61] */    21,     /* '=' */
    /* [62] */    22,     /* '>' */
    /* [63] */    23,     /* '?' */
    /* [64] */    0,      /* '@' */
    /* [65] */    24,     /* 'A' */
    /* [66] */    26,     /* 'B' */
    /* [67] */    28,     /* 'C' */
    /* [68] */    30,     /* 'D' */
    /* [69] */    32,     /* 'E' */
    /* [70] */    34,     /* 'F' */
    /* [71] */    36,     /* 'G' */
    /* [72] */    38,     /* 'H' */
    /* [73] */    40,     /* 'I' */
    /* [74] */    42,     /* 'J' */
    /* [75] */    44,     /* 'K' */
    /* [76] */    46,     /* 'L' */
    /* [77] */    48,     /* 'M' */
    /* [78] */    50,     /* 'N' */
    /* [79] */    52,     /* 'O' */
    /* [80] */    54,     /* 'P' */
    /* [81] */    56,     /* 'Q' */
    /* [82] */    58,     /* 'R' */
    /* [83] */    60,     /* 'S' */
    /* [84] */    62,     /* 'T' */
    /* [85] */    64,     /* 'U' */
    /* [86] */    66,     /* 'V' */
    /* [87] */    68,     /* 'W' */
    /* [88] */    70,     /* 'X' */
    /* [89] */    72,     /* 'Y' */
    /* [90] */    76,     /* 'Z' */
    /* [91] */    77,     /* '[' */
    /* [92] */    0,      /* '\' */
    /* [93] */    78,     /* ']' */
    /* [94] */    0,      /* '^' */
    /* [95] */    79,     /* '_' */
    /* [96] */    80,     /* '`' */
    /* [97] */    24,     /* 'a' */
    /* [98] */    26,     /* 'b' */
    /* [99] */    28,     /* 'c' */
    /* [100] */   30,     /* 'd' */
    /* [101] */   32,     /* 'e' */
    /* [102] */   34,     /* 'f' */
    /* [103] */   36,     /* 'g' */
    /* [104] */   38,     /* 'h' */
    /* [105] */   40,     /* 'i' */
    /* [106] */   42,     /* 'j' */
    /* [107] */   44,     /* 'k' */
    /* [108] */   46,     /* 'l' */
    /* [109] */   48,     /* 'm' */
    /* [110] */   50,     /* 'n' */
    /* [111] */   52,     /* 'o' */
    /* [112] */   54,     /* 'p' */
    /* [113] */   56,     /* 'q' */
    /* [114] */   58,     /* 'r' */
    /* [115] */   60,     /* 's' */
    /* [116] */   62,     /* 't' */
    /* [117] */   64,     /* 'u' */
    /* [118] */   66,     /* 'v' */
    /* [119] */   68,     /* 'w' */
    /* [120] */   70,     /* 'x' */
    /* [121] */   72,     /* 'y' */
    /* [122] */   76,     /* 'z' */
    /* [123] */   81,     /* '{' */
    /* [124] */   0,      /* '|' */
    /* [125] */   82,     /* '}' */
    /* [126] */   83,     /* '~' */
    /* [127] */   0,
    /* [128] */   84,
    /* [129] */   86,
    /* [130] */   86,
    /* [131] */   87,
    /* [132] */   88,
    /* [133] */   90,
    /* [134] */   90,
    /* [135] */   91,
    /* [136] */   92,
    /* [137] */   93,
    /* [138] */   94,
    /* [139] */   95,
    /* [140] */   96,
    /* [141] */   97,
    /* [142] */   98,
    /* [143] */   99,
    /* [144] */   100,
    /* [145] */   101,
    /* [146] */   102,
    /* [147] */   103,
    /* [148] */   104,
    /* [149] */   105,
    /* [150] */   106,
    /* [151] */   107,
    /* [152] */   108,
    /* [153] */   109,
    /* [154] */   110,
    /* [155] */   110,
    /* [156] */   112,
    /* [157] */   113,
    /* [158] */   114,
    /* [159] */   114,
    /* [160] */   116,
    /* [161] */   117,
    /* [162] */   118,
    /* [163] */   119,
    /* [164] */   120,
    /* [165] */   121,
    /* [166] */   122,
    /* [167] */   123,
    /* [168] */   124,
    /* [169] */   125,
    /* [170] */   126,
    /* [171] */   127,
    /* [172] */   128,
    /* [173] */   129,
    /* [174] */   130,
    /* [175] */   131,
    /* [176] */   132,
    /* [177] */   133,
    /* [178] */   134,
    /* [179] */   135,
    /* [180] */   136,
    /* [181] */   137,
    /* [182] */   138,
    /* [183] */   139,
    /* [184] */   140,
    /* [185] */   141,
    /* [186] */   142,
    /* [187] */   143,
    /* [188] */   144,
    /* [189] */   145,
    /* [190] */   146,
    /* [191] */   147,
    /* [192] */   148,
    /* [193] */   150,
    /* [194] */   152,
    /* [195] */   154,
    /* [196] */   156,
    /* [197] */   158,
    /* [198] */   160,
    /* [199] */   162,
    /* [200] */   164,
    /* [201] */   166,
    /* [202] */   168,
    /* [203] */   170,
    /* [204] */   172,
    /* [205] */   174,
    /* [206] */   176,
    /* [207] */   178,
    /* [208] */   180,
    /* [209] */   182,
    /* [210] */   184,
    /* [211] */   186,
    /* [212] */   188,
    /* [213] */   190,
    /* [214] */   192,
    /* [215] */   194,
    /* [216] */   196,
    /* [217] */   198,
    /* [218] */   200,
    /* [219] */   202,
    /* [220] */   204,
    /* [221] */   206,
    /* [222] */   208,
    /* [223] */   209,
    /* [224] */   148,
    /* [225] */   150,
    /* [226] */   152,
    /* [227] */   154,
    /* [228] */   156,
    /* [229] */   158,
    /* [230] */   160,
    /* [231] */   162,
    /* [232] */   164,
    /* [233] */   166,
    /* [234] */   168,
    /* [235] */   170,
    /* [236] */   172,
    /* [237] */   174,
    /* [238] */   176,
    /* [239] */   178,
    /* [240] */   180,
    /* [241] */   182,
    /* [242] */   184,
    /* [243] */   186,
    /* [244] */   188,
    /* [245] */   190,
    /* [246] */   192,
    /* [247] */   210,
    /* [248] */   196,
    /* [249] */   198,
    /* [250] */   200,
    /* [251] */   202,
    /* [252] */   204,
    /* [253] */   206,
    /* [254] */   208,
    /* [255] */   73
};

/* directory structure holder for SortDirectory and CompareDirEntries use */
static s_dirinfo *CompareDir = NULL;

/* --- globals -------------------------------------------------------------- */

/* --- functions ------------------------------------------------------------ */

int CompareDirEntries(const void *entry1, const void *entry2);
void UniqueName(s_dirinfo *dir, bits current, char *name);

/* --- Directory utility functions ----------------------------------------- */

/*
 * Description: Loads a directory
 * Parameters : directory structure
 * Returns    : TRUE if sucessfull
 */
osbool LoadDirectory(s_dirinfo *dir)
{
    dir->size = DIRECTORY_NORMAL_SIZE;
    dir->data = malloc(dir->size);

    if(dir->data==NULL)
    {
        Error("Unable to allocate memory for directory\r\n");
        safe_exit(13);
    }

    if(!LoadObject(dir->data, dir->size, dir->sin.self))
    {
        printf("* Unable to load directory %s (SIN &%08X)\n", dir->path, dir->sin.self);
        return FALSE;
    }

    if(info.disc_record.filecore_ver==0)
    {
        dir->first  = DIR_FIRSTENTRY;
    }
    else
    {
        bits namelen   = *(bits*)(dir->data+BIGDIR_NAMELEN);
        dir->size      = *(bits*)(dir->data+BIGDIR_SIZE);
        dir->namessize = *(bits*)(dir->data+BIGDIR_NAMESSIZE);
        dir->entries   = *(bits*)(dir->data+BIGDIR_ENTRIES);
        dir->first     = BIGDIR_NAME+WHOLEWORDS(namelen+1);
        dir->nameheap  = dir->first+(dir->entries*BIGDIR_ENTRYSIZE);
        dir->backups   = dir->size+BIGDIR_ENDNAME-(dir->entries<<2);

        if(dir->size<=DIRECTORY_MAX_SIZE && (dir->size&(DIRECTORY_EXTENSION_SIZE-1))==0)
        {
            /* load rest of large diretory */
            if(dir->size>DIRECTORY_NORMAL_SIZE)
            {
                if((dir->data=realloc(dir->data, dir->size)) == NULL)
                {
                    Error("\nUnable to reallocate memory for directory\r\n");
                    safe_exit(15);
                }
                if(!LoadObject(dir->data, dir->size, dir->sin.self))
                {
                    Error("\nUnable to load Big Directory %s\r\n", dir->path);
                    safe_exit(16);
                }
            }
        }
        else
        {
            return FALSE;
        }
    } /* if filecore_ver */

    return TRUE;
}

/*
 * Description: Checks if object is a directory
 * Parameters : sin, name (updated on exit),
 *            : pointer to size (updated on exit)
 *            : pointer to parent (updated on exit)
 * Returns    : TRUE if directory
 */
osbool IsDirectory(bits sin, char *name, bits *size, bits *parent)
{
    osbool    isdir = TRUE;
    s_dirinfo dir;

    memset(&dir, 0, sizeof(s_dirinfo));
    dir.sin.self      = sin;
    dir.sin.parent    = 0;
    dir.size          = DIRECTORY_NORMAL_SIZE;
    dir.data          = malloc(dir.size);

    if(dir.data==NULL)
    {
        Error("Unable to allocate memory for directory\r\n");
        safe_exit(17);
    }

    if(!LoadObject(dir.data, dir.size, dir.sin.self))
    {
        Error("Unable to load Object SIN &%08X\r\n", dir.sin.self);
        safe_exit(18);
    }

    /* check for likely directory contents */
    if(info.disc_record.filecore_ver==0)
    {
        bits startname = GetI(dir.data, DIR_STARTNAME,             4);
        bits endname   = GetI(dir.data, dir.size+DIR_ENDNAME,      4);
        *parent        = GetI(dir.data, dir.size+DIR_NEWDIRPARENT, 3);

        GetS(dir.data, dir.size+DIR_NEWDIRNAME,  DIR_FILENAMELEN, name);

        if((startname!=DIRECTORY_HUGO && startname!=DIRECTORY_NICK) || endname!=startname)
             isdir = FALSE;
    }
    else /* filecore_ver != 0 */
    {
        bits version   = GetI(dir.data, BIGDIR_VERSION, 3);
        bits startname = *(bits*)(dir.data+BIGDIR_STARTNAME);
        bits namelen   = *(bits*)(dir.data+BIGDIR_NAMELEN);
        dir.size       = *(bits*)(dir.data+BIGDIR_SIZE);
        *parent        = *(bits*)(dir.data+BIGDIR_PARENT);


        if(version!=0 || startname!=DIRECTORY_BIG1 || namelen<1 || namelen>255 ||
           dir.size>DIRECTORY_MAX_SIZE || (dir.size&(DIRECTORY_EXTENSION_SIZE-1))!=0)
        {
            isdir = FALSE;
        }
        else
        {
            bits endname;

            GetS(dir.data, BIGDIR_NAME, namelen, name);

            /* load rest of large diretory */
            if(dir.size>DIRECTORY_NORMAL_SIZE)
            {
                if((dir.data=realloc(dir.data,dir.size)) == NULL)
                {
                    Error("\nUnable to reallocate memory for big directory\r\n");
                    safe_exit(19);
                }
                if(!LoadObject(dir.data, dir.size, dir.sin.self))
                {
                    Error("\nUnable to load Big Directory %s\r\n", dir.path);
                    safe_exit(20);
                }
            }

            endname = *(bits*)(dir.data+dir.size+BIGDIR_ENDNAME);

            if(endname!=DIRECTORY_BIG2)
                isdir = FALSE;
        }
    } /* if filecore_ver */

    *size = isdir ? dir.size : DIRECTORY_NORMAL_SIZE;

    free(dir.data);

    return isdir;
}

/*
 * Description: Clears a directory
 * Parameters : directory structure
 * Returns    : (none)
 */
// cppcheck-suppress unusedFunction
void CreateDir(s_dirinfo *dir)
{
    char *leaf = GetLeaf(dir->path);

    memset(dir->data, 0, dir->size);

    if(info.disc_record.filecore_ver==0)
    {
        bits name    = strcmp(dir->path, "$")==0  ? DIRECTORY_HUGO : DIRECTORY_NICK;
        dir->entries = 0;
        dir->first   = DIR_FIRSTENTRY;

        SetI(dir->data, DIR_STARTMASSEQ,            1, 0);
        SetI(dir->data, DIR_STARTNAME,              4, name);
        SetI(dir->data, DIR_FIRSTENTRY,             1, 0);
        SetI(dir->data, dir->size+DIR_LASTMARK,     1, 0);
        SetI(dir->data, dir->size+DIR_RESERVED,     2, 0);
        SetI(dir->data, dir->size+DIR_NEWDIRPARENT, 3, dir->sin.parent);
        SetS(dir->data, dir->size+DIR_NEWDIRTITLE, 19, leaf);
        SetS(dir->data, dir->size+DIR_NEWDIRNAME,  10, leaf);
        SetI(dir->data, dir->size+DIR_ENDMASSEQ,    1, 0);
        SetI(dir->data, dir->size+DIR_ENDNAME,      4, name);
        SetI(dir->data, dir->size+DIR_CHECKBYTE,    1, DirCheckByte(dir));
    }
    else
    {
        bits namelen   = (bits)strlen(leaf);

        dir->namessize = 0;
        dir->entries   = 0;
        dir->first     = BIGDIR_NAME+WHOLEWORDS(namelen+1);
        dir->nameheap  = dir->first;
        dir->backups   = dir->size+BIGDIR_ENDNAME;

        dir->data[BIGDIR_STARTMASSEQ]                = 0;
        SetI(dir->data, BIGDIR_VERSION,           3,   0);
        *(bits*)(dir->data+BIGDIR_STARTNAME)         = DIRECTORY_BIG1;
        *(bits*)(dir->data+BIGDIR_NAMELEN)           = namelen;
        *(bits*)(dir->data+BIGDIR_SIZE)              = dir->size;
        *(bits*)(dir->data+BIGDIR_ENTRIES)           = dir->entries;
        *(bits*)(dir->data+BIGDIR_NAMESSIZE)         = dir->namessize;
        *(bits*)(dir->data+BIGDIR_PARENT)            = dir->sin.parent;
        SetS(dir->data, BIGDIR_NAME, namelen, leaf);
        dir->data[BIGDIR_NAME+namelen]               = 13;
        *(bits*)(dir->data+dir->size+BIGDIR_ENDNAME) = DIRECTORY_BIG2;
        dir->data[dir->size+BIGDIR_ENDMASSEQ]        = 0;
        SetI(dir->data, dir->size+BIGDIR_RESERVED, 2,  0);
        dir->data[dir->size+BIGDIR_CHECKBYTE]        = DirCheckByte(dir);
    }
}
/*
 * Description: Adds directory entry
 * Parameters : directory, max allocation of directory for E+ growth
 *            : filename/sin/size/directory flag of entry,
 * Returns    : TRUE if sucessful
 */
// cppcheck-suppress unusedFunction
osbool AddEntry(s_dirinfo *dir, bits alloc, const char *filename, bits sin, bits size, osbool isdir)
{
    char name[FILENAME_LEN];
    bits offset   = dir->first;
    bits attrs    = 3 | (isdir ? DIRECTORY_ATTR_DIR_MASK:0);
    bits loadaddr = 0;
    bits execaddr = 0;

    MakeLoadExec(&loadaddr, &execaddr);

    if(info.disc_record.filecore_ver==0)
    {
        bits lastentry = dir->first;

        /* find end of the directory and insert point */
        while(lastentry<dir->size+DIR_LASTMARK && dir->data[lastentry]!=0)
        {
            GetS(dir->data, lastentry+DIR_OBNAME, DIR_FILENAMELEN, name);

            if(filename_cmp(filename, name)>0)
                offset += DIR_ENTRYSIZE;

            lastentry += DIR_ENTRYSIZE;
        }

        /* check direcrtory isn't full */
        if(lastentry>=dir->size+DIR_LASTMARK)
            return FALSE;

        /* copy up the entries */
        memmove(dir->data+offset+DIR_ENTRYSIZE, dir->data+offset, lastentry-offset);
        dir->data[lastentry+DIR_ENTRYSIZE] = 0;

        dir->entries++;

        /* make new entry */
        SetS(dir->data, offset+DIR_OBNAME,     10, filename);
        SetI(dir->data, offset+DIR_LOAD,        4, loadaddr);
        SetI(dir->data, offset+DIR_EXEC,        4, execaddr);
        SetI(dir->data, offset+DIR_LEN,         4, size);
        SetI(dir->data, offset+DIR_INDDISCADDR, 3, sin);
        SetI(dir->data, offset+DIR_NEWDIRATTS,  1, attrs);
    }
    else
    {
        int filenamelen = WHOLEWORDS(strlen(filename)+1);
        bits namept     = 0;
        osbool done     = FALSE;
        bits entry      = 0;

        /* check the directory isn't full */
        if(dir->nameheap+BIGDIR_ENTRYSIZE+dir->namessize+filenamelen>dir->backups-4)
        {
            /* extend director unless at maximum */
            if(dir->size==DIRECTORY_MAX_SIZE || dir->size+DIRECTORY_NORMAL_SIZE>alloc)
            {
                return FALSE;
            }
            else
            {
                bits dsize = dir->size + DIRECTORY_EXTENSION_SIZE;
                dir->data  = realloc(dir->data, dsize);

                if(dir->data==NULL)
                {
                    /* non fatal error */
                    printf("No memory to extend directory\n");
                    safe_exit(22);
                }

                /* update fields and copy directory tail to new location */
                memcpy(dir->data+dir->backups+DIRECTORY_EXTENSION_SIZE,
                       dir->data+dir->backups,
                       dir->size-dir->backups);
                dir->backups                   += DIRECTORY_EXTENSION_SIZE;
                dir->size                       = dsize;
                *(bits*)(dir->data+BIGDIR_SIZE) = dir->size;
            }
        }

        /* copy name heap up */
        memmove(dir->data+dir->nameheap+BIGDIR_ENTRYSIZE, dir->data+dir->nameheap, dir->namessize);
        dir->nameheap += BIGDIR_ENTRYSIZE;

        /* find insertion point */
        for(entry=0; entry<dir->entries && !done;)
        {
            bits namelen = *(bits*)(dir->data+offset+BIGDIR_OBNAMELEN);
            namept       = *(bits*)(dir->data+offset+BIGDIR_OBNAMEPT);
            GetS(dir->data, dir->nameheap+namept, namelen, name);

            if(filename_cmp(filename, name)>0)
            {
                offset += BIGDIR_ENTRYSIZE;
                entry++;
            }
            else
            {
                done    = TRUE;
            }
        }
        if(entry == dir->entries)
        {
            /* fix up new last entries name pointer */
            namept                                     = (dir->namessize+3) & ~3;
            *(bits*)(dir->data+offset+BIGDIR_OBNAMEPT) = namept;
        }

        /* copy down backup sins */
        memmove(dir->data+dir->backups-4,
                dir->data+dir->backups,
                entry<<2);

        dir->backups -= 4;

        /* fixup name heap */
        AdjustNameHeap(dir, entry, filenamelen);

        /* copy up entries */
        memmove(dir->data+offset+BIGDIR_ENTRYSIZE,
                dir->data+offset,
                (dir->entries-entry)*BIGDIR_ENTRYSIZE);

        *(bits*)(dir->data+BIGDIR_ENTRIES)  = ++dir->entries;

        /* make new entry */
        SetS(dir->data, dir->nameheap+namept, filenamelen, filename);
        filenamelen = (int)strlen(filename);
        dir->data[dir->nameheap+namept+filenamelen]   = 0x0D;

        *(bits*)(dir->data+offset+BIGDIR_LOAD)        = loadaddr;
        *(bits*)(dir->data+offset+BIGDIR_EXEC)        = execaddr;
        *(bits*)(dir->data+offset+BIGDIR_LEN)         = size;
        *(bits*)(dir->data+offset+BIGDIR_INDDISCADDR) = sin;
        *(bits*)(dir->data+offset+BIGDIR_ATTS)        = attrs;
        *(bits*)(dir->data+offset+BIGDIR_OBNAMELEN)   = filenamelen;
        *(bits*)(dir->data+offset+BIGDIR_OBNAMEPT)    = namept;
        *(bits*)(dir->data+dir->backups+(entry<<2))   = sin;
    }

    return TRUE;
}

/*
 * Description: Removes a directory entry (not in map)
 * Parameters : directory, entry number, entry offset
 * Returns    : (none)
 */
// cppcheck-suppress unusedFunction
void RemoveEntry(s_dirinfo *dir, bits entry, bits offset)
{
    if(info.disc_record.filecore_ver==0)
    {
        bits lastentry = offset;

        /* find end of the directory */
        while(dir->data[lastentry] != 0)
            lastentry += DIR_ENTRYSIZE;

        /* copy entries down */
        memmove(dir->data+offset, dir->data+offset+DIR_ENTRYSIZE, lastentry-offset-DIR_ENTRYSIZE);

        /* make end of the director */
        dir->data[lastentry-DIR_ENTRYSIZE] = 0;
    }
    else
    {
        int namelen = WHOLEWORDS((*(bits*)(dir->data+offset+BIGDIR_OBNAMELEN))+1);
        int spare;

        /* fix up name heap */
        AdjustNameHeap(dir, entry+1, -namelen);

        *(bits*)(dir->data+BIGDIR_ENTRIES) = --dir->entries;

        if(entry<dir->entries)
        {
            /* copy subsequent entries down */
            memmove(dir->data+offset,
                    dir->data+offset+BIGDIR_ENTRYSIZE,
                    (dir->entries-entry)*BIGDIR_ENTRYSIZE);
        }

        /* copy name heap down */
        memmove(dir->data+dir->nameheap-BIGDIR_ENTRYSIZE,
                dir->data+dir->nameheap,
                dir->namessize);

        dir->nameheap -= BIGDIR_ENTRYSIZE;

        /* remove backup sin and copy up preceeding*/
        memmove(dir->data+dir->backups+4, dir->data+dir->backups, entry<<2);
        dir->backups += 4;

        /* check for ability to reduce the directory size */
        spare = dir->backups - (dir->nameheap+dir->namessize);
        if(spare > 0x800)
        {
            spare = spare & ~0x7FF;

            memmove(dir->data+dir->backups-spare,
                    dir->data+dir->backups,
                    dir->size-dir->backups);

            dir->backups                   -= spare;
            dir->size                      -= spare;
            *(bits*)(dir->data+BIGDIR_SIZE) = dir->size;
        }
    }
}

/*
 * Description: Adjust a big dir name heap to add or remove entries
 * Parameters : directory, entry number to adjust, correction to pointers
 * Returns    : TRUE if ok, FALSE if no room to extend name heap
 */
osbool AdjustNameHeap(s_dirinfo *dir, bits entry, int adjust)
{
    bits offset = dir->first + entry * BIGDIR_ENTRYSIZE;
    bits namept = *(bits*)(dir->data+offset+BIGDIR_OBNAMEPT);

    if(dir->nameheap+dir->namessize+adjust > dir->backups)
        return FALSE;

    if(entry<dir->entries)
    {
        /* copy up/down part of name heap */
        memmove(dir->data+dir->nameheap+namept + adjust,
                dir->data+dir->nameheap+namept,
                dir->namessize-namept);
    }

    dir->namessize                       = ((dir->namessize+3) & ~3) + adjust;
    *(bits*)(dir->data+BIGDIR_NAMESSIZE) = dir->namessize;

    /* fix up name pointers for remaining entries */
    for(; entry<dir->entries; entry++)
    {
        *(bits*)(dir->data+offset+BIGDIR_OBNAMEPT) += adjust;
        offset += BIGDIR_ENTRYSIZE;
    }

    return TRUE;
}

/*
 * Description: sort the directory entries in to the correct order
 * Note       : dir->entries must be set for old and new dirs
 * Parameters : directory buffer
 * Returns    : none
 */
// cppcheck-suppress unusedFunction
void SortDirectory(s_dirinfo *dir)
{
    osbool bigdir   = info.disc_record.filecore_ver!=0;
    byte*  nameheap = NULL;

    /* copy name heap to temporary buffer for big directories */
    if(bigdir)
    {
        /* do allocate before sort incase of failure */
        if((nameheap=malloc(dir->namessize))==NULL)
        {
            Error("No memory for directory sorting - skipping\r\n");
            return;
        }

        memcpy(nameheap, dir->data+dir->nameheap, dir->namessize);
    }
    else
    {
        /* count entries if not already done (old format only)*/
        if(dir->entries==0)
        {
            osbool done   = FALSE;
            bits   offset = dir->first;

            do
            {
                done    = offset>=dir->size+DIR_LASTMARK || dir->data[offset]==0;
                if(!done)
                {
                    offset += info.disc_record.filecore_ver==0 ? DIR_ENTRYSIZE : BIGDIR_ENTRYSIZE;
                    dir->entries++;
                }
            } while(!done);
        }
    }

    /* set up static pointer to directory for CompareDirEntries */
    CompareDir = dir;

    /* do the sort */
    qsort(dir->data+dir->first,
          dir->entries,
          bigdir ? BIGDIR_ENTRYSIZE : DIR_ENTRYSIZE,
          CompareDirEntries);

    /* for a big dir, rebuild nameheap in the same order as the directory entries */
    if(bigdir)
    {
        bits offset = dir->first;
        bits nextpt = 0;
        bits entry;

        for(entry=0; entry<dir->entries; entry++)
        {
            bits namelen = *(bits*)(dir->data+offset+BIGDIR_OBNAMELEN);
            bits namept  = *(bits*)(dir->data+offset+BIGDIR_OBNAMEPT);

            /* copy the name, rounding up to next word */
            namelen = WHOLEWORDS(namelen+1);
            memcpy(dir->data+dir->nameheap+nextpt, nameheap+namept, namelen);

            /* set back up sin to reflect current order */
            *(bits*)(dir->data+dir->backups+(entry<<2)) = *(bits*)(dir->data+offset+BIGDIR_INDDISCADDR);

            /* update the name pointer and next pointer */
            *(bits*)(dir->data+offset+BIGDIR_OBNAMEPT) = nextpt;
            nextpt                                    += namelen;
            offset                                    += BIGDIR_ENTRYSIZE;
        }

        free(nameheap);
    }
}

/*
 * Description: compares directory entries, used by SortDirectory
 * Parameters : directory buffer
 * Returns    : check byte
 */
int CompareDirEntries(const void *entry1, const void *entry2)
{
    char name1[FILENAME_LEN];
    char name2[FILENAME_LEN];

    if(info.disc_record.filecore_ver==0)
    {
        GetS((byte*)entry1, DIR_OBNAME, DIR_FILENAMELEN, name1);
        GetS((byte*)entry2, DIR_OBNAME, DIR_FILENAMELEN, name2);
    }
    else
    {
        bits namelen;
        bits namept;

        namelen = *(bits*)((byte*)entry1+BIGDIR_OBNAMELEN);
        namept  = *(bits*)((byte*)entry1+BIGDIR_OBNAMEPT);
        GetS(CompareDir->data, CompareDir->nameheap+namept, namelen, name1);

        namelen = *(bits*)((byte*)entry2+BIGDIR_OBNAMELEN);
        namept  = *(bits*)((byte*)entry2+BIGDIR_OBNAMEPT);
        GetS(CompareDir->data, CompareDir->nameheap+namept, namelen, name2);
    }

    return filename_cmp(name1, name2);
}

/*
 * Description: calculate directory check byte
 * Parameters : directory buffer
 * Returns    : check byte
 */
byte DirCheckByte(s_dirinfo *dir)
{
    bits checkbyte = 0;
    bits lastentry = 0;
    bits i;

    if(info.disc_record.filecore_ver==0)
    {
        lastentry = DIR_FIRSTENTRY;

        /* find last directory entry */
        while(lastentry<dir->size+DIR_LASTMARK && dir->data[lastentry]!=0)
            lastentry += DIR_ENTRYSIZE;

        /* accululate whole words at the start */
        for(i=0; i<(lastentry & ~3); i+=4)
            checkbyte = ROR(checkbyte, 13) ^ *(bits*)(dir->data+i);

        /* accumulate remaining bytes of last entry */
        for(; i<lastentry; i++)
            checkbyte = ROR(checkbyte, 13) ^ dir->data[i];

        /* accumulate first bytes of tail */
        for(i=dir->size+DIR_LASTMARK+1; (i&3)!=0; i++)
            checkbyte = ROR(checkbyte, 13) ^ dir->data[i];

        /* accumulate whole words at tail */
        for(; i<dir->size-4; i+=4)
            checkbyte = ROR(checkbyte, 13) ^ *(bits*)(dir->data+i);
    }
    else /* filecore_ver != 0 */
    {
        lastentry = dir->nameheap+dir->namessize;

        /* accumulate up to end of nameheap */
        for(i=0; i<lastentry; i+=4)
            checkbyte = ROR(checkbyte, 13) ^ *(bits*)(dir->data+i);

        /* accumulate first few odd bytes of tail */
        for(i=dir->size+BIGDIR_ENDNAME; i<dir->size+BIGDIR_ENDMASSEQ; i+=4)
            checkbyte = ROR(checkbyte, 13) ^ *(bits*)(dir->data+i);

        /* accumulate all except remaining bytes of last word */
        for(; i<dir->size+BIGDIR_CHECKBYTE; i++)
            checkbyte = ROR(checkbyte, 13) ^ dir->data[i];
    }

    /* EOR bytes together */
    checkbyte = checkbyte ^ (checkbyte>>16);
    checkbyte = checkbyte ^ (checkbyte>>8);
    return (byte)(checkbyte & 0xFF);
}

/*
 * Description: find a directory entry
 * Parameters : directory structure, filename
 * Returns    : sin or 0 if not found
 */
// cppcheck-suppress unusedFunction
bits FindEntry(s_dirinfo *dir, const char *filename, bits *entry, bits *offset)
{
    char name[FILENAME_LEN];
    *entry  = 0;
    *offset = dir->first;

    if(info.disc_record.filecore_ver==0)
    {
        while(dir->data[*offset]!=0)
        {
            GetS(dir->data, (*offset)+DIR_OBNAME, DIR_FILENAMELEN, name);

            if(filename_cmp(name, filename)==0)
                return GetI(dir->data, (*offset)+DIR_INDDISCADDR, 3);

            (*entry)++;
            *offset += DIR_ENTRYSIZE;
        }
    }
    else
    {
        for(*entry=0; (*entry)<dir->entries; (*entry)++)
        {
            bits namelen = *(bits*)(dir->data+(*offset)+BIGDIR_OBNAMELEN);
            bits namept  = *(bits*)(dir->data+(*offset)+BIGDIR_OBNAMEPT);
            GetS(dir->data, dir->nameheap+namept, namelen, name);

            if(filename_cmp(name, filename)==0)
                return *(bits*)(dir->data+(*offset)+BIGDIR_INDDISCADDR);

            *offset += BIGDIR_ENTRYSIZE;
        }
    }
    return 0;
}
/*
 * Description: fix check byte and write a directory
 * Parameters : directory structure
 * Returns    : sucess
 */
// cppcheck-suppress unusedFunction
osbool WriteDirectory(s_dirinfo *dir)
{
    bits cb_off = DIR_CHECKBYTE; // Same for E and E+

    if(info.disc_record.filecore_ver>0)
    {
        /* clear unused area */
        memset(dir->data    + (dir->nameheap+dir->namessize), 0,
               dir->backups - (dir->nameheap+dir->namessize) );
    }

    dir->data[dir->size+cb_off] = (byte)DirCheckByte(dir);

    if(!SaveObject(dir->data, dir->size, dir->sin.self))
    {
        Error("Unable to save Directory %s\r\n", dir->path);
        safe_exit(21);
    }

    return TRUE;
}

/*
 * Description: Attempt to make a unique name in a directory by adding numeric suffix
 * Parameters : directory
 *            : entry number of current name or entries+1 if not part of directory yet,
 *            : current name updated on exit
 * Returns    : none
 */
// cppcheck-suppress unusedFunction
void UniqueName(s_dirinfo *dir, bits current, char *name)
{
    osbool unique;

    do
    {
        char   name2[FILENAME_LEN];
        bits   entry  = 0;
        bits   offset = dir->first;
        osbool done   = FALSE;
        unique        = TRUE;

        /* scan directory for duplicate names */
        do
        {
            if(info.disc_record.filecore_ver==0)
                done = offset>=dir->size+DIR_LASTMARK || dir->data[offset]==0;
            else
                done = entry>=dir->entries;

            if(!done && entry!=current)
            {
                if(info.disc_record.filecore_ver==0)
                {
                    GetS(dir->data, offset+DIR_OBNAME, DIR_FILENAMELEN, name2);
                }
                else
                {
                    bits namelen = *(bits*)(dir->data+offset+BIGDIR_OBNAMELEN);
                    bits namept  = *(bits*)(dir->data+offset+BIGDIR_OBNAMEPT);
                    GetS(dir->data, dir->nameheap+namept, namelen, name2);
                }

                if(filename_cmp(name, name2)==0)
                    unique = FALSE;
            }
            entry++;
            offset += info.disc_record.filecore_ver==0 ? DIR_ENTRYSIZE : BIGDIR_ENTRYSIZE;

        } while(unique && !done);

        /* make name unique by adding or incrementing numeric suffix */
        if(!unique)
        {
            int pos = (int)strlen(name);
            int val;

            while(isdigit(name[pos-1]))
                pos--;

            val = atoi(name+pos) + 1;

            sprintf(name+pos, "%d", val);

            /* check we have not exceeded old format filename length */
            if((int)strlen(name) > (info.disc_record.filecore_ver==0 ? DIR_FILENAMELEN : BIGDIR_FILENAMEMAX))
            {
                /* move name down one - ignore case where 10^namelen numeric entries exist */
                sprintf(name+pos-1, "%d", val);
            }
        }
    } while(!unique);
}

/*
 * Description: makes load & exec addr for a new data file
 * Parameters : pointers to load and exec addrs
 * Returns    : none
 */
void MakeLoadExec(bits *loadaddr, bits *execaddr)
{
    oswordreadclock_utc_block utc;
    utc.op    = oswordreadclock_OP_UTC;
    oswordreadclock_utc(&utc);
    *loadaddr = 0xFFF00000 | (osfile_TYPE_DATA<<8) | utc.utc[4];
    *execaddr = *(bits*)utc.utc;
}

/*
 * Description: returns the leaf element of a path
 * Parameters : pathname
 * Returns    : leafname
 */
char *GetLeaf(char *path)
{
    char *leaf = strrchr(path, '.');

    if(!leaf)
        leaf = path;
    else
        leaf++;

    return leaf;
}

/*
 * Description: return 1-4 byte integer from non-aligned data offset
 * Parameters : data, offset, size
 * Returns    : integer
 */
bits GetI(byte *data, bits offset, bits size)
{
    bits value = 0;
    bits i;

    for(i=0; i<size; i++)
    {
        value |= data[offset+i] << (i<<3);
    }

    return value;
}

/*
 * Description: set 1-4 byte integer to non-aligned data offset
 * Parameters : data, offset, size, value
 * Returns    : integer
 */
void SetI(byte *data, bits offset, bits size, bits value)
{
    bits i;

    for(i=0; i<size; i++)
    {
        data[offset+i] = (byte)(value >> (i<<3));
    }
}

/*
 * Description: gets a control terminated or fixed length string
 * Parameters : data, offset, max size, string buffer (size+1)
 * Returns    : integer
 */
void GetS(byte *data, bits offset, bits size, char *string)
{
    bits i;

    for(i=0; i<size; i++)
    {
        if(data[offset+i] > ' ')
        {
            string[i] = data[offset+i];
        }
        else
        {
            string[i] = 0;
            return;
        }
    }
    string[size] = 0;
}

/*
 * Description: sets a control terminated or fixed legth string
 * Parameters : data, offset, max size, string buffer (size+1)
 * Returns    : integer
 */
void SetS(byte *data, bits offset, bits size, const char *string)
{
    memset(data+offset, 0, size);
    strncpy((char*)(data+offset), string, size);
    data[offset+size-1] = 0;
}

/*
 * Description: cmpares two filenames using observed filer rules
 * Parameters : strings
 * Returns    : as strcmp
 */
int filename_cmp(const char *s1, const char *s2)
{
    int c1;

    do
    {
        int c2;

        /* look up order code for each character */
        c1 = fn_collate[(*(s1++)) & 0xFF];
        c2 = fn_collate[(*(s2++)) & 0xFF];

        /* check end of strings ('\0' maps to -1) */
        if(c1==-1)
            return c2==-1 ? 0 : -1;
        else if(c2==-1)
            return 1;
        else if(c1!=c2)
            return c1-c2;

    } while(c1>=0);

    return 0;
}

/*
 * Description: case insensitive string collation
 * Parameters : strings
 * Returns    : as strcmp
 */
int stricoll(const char *s1, const char *s2)
{
    return territory_collate(territory_CURRENT, s1, s2, territory_IGNORE_CASE);
}

/* EOF */
