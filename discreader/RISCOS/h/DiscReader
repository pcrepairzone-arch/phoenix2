/*******************************************************************************
*                                                                              *
* Project       : DiscReader                                                   *
* Filename      : DiscReader.h                                                 *
* Version       : 1.13 (20-Aug-2017)                                           *
* Author        : David J Ruck for The ARM Club                                *
* Description   : Reads E & E+ discs using code from DiscKnight                *
* Information   : Main header file                                             *
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
*                    NOTE: Update program version below                        *
*******************************************************************************/

#ifndef _DiscReader_h
#define _DiscReader_h

#include "SparceFile.h"

/* --- literals ------------------------------------------------------------- */

/* #define BUILD_READONLY */

#define PROGNAME                "DiscReader"
#define VERSION                 "1.13 (20-Aug-2017)"
#define PATHNAME_LEN            1024
#define FILENAME_LEN            256
#define STRING_LEN              256

#define LBA0_ADDR                   0x0000
#define LBA0_SIZE                   0x1000
#define BOOT_BLOCK_ADDR             0xC00
#define BOOT_BLOCK_DEFECTS_END      0x1B0
#define BOOT_BLOCK_RECORD_OFFSET    0x1C0
#define BOOT_BLOCK_CRC              0x1FF
#define BOOT_BLOCK_SIZE             0x200

#define DEFECT_LIST_END1            0x20000000
#define DEFECT_LIST_END2            0x40000000

#define DISC_RECORD_LOG2SECSIZE     0
#define DISC_RECORD_SECSPERTRACK    1
#define DISC_RECORD_HEADS           2
#define DISC_RECORD_DENSITY         3
#define DISC_RECORD_IDLEN           4
#define DISC_RECORD_LOG2BPMB        5
#define DISC_RECORD_SKEW            6
#define DISC_RECORD_BOOTOPTION      7
#define DISC_RECORD_LOWSECTOR       8
#define DISC_RECORD_NZONES          9
#define DISC_RECORD_ZONE_SPARE      10
#define DISC_RECORD_ROOT            12
#define DISC_RECORD_DISC_SIZE_LOW   16
#define DISC_RECORD_CYCLE_ID        20
#define DISC_RECORD_NAME            22
#define DISC_RECORD_NAME_SIZE       10
#define DISC_RECORD_DISC_TYPE       32
#define DISC_RECORD_DISC_SIZE_HIGH  36
#define DISC_RECORD_SHARE_SIZE      40
#define DISC_RECORD_BIG_FLAG        41
#define DISC_RECORD_NZONES2         42
#define DISC_RECORD_FILECORE_VER    44
#define DISC_RECORD_ROOT_DIR_SIZE   48
#define DISC_RECORD_SIZE            60
#define DISC_RECORD_BITS            (DISC_RECORD_SIZE<<3)

#define MAP_BITS_OFFSET             4
#define MAP_FREECHAIN_ZERO          0x18
#define MAP_FREECHAIN_EMPTY         0xFFFFFFE8

#define DIRECTORY_NORMAL_SIZE       2048
#define DIRECTORY_EXTENSION_SIZE    2048
#define DIRECTORY_MAX_SIZE          (4096*1024)
#define DIRECTORY_HUGO              (*(bits*)"Hugo")
#define DIRECTORY_NICK              (*(bits*)"Nick")
#define DIRECTORY_BIG1              (*(bits*)"SBPr")
#define DIRECTORY_BIG2              (*(bits*)"oven")
#define DIRECTORY_ATTR_DIR_MASK        8
#define DIRECTORY_LOST_FOUND        "Lost+Found"
#define DIRECTORY_MAXSCORE          5                   /* max problems before clearing dir */

#define DIR_STARTMASSEQ             0
#define DIR_STARTNAME               1
#define DIR_FIRSTENTRY              5
#define DIR_OBNAME                  0
#define DIR_FILENAMELEN             10
#define DIR_LOAD                    10
#define DIR_EXEC                    14
#define DIR_LEN                     18
#define DIR_INDDISCADDR             22
#define DIR_NEWDIRATTS              25
#define DIR_ENTRYSIZE               26
#define DIR_LASTMARK                -41
#define DIR_RESERVED                -40
#define DIR_NEWDIRPARENT            -38
#define DIR_NEWDIRTITLE             -35
#define DIR_NEWDIRNAME              -16
#define DIR_ENDMASSEQ               -6
#define DIR_ENDNAME                 -5
#define DIR_CHECKBYTE               -1

#define BIGDIR_STARTMASSEQ          0
#define BIGDIR_VERSION              1
#define BIGDIR_STARTNAME            4
#define BIGDIR_NAMELEN              8
#define BIGDIR_SIZE                 12
#define BIGDIR_ENTRIES              16
#define BIGDIR_NAMESSIZE            20
#define BIGDIR_PARENT               24
#define BIGDIR_NAME                 28
#define BIGDIR_LOAD                 0
#define BIGDIR_EXEC                 4
#define BIGDIR_LEN                  8
#define BIGDIR_INDDISCADDR          12
#define BIGDIR_ATTS                 16
#define BIGDIR_OBNAMELEN            20
#define BIGDIR_OBNAMEPT             24
#define BIGDIR_ENTRYSIZE            28
#define BIGDIR_FILENAMEMAX          256
#define BIGDIR_ENDNAME              -8
#define BIGDIR_ENDMASSEQ            -4
#define BIGDIR_RESERVED             -3
#define BIGDIR_CHECKBYTE            -1
#define BIGDIR_MAXSIZE              (4*1024*1024)

#define DESC_LOG2SECSIZE            "Sector size"
#define DESC_SECPERTRACK            "Sectors per track"
#define DESC_HEADS                  "Number of Heads"
#define DESC_TRACKS                 "Number of Tracks"
#define DESC_DENSITY                "Density"
#define DESC_IDLEN                  "ID Length"
#define DESC_LOG2BPMB               "Bytes per map bit (LFAU)"
#define DESC_SKEW                   "Sector skew"
#define DESC_BOOTOPTION             "Boot Option (*Opt 4,n)"
#define DESC_LOWSECTOR1             "Lowest sector id"
#define DESC_LOWSECTOR2             "Disc sides"
#define DESC_LOWSECTOR3             "Disc is 40 track"
#define DESC_NZONES                 "Number of map zones"
#define DESC_ZONESPARE              "Spare bits in zones"
#define DESC_ROOT                   "SIN of root directory"
#define DESC_DISCSIZE               "Disc size"
#define DESC_CYCLEID                "Disc cycle id"
#define DESC_NAME                   "Disc name"
#define DESC_DISCTYPE               "File type of disc"
#define DESC_SHARESIZE              "Sharing granularity"
#define DESC_BIGFLAG                "Big Flag (>512M)"
#define DESC_FILECOREVER            "FileCore version"
#define DESC_ROOTDIRSIZE            "Root directory size"
#define DESC_MINOBJECT              "Minimum object size"

#define ILLEGAL_CHARS            " \"$%^&.:@\\^|"

/* --- macros --------------------------------------------------------------- */

#define TRACEF          printf
#define UNUSED(x)       (void)x;

#define WHOLEWORDS(r)   ((r+3) & ~3)                  /* round to next whole word */
#define ROR(v,r)        ((v >> r) | (v << (32-r)))    /* rotate right */
#define ADDR64SEC(a, s) (a.low>>s) | (a.high<<(32-s)) /* addr 64 to sectors */

/* --- enums ---------------------------------------------------------------- */

/* --- types ---------------------------------------------------------------- */

typedef unsigned short half;

typedef struct                        /* processed disc record */
{
    bits    log2secsize;
    bits    secspertrack;
    bits    heads;
    bits    density;
    bits    idlen;
    bits    log2bpmb;
    bits    skew;
    bits    bootoption;
    bits    lowsector;
    bits    nzones;
    bits    zone_spare;
    bits    root;
    bits64  disc_size;
    bits    cycle_id;
    char    name[11];
    bits    share_size;
    osbool  big_flag;
    osbool  disc_type;
    bits    filecore_ver;
    bits    root_dir_size;
} s_disc_record;

typedef struct s__usedinfo            /* infomation on map object chunks */
{
    bits                zone;
    bits                start;
    bits                len;
    struct s__usedinfo* next;
} s_usedinfo;

typedef struct                         /* infomation on map objects */
{
    byte*               shared;
    bits                directory;
    struct
    {
        bits            used:1;
        bits            shared:1;
        bits            unshared:1;
        bits            checked:1;
    } status;
    struct s__usedinfo* chunks;
} s_objinfo;

typedef struct s__freeinfo           /* information on free space objects */
{
    bits                zone;
    bits                start;
    bits                len;
    struct s__freeinfo* prev;
    struct s__freeinfo* next;
} s_freeinfo;

typedef struct                        /* directory parsing info */
{
    struct
    {
        bits    self;
        bits    parent;
    } sin;
    char    path[PATHNAME_LEN];

    struct
    {
        bits  badname:1;
        bits  backup:1;
    } flags;

    bits    size;
    byte*   data;
    bits    first;
    bits    entries;

    /* new format entries */
    bits    nameheap;
    bits    namessize;
    bits    backups;
} s_dirinfo;

typedef struct
{
    bits    driveno;
    bits64  address;
} s_discspec64;

typedef struct                      /* program information */
{
    struct                          /* program options */
    {
        osbool  help;
        osbool  verbose;
        osbool  file;
        osbool  extract;
        osbool  backup;
        struct
        {
            osbool  map;
            osbool  files;
            osbool  tables;
            osbool  stats;
        } show;
    } opts;

    struct
    {
        char*   name;               /* input file info */
        osbool  sparce;
        os_fw   h;
    } file;

    struct
    {
        char*   fs_name;
        int     drive;
        int     byteSWI;
        int     sectorSWI;
        int     byte64SWI;
    } disc;

    struct
    {
        bits    map[2];
        bits    root;
        bits    map_zone;
    } locations;

    struct
    {
        bits    sizesec;
        bits    cylinders;
        bits    heads;
        bits    sectors;
        bits    l2secsize;
        bits    startsec;
    } params;

    struct
    {
        char*   source;
        char*   dest;
        bits    matchlen;
    } backup;

    struct
    {
        struct
        {
            bits    zone;
            bits    ids;
            bits    ids_pz;
            bits    lfau;
            bits    l2share;
            bits    share;
            bits    minimum;
            bits    map;
        } size;

        struct
        {
            bits    objects;
            bits    free;
            bits    files;
            bits    directories;
            bits    defects;
            bits    excess;
            struct
            {
                bits all;
                bits necessary;
            } fragmented;
        } count;

        struct
        {
            bits64  used;
            bits64  free;
            bits64  files;
            bits64  directories;
            bits64  defects;
            bits64  rounding;
            bits64  excess;
        } space;
    } stats;


    char*           extract_file;
    s_disc_record   disc_record;
    byte*           lba0;
    byte*           bootblock;
    byte*           map[2];
    s_objinfo**     objects;
    s_freeinfo*     freelist;

} s_info;

/* --- globals -------------------------------------------------------------- */

extern s_info info;

/* --- functions ------------------------------------------------------------ */

/* DiscReader.c */
void   Info(const char *string, ...);
void   Error(const char *string, ...);
void   safe_exit(int code);
char*  SecAddrToString(bits secaddr);

/* DiscOps.c */
osbool Initialise(void);
void   Extract(byte *buffer, osbool sectors, bits start, bits size);
void   ReadBytes(byte *buffer, bits start, bits size);
void   WriteBytes(byte *buffer, bits start, bits size);
void   ReadSectors(byte *buffer, bits start, bits size);
void   WriteSectors(byte *buffer, bits start, bits size);

/* BootRecord.c */
osbool ReadBootBlock(void);
osbool WriteBootBlock(void);
void   GetDiscRecord(s_disc_record *dr, byte *br);
void   DisplayRecord(s_disc_record *dr, osbool full);
bits   CalcMapLocation(s_disc_record *dr);

/* Map.c */
osbool ReadMap(void);
osbool WriteMap(void);
bits   MakeID(s_freeinfo *freeinf, bits size);
bits   MapAddr(bits zone, bits bitoffset);
s_usedinfo *GetUsedInfo(bits id);
bits   GetBit(byte *base, bits bitno);
void   SetBit(byte *base, bits bitno, bits value);

/* Objects.c */
osbool ReadObjects(void);
osbool LoadObject(byte *buffer, bits size, bits sin);
osbool SaveObject(byte *buffer, bits size, bits sin);
osbool ExtractObject(byte *buffer, bits size, bits sin);
osbool CopyObject(bits sin, char *filename, bits size, bits loadaddr, bits execaddr);
bits   SinAddr(bits sin);
void   AddObjectStat(bits size, osbool isdir);

/* Directories.c */
osbool ReadDirectory(s_dirinfo *dir);

/* DirUtils.c */
osbool LoadDirectory(s_dirinfo *dir);
osbool IsDirectory(bits sin, char *name, bits *size, bits *parent);
void   CreateDir(s_dirinfo *dir);
osbool AddEntry(s_dirinfo *dir, bits alloc, const char *filename, bits sin, bits size, osbool isdir);
void   RemoveEntry(s_dirinfo *dir, bits entry, bits offset);
bits   FindEntry(s_dirinfo *dir, const char *filename, bits *entry, bits *offset);
osbool WriteDirectory(s_dirinfo *dir);
byte   DirCheckByte(s_dirinfo *dir);
osbool AdjustNameHeap(s_dirinfo *dir, bits entry, int adjust);
void   SortDirectory(s_dirinfo *dir);
void   MakeLoadExec(bits *loadaddr, bits *execaddr);
char*  GetLeaf(char *path);
bits   GetI(byte *data, bits offset, bits size);
void   SetI(byte *data, bits offset, bits size, bits value);
void   GetS(byte *data, bits offset, bits size, char *string);
void   SetS(byte *data, bits offset, bits size, const char *string);
int    filename_cmp(const char *s1, const char *s2);
int    stricoll(const char *s1, const char *s2);

#endif
/* EOF */
