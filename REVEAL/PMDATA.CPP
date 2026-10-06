/*===========================================================================
 * PMDATA.CPP  -  Reveal for OS/2: data gathering and report building
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * Every page in the detail pane is produced here as a flat list of report
 * rows.  Nothing in this file paints anything, and nothing in PMPANE.CPP
 * knows what a processor is; adding a page means writing one Build...()
 * function and adding a line to the category table.
 *
 * The pages are the Windows builds' pages.  The sources are not:
 *
 *   Windows builds                    this build
 *   ------------------------------    -----------------------------------
 *   GlobalMemoryStatus                DosQuerySysInfo QSV_TOTPHYSMEM.. (the
 *                                     memory figures are new with Warp)
 *   the paging file                   SWAPPATH= in CONFIG.SYS, and the size
 *                                     SWAPPER.DAT has grown to
 *   BIOS data area equipment word     DosDevConfig
 *   ROM through a selector/pointer    LOWMEM.CPP, via the TESTCFG$ driver
 *   SYSTEM.INI [boot] drivers         CONFIG.SYS DEVICE= / BASEDEV= / IFS=,
 *                                     and OS2SYS.INI for the display driver
 *   GetDriveType, GetDiskFreeSpace    DosQueryFSAttach, DosQueryFSInfo
 *   INT 13h / IOCTL disk geometry     DosPhysicalDisk + IOCTL_PHYSICALDISK
 *   ToolHelp / Toolhelp32 processes   the Window List (WinQuerySwitchList)
 *   WINMM device counts               MCI "sysinfo" strings, if MMPM/2 is
 *                                     installed
 *
 * Anything this OS/2 cannot answer is left off its page rather than shown as
 * a zero: a missing row reads as "not known", a zero reads as a fact.
 *
 * The benchmark is the one long operation, and it runs on a worker thread
 * (PMWORKER.C) - see the Benchmark section at the end.
 *===========================================================================*/
#define INCL_WIN
#define INCL_GPI
#define INCL_DOS
#define INCL_DOSDEVIOCTL
#define INCL_DOSERRORS

#include <os2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "reveal.h"
#include "lowmem.h"
#include "pmworker.h"
#include "res/resource.h"

#include "processr.hpp"
#include "romsrchr.hpp"
#include "benchmrk.hpp"

/*===========================================================================
 * Cached machine description
 *
 * The expensive, unchanging half (CPU identification, ROM scans, CONFIG.SYS)
 * is read once at start-up.  The volatile half (memory and swap figures, the
 * first-megabyte map) is re-read by RvDataRefresh.
 *=========================================================================*/

typedef struct rv_memory_t {
    BOOL    known;                      /* QSV_TOTPHYSMEM answered           */
    ULONG   total_phys;                 /* bytes                             */
    ULONG   resident;                   /* bytes that cannot be paged out    */
    ULONG   avail;                      /* bytes still allocatable, swap too */
    ULONG   max_private;                /* largest private allocation        */
    ULONG   max_shared;                 /* largest shared allocation         */
    BOOL    high_known;                 /* Warp 4.5 high-memory figures      */
    ULONG   max_high_private;
    ULONG   max_high_shared;
    ULONG   va_limit_mb;                /* QSV_VIRTUALADDRESSLIMIT, 0 = n/a  */
} rv_memory_t;

typedef struct rv_swap_t {
    BOOL    known;                      /* SWAPPATH= found and parsed        */
    char    directory[ CCHMAXPATH ];
    ULONG   minfree_kb;                 /* keep this much free on the drive  */
    ULONG   initial_kb;                 /* size SWAPPER.DAT starts at        */
    BOOL    file_known;
    double  file_bytes;                 /* what SWAPPER.DAT has grown to     */
    BOOL    drive_known;
    double  drive_free;                 /* bytes free on its drive           */
} rv_swap_t;

static processor_t          g_cpu;
static rv_memory_t          g_mem;
static rv_swap_t            g_swap;
static benchmark_results_t  g_bench;
static BOOL                 g_bench_done  = FALSE;
static BOOL                 g_initialised = FALSE;
static BOOL                 g_rom         = FALSE;  /* ROM copy is there    */
static ULONG                g_cpu_mhz     = 0;
static ULONG                g_ncpu        = 0;      /* 0 = not reported     */
static ULONG                g_ver_major   = 0;
static ULONG                g_ver_minor   = 0;
static ULONG                g_ver_rev     = 0;
static ULONG                g_boot_drive  = 3;      /* 1 = A:               */
static ULONG                g_page_size   = 4096;
static BYTE                 g_model       = 0;
static BYTE                 g_submodel    = 0;
static BOOL                 g_model_known = FALSE;
static BYTE                 g_floppies    = 0;
static BYTE                 g_serial      = 0;
static BYTE                 g_parallel    = 0;
static BYTE                 g_adapter     = 1;      /* 0 = monochrome       */
static char                 g_pc_vendor[ 64 ];
static char                 g_bios_name[ 64 ];
static char                 g_bios_date[ 40 ];
static char                 g_video_rom_vendor[ 64 ];
static char                 g_video_rom_version[ 64 ];

BYTE g_memmap[ MEMMAP_BLOCKS ];

/*---------------------------------------------------------------------------
 * Names the ROM searches look for - the Windows builds' tables, copied from
 * HARDWARE.CPP and VIDEO.CPP (neither is linked: the rest of both is BIOS
 * calls).
 *-------------------------------------------------------------------------*/

static const char* ComputerNames[] =
{
    "Amstrad", "Wang", "Apricot", "Bull ", "Toshiba", "NEC ", "Texas Inst",
    "Austin", "Cumulus", "Ergo", "Hyundai", "Nokia", "Ogivar", "Siemens",
    "Tandem", "Tandon", "Mitsubishi", "Matsushita", "Sharp", "DEC ", "Micron",
    "Hewlett Packard", "Hewlett-Packard", "Epson", "Leading Edge", "NCR ",
    "Acer", "ALR ", "Norsk", "Altec", "Altos", "Televideo", "Chaintech",
    "Commodore", "CompuAdd", "Gateway", "Hauppauge", "Northgate", "Goldstar",
    "Laser", "Normerel", "Panasonic", "Swan", "Orange", "Xerox", "Zeos",
    "Parallan", "Ampro", "AT&T", "Club ", "Data General", "Sperry", "Unisys",
    "Arche", "PC Brand", "Elonex", "Emerson", "DFI", "SDI ", "AIR ", "CSS ",
    "Mitsuba", "Osicom", "Polywell", "SAI ", "Micro Express", "Intel",
    "Packard Bell", "PC Craft", "Headstart", "Telex", "Mitac", "Everex",
    "DTK ", "Koyro", "Tandy", "Wyse", "Zenith", "Amkly", "AST ", "Grid",
    "Cheetah", "Psion", "US Micro", "Smith Corona", "Reply", "Dolch",
    "Aquiline", "Comtrade", "Cardinal", "Compudyne", "Schmit", "Victor",
    "Chicony", "Brother", "Dell", "HP ", "ALI", "eMachines", "ECS ", "FIC ",
    "Compaq", "M5Pi", "Asus", "Chaintech", "Soyo", "Abit", "Gigabyte",
    "Ambra", "AOpen", "Tekram", "Tyan", "MSI ", "Olivetti", "ZEOS ", "IBM ",
    NULL
};

static const char* BiosNames[] =
{
    "Phoenix", "AMI ", "American Megatrends", "Award", "DTK ", "Micronics",
    "ERSO", "Datatech", "Data Tech", "Olivetti", "Intel", "Seiko", "Ultima",
    "Mylex", "Artek", "Zenith", "(C)ZDS ", "Western Digital", "Quadtel",
    "Compaq", "Monolithic", "Micro Firmware", "Microid", "Tandy", "IBM ",
    NULL
};

static const char* VideoManufacturers[] = {
    "Ahead", "Appian", "Oak Technologies", "Sigma", "Video Seven", "Video-7",
    "Genoa Systems", "Chips and Technologies", "Tseng Labs", "Tecmar", "Paradise",
    "ATI Tech", "Everex", "Quadtel", "Headland", "Hercules", "Rendition",
    "Trident", "Western Digital", "MaxLogic", "Diamond", "AMI ", "PCG ",
    "Tatung", "Amdek", "AST ", "Cirrus Logic", "Matrox", "Orchid", "STB ",
    "Number Nine", "3Dfx", "nVidia", "Avance Logic", "Imagination",
    "PowerColor", "Tandy", "Olivetti", "Colorgraphics", "NeoMagic",
    "S3 Inc", "Intel", "SiS", "VIA",
    NULL
};

/*===========================================================================
 * Small formatting helpers
 *=========================================================================*/

#define NUMBUF_COUNT    8
#define NUMBUF_SIZE     32

static char  numbuf[ NUMBUF_COUNT ][ NUMBUF_SIZE ];
static int   numbuf_next = 0;

static char* NextNumBuf( void )
{
    char* p = numbuf[ numbuf_next ];
    numbuf_next = ( numbuf_next + 1 ) % NUMBUF_COUNT;
    return p;
}

/*---------------------------------------------------------------------------
 * Comma - group a number in threes: 1234567 -> "1,234,567"
 *-------------------------------------------------------------------------*/

static const char* Comma( ULONG value )
{
    char  digits[ 16 ];
    char* out = NextNumBuf();
    int   len = 0;
    int   i, j;

    if ( value == 0 ) {
        digits[ len++ ] = '0';
    }
    while ( value > 0 ) {
        digits[ len++ ] = (char)( '0' + (int)( value % 10 ) );
        value /= 10;
    }

    j = 0;
    for ( i = len - 1; i >= 0; --i ) {
        out[ j++ ] = digits[ i ];
        if ( i > 0 && ( i % 3 ) == 0 ) out[ j++ ] = ',';
    }
    out[ j ] = '\0';
    return out;
}

/*---------------------------------------------------------------------------
 * KBytes / ByteSize - a size with the unit the reader wants
 *
 * ByteSize takes a double: JFS volumes and FAT32 through the IFS pass 4 GB,
 * and a ULONG of bytes would wrap on them.
 *-------------------------------------------------------------------------*/

static const char* KBytes( ULONG kb )
{
    char* out = NextNumBuf();

    if ( kb >= 1024UL * 10UL ) {
        sprintf( out, "%s MB", Comma( kb / 1024UL ) );
    }
    else {
        sprintf( out, "%s KB", Comma( kb ) );
    }
    return out;
}

static const char* ByteSize( double bytes )
{
    char*  out = NextNumBuf();
    double mb  = bytes / 1048576.0;

    if ( mb >= 1024.0 * 100.0 ) {
        sprintf( out, "%s GB", Comma( (ULONG)( mb / 1024.0 ) ) );
    }
    else if ( mb >= 10.0 ) {
        sprintf( out, "%s MB", Comma( (ULONG)mb ) );
    }
    else {
        sprintf( out, "%s KB", Comma( (ULONG)( bytes / 1024.0 ) ) );
    }
    return out;
}

static const char* YesNo( ULONG flag )
{
    return flag ? "Yes" : "No";
}

static int Percent( double part, double whole )
{
    int pct;

    if ( whole <= 0.0 ) return 0;
    pct = (int)( part * 100.0 / whole + 0.5 );
    if ( pct < 0 )   pct = 0;
    if ( pct > 100 ) pct = 100;
    return pct;
}

static void CopyStr( char* dest, const char* src, int size )
{
    int i = 0;

    if ( dest == NULL || size <= 0 ) return;
    if ( src != NULL ) {
        while ( i < size - 1 && src[ i ] != '\0' ) { dest[ i ] = src[ i ]; ++i; }
    }
    dest[ i ] = '\0';
}

static void TrimRight( char* text )
{
    int len = (int)strlen( text );

    while ( len > 0 && isspace( (unsigned char)text[ len - 1 ] ) ) {
        text[ --len ] = '\0';
    }
}

static const char* SkipSpace( const char* p )
{
    while ( *p == ' ' || *p == '\t' ) ++p;
    return p;
}

/* The last path component: "C:\OS2\BOOT\MOUSE.SYS" -> "MOUSE.SYS" */
static const char* BaseName( const char* path )
{
    const char* p = path;
    const char* base = path;

    for ( ; *p != '\0'; ++p ) {
        if ( *p == '\\' || *p == '/' || *p == ':' ) base = p + 1;
    }
    return base;
}

/*===========================================================================
 * Report assembly
 *=========================================================================*/

static report_row_t*    r_rows  = NULL;
static int              r_max   = 0;
static int              r_count = 0;

static void RStart( report_row_t* rows, int max_rows )
{
    r_rows  = rows;
    r_max   = max_rows;
    r_count = 0;
}

static void RAdd( int kind, const char* label, const char* value )
{
    report_row_t* row;

    /* the last row is kept for the RK_END that closes every page */
    if ( r_rows == NULL || r_count >= r_max - 1 ) {
        if ( kind != RK_END || r_rows == NULL || r_count >= r_max ) return;
    }

    row = &r_rows[ r_count++ ];
    row->kind  = kind;
    row->pct   = 0;
    row->align = 0;
    CopyStr( row->label, label, RPT_LABEL_MAX );
    CopyStr( row->value, value, RPT_VALUE_MAX );
}

static void RFmt( int kind, const char* label, const char* fmt, ... )
{
    char    buffer[ 256 ];
    va_list args;

    va_start( args, fmt );
    _vsnprintf( buffer, sizeof( buffer ) - 1, fmt, args );
    va_end( args );
    buffer[ sizeof( buffer ) - 1 ] = '\0';

    RAdd( kind, label, buffer );
}

static void RGauge( const char* label, const char* value, int pct )
{
    if ( r_rows == NULL || r_count >= r_max - 1 ) return;

    RAdd( RK_GAUGE, label, value );
    if ( pct < 0 )   pct = 0;
    if ( pct > 100 ) pct = 100;
    r_rows[ r_count - 1 ].pct = pct;
}

/* a table row (RK_COLHDR / RK_COLROW) whose columns named in 'align' -
   RIGHT_COL bits - are right-justified against the next column stop */
static void RColumns( int kind, int align, const char* value )
{
    if ( r_rows == NULL || r_count >= r_max - 1 ) return;

    RAdd( kind, "", value );
    r_rows[ r_count - 1 ].align = align;
}

static void RHeading( const char* text )  { RAdd( RK_HEADING, text, "" ); }
static void RBlank( void )                { RAdd( RK_BLANK, "", "" ); }
static void RNote( const char* text )     { RAdd( RK_NOTE, "", text ); }

/*===========================================================================
 * The system: DosQuerySysInfo, one index at a time
 *
 * One index per call, never a range: a range that runs past the last index
 * this version of OS/2 knows fails as a whole, and the memory figures
 * (17..21) and everything after them are indeed new with Warp.
 *=========================================================================*/

static BOOL SysValue( ULONG index, ULONG* value )
{
    ULONG v = 0;

    if ( DosQuerySysInfo( index, index, &v, sizeof( v ) ) != NO_ERROR ) return FALSE;
    *value = v;
    return TRUE;
}

/*---------------------------------------------------------------------------
 * OsName - the release, from the version DosQuerySysInfo reports
 *
 * OS/2 2.x and Warp report major 20; the minor number is the release (a
 * compatibility habit from 1.x, where 10 meant 1.0 and 20 meant 2.0).
 * Every later IBM kernel - the 4.5 fix packs, eComStation and ArcaOS - says
 * 20.45, so that is as far as the version number alone can go.
 *-------------------------------------------------------------------------*/

static const char* OsName( void )
{
    static char name[ 64 ];

    if ( g_ver_major == 20 ) {
        switch ( g_ver_minor ) {
            case 0:   return "OS/2 2.0";
            case 10:  return "OS/2 2.1";
            case 11:  return "OS/2 2.11";
            case 30:  return "OS/2 Warp 3";
            case 40:  return "OS/2 Warp 4";
            case 45:  return "OS/2 Warp 4.5";
            default:  break;
        }
        if ( g_ver_minor > 45 ) {
            sprintf( name, "OS/2 (kernel %lu.%lu)", g_ver_major, g_ver_minor );
            return name;
        }
    }

    sprintf( name, "OS/2 %lu.%02lu", g_ver_major / 10, g_ver_minor );
    return name;
}

static const char* OsLine( void )
{
    static char line[ 96 ];

    if ( g_ver_rev != 0 )
        sprintf( line, "%s (version %lu.%lu, revision %lu)", OsName(),
                 g_ver_major, g_ver_minor, g_ver_rev );
    else
        sprintf( line, "%s (version %lu.%lu)", OsName(), g_ver_major, g_ver_minor );
    return line;
}

static char BootLetter( void )
{
    if ( g_boot_drive < 1 || g_boot_drive > 26 ) return 'C';
    return (char)( 'A' + g_boot_drive - 1 );
}

/*===========================================================================
 * CONFIG.SYS
 *
 * Read once from the boot drive.  A good deal of what Windows kept in
 * SYSTEM.INI - which drivers are loaded, where the paging file is - lives
 * here on OS/2, and nowhere else: there is no API for the swap path or the
 * list of base device drivers.
 *=========================================================================*/

#define CONFIG_MAX_BYTES    65535L
#define CONFIG_MAX_LINES    1024

static char*    cfg_text = NULL;
static char*    cfg_line[ CONFIG_MAX_LINES ];
static int      cfg_lines = 0;

static void ConfigLoad( void )
{
    char    path[ 16 ];
    FILE*   fp;
    long    size;
    char*   p;

    sprintf( path, "%c:\\CONFIG.SYS", BootLetter() );

    fp = fopen( path, "rb" );
    if ( fp == NULL ) return;

    fseek( fp, 0L, SEEK_END );
    size = ftell( fp );
    fseek( fp, 0L, SEEK_SET );
    if ( size <= 0 || size > CONFIG_MAX_BYTES ) size = CONFIG_MAX_BYTES;

    cfg_text = (char*)malloc( (size_t)size + 1 );
    if ( cfg_text == NULL ) {
        fclose( fp );
        return;
    }

    size = (long)fread( cfg_text, 1, (size_t)size, fp );
    fclose( fp );
    cfg_text[ size ] = '\0';

    /* split in place: every CR, LF and the DOS end-of-file mark ends a line */
    p = cfg_text;
    while ( *p != '\0' && cfg_lines < CONFIG_MAX_LINES ) {
        cfg_line[ cfg_lines++ ] = p;
        while ( *p != '\0' && *p != '\r' && *p != '\n' && *p != 0x1A ) ++p;
        while ( *p == '\r' || *p == '\n' || *p == 0x1A ) *p++ = '\0';
    }
}

static void ConfigFree( void )
{
    if ( cfg_text != NULL ) free( cfg_text );
    cfg_text  = NULL;
    cfg_lines = 0;
}

/*---------------------------------------------------------------------------
 * ConfigValue - the value of line 'n' if its keyword is 'keyword'
 *
 * The keyword is everything before the first '=', compared without case and
 * with runs of blanks collapsed, so "SET VIDEO_DEVICES" matches
 * "set  video_devices =".  A REM line never matches: its keyword starts
 * "REM".  NULL when the line is something else.
 *-------------------------------------------------------------------------*/

static const char* ConfigValue( int n, const char* keyword )
{
    const char* line;
    const char* eq;
    char        kw[ 48 ];
    int         len = 0;
    const char* p;

    if ( n < 0 || n >= cfg_lines ) return NULL;

    line = SkipSpace( cfg_line[ n ] );
    eq = strchr( line, '=' );
    if ( eq == NULL ) return NULL;

    for ( p = line; p < eq && len < (int)sizeof( kw ) - 1; ++p ) {
        char c = *p;

        if ( c == '\t' ) c = ' ';
        if ( c == ' ' && ( len == 0 || kw[ len - 1 ] == ' ' ) ) continue;
        kw[ len++ ] = (char)toupper( (unsigned char)c );
    }
    while ( len > 0 && kw[ len - 1 ] == ' ' ) --len;
    kw[ len ] = '\0';

    if ( stricmp( kw, keyword ) != 0 ) return NULL;
    return SkipSpace( eq + 1 );
}

/* The first line with this keyword, or NULL. */
static const char* ConfigFind( const char* keyword )
{
    int n;

    for ( n = 0; n < cfg_lines; ++n ) {
        const char* v = ConfigValue( n, keyword );
        if ( v != NULL ) return v;
    }
    return NULL;
}

/*---------------------------------------------------------------------------
 * ConfigDriverLine - the DEVICE= line loading a given driver file
 *-------------------------------------------------------------------------*/

static const char* ConfigDriverLine( const char* keyword, const char* file )
{
    int n;

    for ( n = 0; n < cfg_lines; ++n ) {
        const char* v = ConfigValue( n, keyword );
        char        first[ CCHMAXPATH ];
        int         len = 0;

        if ( v == NULL ) continue;
        while ( v[ len ] != '\0' && v[ len ] != ' ' && v[ len ] != '\t' &&
                len < (int)sizeof( first ) - 1 ) {
            first[ len ] = v[ len ];
            ++len;
        }
        first[ len ] = '\0';

        if ( stricmp( BaseName( first ), file ) == 0 ) return v;
    }
    return NULL;
}

/*---------------------------------------------------------------------------
 * AddDriverList - one row per driver loaded by lines with this keyword
 *
 * Label is the file name, value the parameters after it.  Returns the count.
 *-------------------------------------------------------------------------*/

static int AddDriverList( const char* keyword, int limit )
{
    int n;
    int count = 0;

    for ( n = 0; n < cfg_lines && count < limit; ++n ) {
        const char* v = ConfigValue( n, keyword );
        char        file[ CCHMAXPATH ];
        int         len = 0;

        if ( v == NULL || *v == '\0' ) continue;

        while ( v[ len ] != '\0' && v[ len ] != ' ' && v[ len ] != '\t' &&
                len < (int)sizeof( file ) - 1 ) {
            file[ len ] = v[ len ];
            ++len;
        }
        file[ len ] = '\0';

        RAdd( RK_SUBITEM, BaseName( file ), SkipSpace( v + len ) );
        ++count;
    }
    return count;
}

/*===========================================================================
 * Memory
 *=========================================================================*/

/*---------------------------------------------------------------------------
 * ParseSwapPath - "SWAPPATH=C:\OS2\SYSTEM 2048 4096"
 *
 * The directory, then the free space to leave on its drive and the size the
 * swap file starts at, both in KB.  The two numbers are optional.
 *-------------------------------------------------------------------------*/

static void ParseSwapPath( void )
{
    const char* v = ConfigFind( "SWAPPATH" );
    int         len = 0;

    memset( &g_swap, 0, sizeof( g_swap ) );
    if ( v == NULL || *v == '\0' ) return;

    while ( v[ len ] != '\0' && v[ len ] != ' ' && v[ len ] != '\t' &&
            len < (int)sizeof( g_swap.directory ) - 1 ) {
        g_swap.directory[ len ] = v[ len ];
        ++len;
    }
    g_swap.directory[ len ] = '\0';

    v = SkipSpace( v + len );
    g_swap.minfree_kb = strtoul( v, (char**)&v, 10 );
    v = SkipSpace( v );
    g_swap.initial_kb = strtoul( v, NULL, 10 );

    g_swap.known = ( g_swap.directory[ 0 ] != '\0' );
}

/*---------------------------------------------------------------------------
 * ReadSwapFile - how big SWAPPER.DAT is now, and room left for it to grow
 *-------------------------------------------------------------------------*/

static void ReadSwapFile( void )
{
    char            path[ CCHMAXPATH + 16 ];
    HDIR            hdir  = HDIR_CREATE;
    ULONG           count = 1;
    FILEFINDBUF3    ff;
    FSALLOCATE      fsa;
    int             len;

    g_swap.file_known  = FALSE;
    g_swap.drive_known = FALSE;
    if ( !g_swap.known ) return;

    CopyStr( path, g_swap.directory, CCHMAXPATH );
    len = (int)strlen( path );
    if ( len > 0 && path[ len - 1 ] != '\\' ) strcat( path, "\\" );
    strcat( path, "SWAPPER.DAT" );

    /* DosFindFirst, not DosQueryPathInfo: the file is open by the kernel for
       the life of the system, and a directory search reads its size from the
       directory without having to open it. */
    if ( DosFindFirst( (PSZ)path, &hdir, FILE_NORMAL | FILE_HIDDEN | FILE_SYSTEM,
                       &ff, sizeof( ff ), &count, FIL_STANDARD ) == NO_ERROR ) {
        g_swap.file_bytes = (double)ff.cbFile;
        g_swap.file_known = TRUE;
        DosFindClose( hdir );
    }

    if ( isalpha( (unsigned char)g_swap.directory[ 0 ] ) && g_swap.directory[ 1 ] == ':' ) {
        ULONG disk = (ULONG)( toupper( (unsigned char)g_swap.directory[ 0 ] ) - 'A' + 1 );

        if ( DosQueryFSInfo( disk, FSIL_ALLOC, &fsa, sizeof( fsa ) ) == NO_ERROR ) {
            g_swap.drive_free = (double)fsa.cUnitAvail * (double)fsa.cSectorUnit
                                * (double)fsa.cbSector;
            g_swap.drive_known = TRUE;
        }
    }
}

static void ReadMemoryStatus( void )
{
    ULONG v;

    memset( &g_mem, 0, sizeof( g_mem ) );

    g_mem.known = SysValue( QSV_TOTPHYSMEM, &g_mem.total_phys );
    if ( g_mem.known ) {
        SysValue( QSV_TOTRESMEM,  &g_mem.resident );
        SysValue( QSV_TOTAVAILMEM, &g_mem.avail );
        SysValue( QSV_MAXPRMEM,   &g_mem.max_private );
        SysValue( QSV_MAXSHMEM,   &g_mem.max_shared );
    }

    /* the high-memory arena above 512 MB: Warp Server SMP and 4.5 kernels */
    if ( SysValue( QSV_MAXHPRMEM, &g_mem.max_high_private ) &&
         SysValue( QSV_MAXHSHMEM, &g_mem.max_high_shared ) ) {
        g_mem.high_known = TRUE;
    }
    if ( SysValue( QSV_VIRTUALADDRESSLIMIT, &v ) ) g_mem.va_limit_mb = v;

    ReadSwapFile();
}

/*===========================================================================
 * The first-megabyte map
 *=========================================================================*/

static void MarkBlocks( ULONG from, ULONG to, BYTE what )
{
    int first = (int)( from / MEMMAP_BLOCK_BYTES );
    int last  = (int)( ( to + MEMMAP_BLOCK_BYTES - 1 ) / MEMMAP_BLOCK_BYTES );
    int i;

    if ( first < 0 ) first = 0;
    if ( last > MEMMAP_BLOCKS ) last = MEMMAP_BLOCKS;

    for ( i = first; i < last; ++i ) g_memmap[ i ] = what;
}

/*---------------------------------------------------------------------------
 * BuildMemoryMap
 *
 * Conventional memory is all shown as in use: it is OS/2's, which runs its
 * kernel and drivers there and hands what is left to its own memory manager
 * - there is no DOS arena to ask.  The upper memory area is classified by
 * the 55 AA signature every option ROM must start with, read out of LOWMEM's
 * copy of the ROM area; without that copy there is no map.
 *-------------------------------------------------------------------------*/

static void BuildMemoryMap( void )
{
    ULONG addr;
    int   i;

    for ( i = 0; i < MEMMAP_BLOCKS; ++i ) g_memmap[ i ] = MM_UNUSED;
    if ( !g_rom ) return;

    MarkBlocks( 0, 0xA0000UL, MM_PROGRAM );
    MarkBlocks( 0, MEMMAP_BLOCK_BYTES, MM_SYSTEM );     /* vectors + BDA    */

    MarkBlocks( 0xA0000UL, 0xC0000UL, MM_VIDEO );
    MarkBlocks( 0xC0000UL, 0xF0000UL, MM_UPPER );

    for ( addr = 0xC0000UL; addr < 0xF0000UL; addr += 0x800UL ) {
        const BYTE* rom = LowRom( addr, 3 );

        if ( rom != NULL && rom[ 0 ] == 0x55 && rom[ 1 ] == 0xAA ) {
            ULONG length = (ULONG)rom[ 2 ] * 512UL;

            if ( length == 0 || length > 0x20000UL ) length = 0x800UL;
            MarkBlocks( addr, addr + length, MM_ADAPTER_ROM );
            addr += ( ( length - 1 ) & ~0x7FFUL );
        }
    }

    MarkBlocks( 0xF0000UL, 0x100000UL, MM_SYSTEM_ROM );
}

/*===========================================================================
 * Processor speed
 *
 * The time stamp counter against a clock.  The clock is the 1.19 MHz
 * high-resolution timer where the kernel has one (DosTmrQueryTime, new with
 * Warp and so looked up at run time rather than imported - an import OS/2
 * 2.x cannot satisfy would stop the program loading at all), and the
 * millisecond count otherwise, which only ticks every 32 ms or so and so is
 * given a longer interval.  Three passes, best of, because a pass that loses
 * the CPU to another thread reads low and never high.
 *=========================================================================*/

#define ORD_DOS32TMRQUERYFREQ   362
#define ORD_DOS32TMRQUERYTIME   363

typedef APIRET ( APIENTRY *PFN_TMRFREQ )( PULONG );
typedef APIRET ( APIENTRY *PFN_TMRTIME )( PQWORD );

static ULONG RdTscLow( void )
{
    ULONG tsc_low;

    __asm {
        xor     eax, eax
        xor     edx, edx            // RDTSC writes both
        _emit   0x0F                // RDTSC
        _emit   0x31
        mov     tsc_low, eax
    }
    return tsc_low;
}

static ULONG MsCount( void )
{
    ULONG ms = 0;

    DosQuerySysInfo( QSV_MS_COUNT, QSV_MS_COUNT, &ms, sizeof( ms ) );
    return ms;
}

static ULONG MeasureMhzWithTimer( PFN_TMRTIME pTime, ULONG freq )
{
    ULONG best = 0;
    int   pass;

    for ( pass = 0; pass < 3; ++pass ) {
        QWORD  start, now;
        ULONG  tsc_start, tsc_end;
        ULONG  ticks, cycles, mhz;
        double elapsed_us;

        if ( pTime( &start ) != NO_ERROR ) return 0;
        tsc_start = RdTscLow();

        /* 50 ms: long enough to swamp the call overhead, short enough that
           a 4 GHz part cannot wrap the 32-bit cycle count (that takes 1 s) */
        do {
            if ( pTime( &now ) != NO_ERROR ) return 0;
            ticks = now.ulLo - start.ulLo;
        } while ( (double)ticks * 1000.0 / (double)freq < 50.0 );

        tsc_end = RdTscLow();

        elapsed_us = (double)ticks * 1000000.0 / (double)freq;
        cycles     = tsc_end - tsc_start;           /* wraps cancel out      */
        if ( elapsed_us < 1.0 ) continue;

        mhz = (ULONG)( (double)cycles / elapsed_us + 0.5 );
        if ( mhz > best ) best = mhz;
    }
    return best;
}

static ULONG MeasureMhzWithMsCount( void )
{
    ULONG best = 0;
    int   pass;

    for ( pass = 0; pass < 3; ++pass ) {
        ULONG tick_start, tick_now, tick_end;
        ULONG tsc_start, tsc_end;
        ULONG elapsed, cycles, mhz;

        /* line up on a tick edge so the timer granularity is not counted twice */
        tick_start = MsCount();
        do { tick_now = MsCount(); } while ( tick_now == tick_start );

        tsc_start = RdTscLow();
        do { tick_end = MsCount(); } while ( ( tick_end - tick_now ) < 150UL );
        tsc_end = RdTscLow();

        elapsed = tick_end - tick_now;              /* milliseconds          */
        cycles  = tsc_end - tsc_start;
        if ( elapsed == 0 ) continue;

        mhz = ( cycles / elapsed ) / 1000UL;        /* cycles/ms -> cycles/us */
        if ( mhz > best ) best = mhz;
    }
    return best;
}

static ULONG MeasureMhz( void )
{
    HMODULE     hDoscalls = NULLHANDLE;
    PFN         pfn       = NULL;
    PFN_TMRFREQ pFreq     = NULL;
    PFN_TMRTIME pTime     = NULL;
    ULONG       freq      = 0;

    if ( g_cpu.intel_features1.bits.tsc == 0 &&
         g_cpu.amd_features1.bits.tsc   == 0 ) return 0;

    if ( DosQueryModuleHandle( (PSZ)"DOSCALLS", &hDoscalls ) == NO_ERROR ) {
        if ( DosQueryProcAddr( hDoscalls, ORD_DOS32TMRQUERYFREQ, NULL, &pfn ) == NO_ERROR )
            pFreq = (PFN_TMRFREQ)pfn;
        if ( DosQueryProcAddr( hDoscalls, ORD_DOS32TMRQUERYTIME, NULL, &pfn ) == NO_ERROR )
            pTime = (PFN_TMRTIME)pfn;
    }

    if ( pFreq != NULL && pTime != NULL && pFreq( &freq ) == NO_ERROR && freq != 0 ) {
        ULONG mhz = MeasureMhzWithTimer( pTime, freq );
        if ( mhz != 0 ) return mhz;
    }

    return MeasureMhzWithMsCount();
}

/*===========================================================================
 * The BIOS and the equipment list
 *=========================================================================*/

static void ReadBiosFromRom( void )
{
    RomStringSearch( F800, ComputerNames, g_pc_vendor, sizeof( g_pc_vendor ) );
    RomStringSearch( F800, BiosNames,     g_bios_name, sizeof( g_bios_name ) );
    RomDateSearch( F800, g_bios_date, sizeof( g_bios_date ) );

    /* the video adapter's own ROM, which OS/2 never reports */
    RomStringSearch( C000, VideoManufacturers, g_video_rom_vendor,
                     sizeof( g_video_rom_vendor ) );
    RomVersionSearch( C000, g_video_rom_version, sizeof( g_video_rom_version ) );
}

static void ReadEquipment( void )
{
    BYTE b;

    if ( DosDevConfig( &b, DEVINFO_FLOPPY )  == NO_ERROR ) g_floppies = b;
    if ( DosDevConfig( &b, DEVINFO_RS232 )   == NO_ERROR ) g_serial   = b;
    if ( DosDevConfig( &b, DEVINFO_PRINTER ) == NO_ERROR ) g_parallel = b;
    if ( DosDevConfig( &b, DEVINFO_ADAPTER ) == NO_ERROR ) g_adapter  = b;

    if ( DosDevConfig( &g_model, DEVINFO_MODEL ) == NO_ERROR &&
         DosDevConfig( &g_submodel, DEVINFO_SUBMODEL ) == NO_ERROR ) {
        g_model_known = TRUE;
    }
}

/*---------------------------------------------------------------------------
 * ModelName - the BIOS model byte, which is how a PS/2 tells you it is one
 *-------------------------------------------------------------------------*/

static const char* ModelName( BYTE model, BYTE submodel )
{
    switch ( model ) {
        case 0xFF:  return "IBM PC";
        case 0xFE:  return "IBM PC/XT";
        case 0xFD:  return "IBM PCjr";
        case 0xFB:  return "IBM PC/XT (1986)";
        case 0xFA:  return "IBM PS/2 Model 25 or 30";
        case 0xF9:  return "IBM PC Convertible";
        case 0xF8:  return "IBM PS/2, Micro Channel 386";
        case 0xFC:
            switch ( submodel ) {
                case 0x04:  return "IBM PS/2 Model 50 or 60";
                case 0x05:  return "IBM PS/2 Model 60";
                case 0x0B:  return "IBM PS/1";
                default:    break;
            }
            return "PC/AT class";
        default:
            break;
    }
    return "Other";
}

/*===========================================================================
 * Initialisation
 *=========================================================================*/

BOOL RvDataInit( void )
{
    if ( g_initialised ) return TRUE;

    /* Never ask the user to put a disk in the drive: the Drives page asks
       every letter for its free space, A: and B: included, and with this an
       empty drive fails the call instead of raising the hard-error box. */
    DosError( FERR_DISABLEHARDERR );

    memset( &g_cpu,   0, sizeof( g_cpu ) );
    memset( &g_bench, 0, sizeof( g_bench ) );
    g_pc_vendor[ 0 ] = g_bios_name[ 0 ] = g_bios_date[ 0 ] = '\0';
    g_video_rom_vendor[ 0 ] = g_video_rom_version[ 0 ] = '\0';

    SysValue( QSV_VERSION_MAJOR,    &g_ver_major );
    SysValue( QSV_VERSION_MINOR,    &g_ver_minor );
    SysValue( QSV_VERSION_REVISION, &g_ver_rev );
    SysValue( QSV_BOOT_DRIVE,       &g_boot_drive );
    SysValue( QSV_PAGE_SIZE,        &g_page_size );
    if ( !SysValue( QSV_NUMPROCESSORS, &g_ncpu ) ) g_ncpu = 0;

    ConfigLoad();
    ParseSwapPath();
    ReadEquipment();

    GetProcessorInfo( &g_cpu );

    g_rom = LowMemInit();
    if ( g_rom ) ReadBiosFromRom();

    g_cpu_mhz = MeasureMhz();

    g_initialised = TRUE;
    RvDataRefresh();
    return TRUE;
}

void RvDataTerm( void )
{
    LowMemTerm();
    ConfigFree();
    g_initialised = FALSE;
}

void RvDataRefresh( void )
{
    ReadMemoryStatus();
    BuildMemoryMap();
}

/*===========================================================================
 * Category table
 *=========================================================================*/

static const char* CategoryNames[ CAT_COUNT ] = {
    "System",
    "Processor",
    "Memory",
    "First Megabyte",
    "Display",
    "Drives",
    "Network",
    "Ports",
    "OS/2",
    "Environment",
    "Benchmark"
};

static const int CategoryBitmaps[ CAT_COUNT ] = {
    IDB_SYSTEM,
    IDB_CPU,            /* replaced at run time by the detected part        */
    IDB_MEMORY,
    IDB_FIRSTMEG,
    0,                  /* Display: no illustration in the resource set     */
    IDB_DRIVES,
    IDB_NETWORK,
    IDB_PORTS,
    0,                  /* OS/2                                             */
    0,                  /* Environment                                     */
    IDB_BENCHMARK
};

const char* RvCategoryName( int category )
{
    if ( category < 0 || category >= CAT_COUNT ) return "";
    return CategoryNames[ category ];
}

/*---------------------------------------------------------------------------
 * ProcessorBitmap - the chip on the Processor page is the chip in the socket
 *
 * GetVendorString shortens the CPUID vendor ("GenuineIntel" -> "Intel"), so
 * both spellings are matched.
 *-------------------------------------------------------------------------*/

static int ProcessorBitmap( void )
{
    const char* vendor = g_cpu.vendor;
    const char* name   = ( g_cpu.brand[ 0 ] != '\0' ) ? g_cpu.brand : g_cpu.description;
    int         family = g_cpu.processor_family;

    if ( strstr( vendor, "NexGen" ) != NULL || strstr( name, "Nx58" ) != NULL )
        return IDB_NX586;

    if ( strstr( vendor, "Cyrix" ) != NULL ) {
        if ( family >= 5 || strstr( name, "6x86" ) != NULL ) return IDB_CX6X86;
        return IDB_CX486;
    }

    if ( strstr( vendor, "AMD" ) != NULL ) {
        if ( family >= 5 ) return IDB_AMDK6;
        if ( family == 4 ) return IDB_AM486DX;
    }

    if ( strstr( name, "TI486" ) != NULL ) return IDB_TI486;

    if ( strstr( vendor, "Intel" ) != NULL ) {
        if ( family >= 6 ) return IDB_PPRO;
        if ( family == 5 ) return IDB_PENTIUM;
        if ( family == 4 ) return IDB_I486;
        if ( family == 3 ) return IDB_386;
    }

    if ( family >= 6 ) return IDB_PPRO;
    if ( family == 5 ) return IDB_PENTIUM;
    if ( family == 4 ) return IDB_486;
    if ( family == 3 ) return IDB_386;

    return IDB_CPU;
}

int RvCategoryBitmap( int category )
{
    if ( category < 0 || category >= CAT_COUNT ) return 0;
    if ( category == CAT_PROCESSOR ) return ProcessorBitmap();
    return CategoryBitmaps[ category ];
}

/*---------------------------------------------------------------------------
 * ProcessorName - the one line everybody reads first
 *-------------------------------------------------------------------------*/

static const char* ProcessorName( void )
{
    static char name[ 64 ];

    if ( g_cpu.brand[ 0 ] != '\0' ) {
        CopyStr( name, g_cpu.brand, sizeof( name ) );
    }
    else if ( g_cpu.description[ 0 ] != '\0' &&
              strcmp( g_cpu.description, "Unknown" ) != 0 ) {
        if ( g_cpu.vendor[ 0 ] != '\0' && g_cpu.vendor[ 0 ] != '"' &&
             strcmp( g_cpu.vendor, "Unknown" ) != 0 )
            sprintf( name, "%s %s", g_cpu.vendor, g_cpu.description );
        else
            CopyStr( name, g_cpu.description, sizeof( name ) );
    }
    else {
        CopyStr( name, "Unknown processor", sizeof( name ) );
    }
    return name;
}

/*===========================================================================
 * The display
 *=========================================================================*/

typedef struct rv_display_t {
    LONG    width, height;
    LONG    colors;                     /* CAPS_COLORS                      */
    LONG    planes, bits;
    LONG    font_dpi_x, font_dpi_y;
    LONG    ppm_x, ppm_y;               /* pels per metre                   */
} rv_display_t;

static BOOL ReadDisplay( rv_display_t* d )
{
    HPS   hps;
    HDC   hdc;
    LONG  caps[ 2 ];

    memset( d, 0, sizeof( *d ) );

    hps = WinGetScreenPS( HWND_DESKTOP );
    if ( hps == NULLHANDLE ) return FALSE;

    hdc = GpiQueryDevice( hps );
    if ( hdc != NULLHANDLE && hdc != HDC_ERROR ) {
        if ( DevQueryCaps( hdc, CAPS_WIDTH, 2, caps ) ) {
            d->width  = caps[ 0 ];
            d->height = caps[ 1 ];
        }
        DevQueryCaps( hdc, CAPS_COLORS, 1, &d->colors );
        if ( DevQueryCaps( hdc, CAPS_COLOR_PLANES, 2, caps ) ) {
            d->planes = caps[ 0 ];
            d->bits   = caps[ 1 ];
        }
        if ( DevQueryCaps( hdc, CAPS_HORIZONTAL_RESOLUTION, 2, caps ) ) {
            d->ppm_x = caps[ 0 ];
            d->ppm_y = caps[ 1 ];
        }
        if ( DevQueryCaps( hdc, CAPS_HORIZONTAL_FONT_RES, 2, caps ) ) {
            d->font_dpi_x = caps[ 0 ];
            d->font_dpi_y = caps[ 1 ];
        }
    }

    WinReleasePS( hps );

    /* the screen is always there: a zero here means the queries failed */
    if ( d->width <= 0 ) {
        d->width  = WinQuerySysValue( HWND_DESKTOP, SV_CXSCREEN );
        d->height = WinQuerySysValue( HWND_DESKTOP, SV_CYSCREEN );
    }
    return TRUE;
}

static void AddColours( const rv_display_t* d )
{
    LONG depth = d->planes * d->bits;

    if ( depth >= 24 || d->colors >= 16777216L ) {
        RAdd( RK_ITEM, "Colours", "16.7 million" );
    }
    else if ( depth >= 16 || d->colors >= 65536L ) {
        RAdd( RK_ITEM, "Colours", "65,536" );
    }
    else if ( d->colors > 0 ) {
        RFmt( RK_ITEM, "Colours", "%s", Comma( (ULONG)d->colors ) );
    }
    else if ( depth > 0 ) {
        RFmt( RK_ITEM, "Colours", "%s", Comma( 1UL << depth ) );
    }
}

/* PM keeps the name of the installed display driver in OS2SYS.INI; not
   every release writes it, so an empty answer just leaves the row out. */
static BOOL DisplayDriverName( char* out, int size )
{
    out[ 0 ] = '\0';
    PrfQueryProfileString( HINI_SYSTEMPROFILE, (PCSZ)"PM_DISPLAYDRIVERS",
                           (PCSZ)"CURRENTDRIVER", (PCSZ)"", out, (ULONG)size );
    TrimRight( out );
    return out[ 0 ] != '\0';
}

/*===========================================================================
 * Page builders
 *=========================================================================*/

/*---------------------------------------------------------------------------
 * System - the page that answers "what is this machine?" in one screen
 *-------------------------------------------------------------------------*/

static void BuildSystem( void )
{
    rv_display_t d;
    char         driver[ 64 ];

    ReadDisplay( &d );

    RHeading( "Computer" );
    if ( g_pc_vendor[ 0 ] != '\0' ) RAdd( RK_ITEM, "Vendor", g_pc_vendor );
    if ( g_model_known )
        RFmt( RK_ITEM, "Model", "%s (%02Xh/%02Xh)",
              ModelName( g_model, g_submodel ), (int)g_model, (int)g_submodel );
    if ( g_bios_name[ 0 ] != '\0' ) RAdd( RK_ITEM, "BIOS", g_bios_name );
    if ( g_bios_date[ 0 ] != '\0' ) RAdd( RK_ITEM, "BIOS date", g_bios_date );

    RBlank();
    RHeading( "Processor" );
    RAdd( RK_ITEM, "Processor", ProcessorName() );
    if ( g_cpu_mhz > 0 ) {
        RFmt( RK_ITEM, "Speed", "%lu MHz (measured)", g_cpu_mhz );
    }
    if ( g_ncpu > 1 ) {
        RFmt( RK_ITEM, "Processors", "%lu", g_ncpu );
    }
    RAdd( RK_ITEM, "Math coprocessor", g_cpu.coprocessor_description );

    RBlank();
    RHeading( "Operating system" );
    RAdd( RK_ITEM, "OS/2", OsLine() );
    RFmt( RK_ITEM, "Boot drive", "%c:", BootLetter() );

    RBlank();
    RHeading( "Memory" );
    if ( g_mem.known ) {
        RFmt( RK_ITEM, "Total", "%s", ByteSize( (double)g_mem.total_phys ) );
        RFmt( RK_ITEM, "Resident", "%s", ByteSize( (double)g_mem.resident ) );
    }
    else {
        RNote( "This version of OS/2 does not report its memory." );
    }

    RBlank();
    RHeading( "Display" );
    RFmt( RK_ITEM, "Resolution", "%ld x %ld", d.width, d.height );
    AddColours( &d );
    if ( DisplayDriverName( driver, sizeof( driver ) ) )
        RAdd( RK_ITEM, "Driver", driver );
}

/*---------------------------------------------------------------------------
 * Processor
 *-------------------------------------------------------------------------*/

static void BuildProcessor( void )
{
    RHeading( "Identification" );
    RAdd( RK_ITEM, "Processor", ProcessorName() );
    RAdd( RK_ITEM, "Vendor", g_cpu.vendor );
    if ( g_cpu.brand[ 0 ] != '\0' && g_cpu.description[ 0 ] != '\0' &&
         strcmp( g_cpu.description, "Unknown" ) != 0 )
        RAdd( RK_ITEM, "Description", g_cpu.description );

    /* Family 0Fh means "see the extended family" - see the Win32s build. */
    {
        int family = g_cpu.processor_family;
        int model  = (int)g_cpu.cpuid.bits.model;

        if ( family == 15 ) family += (int)g_cpu.cpuid.bits.extfamily;
        if ( family == 6 || family >= 15 ) model += (int)g_cpu.cpuid.bits.extmodel << 4;

        RFmt( RK_ITEM, "Family / model / stepping", "%d / %d / %d",
              family, model, (int)g_cpu.cpuid.bits.stepping );
    }
    if ( g_cpu_mhz > 0 ) {
        RFmt( RK_ITEM, "Clock speed", "%lu MHz (measured)", g_cpu_mhz );
    }
    else {
        RAdd( RK_ITEM, "Clock speed", "Not measured" );
    }
    RAdd( RK_ITEM, "Math coprocessor", g_cpu.coprocessor_description );
    /* A kernel that does not answer QSV_NUMPROCESSORS is a uniprocessor
       kernel, which uses one however many are fitted. */
    RFmt( RK_ITEM, "Processors in use", "%lu", ( g_ncpu > 0 ) ? g_ncpu : 1UL );

    RBlank();
    RHeading( "Features" );
    RAdd( RK_SUBITEM, "Enhanced virtual 8086 mode", YesNo( g_cpu.cpu_features1.bits.vme ) );
    RAdd( RK_SUBITEM, "Model-specific registers",   YesNo( g_cpu.cpu_features1.bits.msr ) );
    RAdd( RK_SUBITEM, "Physical address extension", YesNo( g_cpu.cpu_features1.bits.pae ) );
    RAdd( RK_SUBITEM, "Machine check exception",    YesNo( g_cpu.cpu_features1.bits.mce ) );
    RAdd( RK_SUBITEM, "Time stamp counter",         YesNo( g_cpu.cpu_features1.bits.tsc ) );
    RAdd( RK_SUBITEM, "CMPXCHG8B instruction",      YesNo( g_cpu.cpu_features1.bits.cx8 ) );
    RAdd( RK_SUBITEM, "Page size extension",        YesNo( g_cpu.cpu_features1.bits.pse ) );
    RAdd( RK_SUBITEM, "Conditional move",           YesNo( g_cpu.cpu_features1.bits.cmov ) );
    RAdd( RK_SUBITEM, "On-chip APIC",               YesNo( g_cpu.cpu_features1.bits.apic ) );

    RBlank();
    RHeading( "Instruction sets" );
    RAdd( RK_SUBITEM, "Intel MMX",          YesNo( g_cpu.cpu_features1.bits.mmx ) );
    RAdd( RK_SUBITEM, "AMD extended MMX",   YesNo( g_cpu.amd_features1.bits.mmxext ) );
    RAdd( RK_SUBITEM, "AMD 3DNow!",         YesNo( g_cpu.amd_features1.bits.threednow ) );
    RAdd( RK_SUBITEM, "AMD enhanced 3DNow!",YesNo( g_cpu.amd_features1.bits.threednowext ) );
    RAdd( RK_SUBITEM, "Intel SSE",          YesNo( g_cpu.cpu_features1.bits.sse ) );
    RAdd( RK_SUBITEM, "Intel SSE2",         YesNo( g_cpu.cpu_features1.bits.sse2 ) );
    RAdd( RK_SUBITEM, "Intel SSE3",         YesNo( g_cpu.cpu_features2.bits.sse3 ) );

    if ( g_cpu_mhz == 0 ) {
        RBlank();
        RNote( "Clock speed needs the time stamp counter." );
        RNote( "This processor does not have one." );
    }

    if ( g_cpu.processor_family == 5 && g_cpu.coprocessor_family != 0 ) {
        RBlank();
        RHeading( "Errata" );
        RAdd( RK_SUBITEM, "FDIV bug", YesNo( HasFdivBug() ) );
    }
}

/*---------------------------------------------------------------------------
 * Memory
 *
 * OS/2 has no "free memory" figure, and the honest page does not invent one.
 * What it reports is how much of physical memory is RESIDENT - locked down
 * by the kernel and drivers, never paged out - and how much could still be
 * allocated, which counts the swap file's room to grow and so routinely
 * exceeds the memory fitted.
 *-------------------------------------------------------------------------*/

static void BuildMemory( void )
{
    RHeading( "In use now" );
    if ( g_mem.known ) {
        RGauge( "Resident memory", ByteSize( (double)g_mem.resident ),
                Percent( (double)g_mem.resident, (double)g_mem.total_phys ) );
    }
    if ( g_swap.file_known && g_swap.drive_known ) {
        /* room to grow: what is free on the drive less what SWAPPATH says
           must be left there */
        double room = g_swap.drive_free - (double)g_swap.minfree_kb * 1024.0;

        if ( room < 0.0 ) room = 0.0;
        RGauge( "Swap file", ByteSize( g_swap.file_bytes ),
                Percent( g_swap.file_bytes, g_swap.file_bytes + room ) );
    }
    if ( !g_mem.known && !g_swap.file_known ) {
        RNote( "This version of OS/2 reports no memory figures." );
    }

    if ( g_mem.known ) {
        RBlank();
        RHeading( "OS/2 memory" );
        RFmt( RK_ITEM, "Physical memory",   "%s", ByteSize( (double)g_mem.total_phys ) );
        RFmt( RK_ITEM, "Resident",          "%s", ByteSize( (double)g_mem.resident ) );
        RFmt( RK_ITEM, "Available to programs", "%s", ByteSize( (double)g_mem.avail ) );
        RFmt( RK_ITEM, "Largest private block", "%s", ByteSize( (double)g_mem.max_private ) );
        RFmt( RK_ITEM, "Largest shared block",  "%s", ByteSize( (double)g_mem.max_shared ) );
        if ( g_mem.high_known && ( g_mem.max_high_private != 0 || g_mem.max_high_shared != 0 ) ) {
            RFmt( RK_ITEM, "High memory, private", "%s",
                  ByteSize( (double)g_mem.max_high_private ) );
            RFmt( RK_ITEM, "High memory, shared", "%s",
                  ByteSize( (double)g_mem.max_high_shared ) );
        }
        if ( g_mem.va_limit_mb != 0 )
            RFmt( RK_ITEM, "Virtual address limit", "%s MB", Comma( g_mem.va_limit_mb ) );
        RFmt( RK_ITEM, "Page size", "%s", Comma( g_page_size ) );
    }

    RBlank();
    RHeading( "Swap file" );
    if ( g_swap.known ) {
        RAdd( RK_ITEM, "Directory", g_swap.directory );
        if ( g_swap.file_known )
            RFmt( RK_ITEM, "SWAPPER.DAT", "%s", ByteSize( g_swap.file_bytes ) );
        if ( g_swap.initial_kb != 0 )
            RFmt( RK_ITEM, "Initial size", "%s", KBytes( g_swap.initial_kb ) );
        RFmt( RK_ITEM, "Kept free on the drive", "%s", KBytes( g_swap.minfree_kb ) );
        if ( g_swap.drive_known )
            RFmt( RK_ITEM, "Free on the drive", "%s", ByteSize( g_swap.drive_free ) );
    }
    else {
        RNote( "There is no SWAPPATH line in CONFIG.SYS." );
    }

    if ( g_mem.known ) {
        RBlank();
        RNote( "\"Available to programs\" counts the swap file's room" );
        RNote( "to grow, so it can be larger than the memory fitted." );
    }
}

/*---------------------------------------------------------------------------
 * First Megabyte - the map, and the legend the map needs
 *-------------------------------------------------------------------------*/

static void BuildFirstMeg( void )
{
    ULONG counts[ MM_CLASS_COUNT ];
    int   i;

    RHeading( "Memory below 1 MB" );

    if ( !g_rom ) {
        RNote( "The adapter and BIOS area can only be read through the" );
        RNote( "TESTCFG.SYS driver, and it did not answer.  Check that" );
        RNote( "CONFIG.SYS has DEVICE=...\\OS2\\BOOT\\TESTCFG.SYS." );
        return;
    }

    for ( i = 0; i < MM_CLASS_COUNT; ++i ) counts[ i ] = 0;
    for ( i = 0; i < MEMMAP_BLOCKS; ++i ) {
        if ( g_memmap[ i ] < MM_CLASS_COUNT ) counts[ g_memmap[ i ] ]++;
    }

    RAdd( RK_MEMMAP, "", "" );

    RBlank();
    RHeading( "Totals" );
    RFmt( RK_ITEM, "OS/2, drivers and programs", "%s",
          KBytes( ( counts[ MM_SYSTEM ] + counts[ MM_PROGRAM ] ) * 4UL ) );
    RFmt( RK_ITEM, "Video adapter", "%s",     KBytes( counts[ MM_VIDEO ] * 4UL ) );
    RFmt( RK_ITEM, "Adapter ROM", "%s",       KBytes( counts[ MM_ADAPTER_ROM ] * 4UL ) );
    RFmt( RK_ITEM, "Upper memory area", "%s", KBytes( counts[ MM_UPPER ] * 4UL ) );
    RFmt( RK_ITEM, "System ROM BIOS", "%s",   KBytes( counts[ MM_SYSTEM_ROM ] * 4UL ) );

    RBlank();
    RNote( "Each cell is 4 KB.  Conventional memory belongs to OS/2" );
    RNote( "itself, so all of it is shown in use." );
}

/*---------------------------------------------------------------------------
 * Display
 *-------------------------------------------------------------------------*/

static void BuildDisplay( void )
{
    rv_display_t d;
    char         driver[ 64 ];
    const char*  vio;

    ReadDisplay( &d );

    RHeading( "Current mode" );
    RFmt( RK_ITEM, "Resolution", "%ld x %ld pixels", d.width, d.height );
    AddColours( &d );
    if ( d.planes > 0 )
        RFmt( RK_ITEM, "Colour depth", "%ld plane(s) x %ld bit(s)", d.planes, d.bits );
    if ( d.font_dpi_x > 0 )
        RFmt( RK_ITEM, "Font resolution", "%ld x %ld dots per inch",
              d.font_dpi_x, d.font_dpi_y );
    if ( d.ppm_x > 0 && d.ppm_y > 0 )
        RFmt( RK_ITEM, "Display size", "%ld x %ld mm",
              d.width * 1000L / d.ppm_x, d.height * 1000L / d.ppm_y );

    RBlank();
    RHeading( "Display drivers" );
    if ( DisplayDriverName( driver, sizeof( driver ) ) )
        RAdd( RK_ITEM, "Presentation Manager", driver );
    vio = ConfigFind( "SET VIDEO_DEVICES" );
    if ( vio != NULL ) {
        char name[ 32 ];

        RAdd( RK_ITEM, "Text mode (VIO)", vio );

        /* SET VIO_SVGA=DEVICE(BVHVGA,BVHSVGA) names the handlers behind it */
        sprintf( name, "SET %.24s", vio );
        vio = ConfigFind( name );
        if ( vio != NULL ) RAdd( RK_ITEM, "Video handlers", vio );
    }
    RAdd( RK_ITEM, "Primary adapter", g_adapter ? "Colour" : "Monochrome" );

    if ( g_video_rom_vendor[ 0 ] != '\0' || g_video_rom_version[ 0 ] != '\0' ) {
        RBlank();
        RHeading( "Adapter ROM" );
        if ( g_video_rom_vendor[ 0 ] != '\0' )
            RAdd( RK_ITEM, "Vendor", g_video_rom_vendor );
        if ( g_video_rom_version[ 0 ] != '\0' )
            RAdd( RK_ITEM, "ROM version", g_video_rom_version );
    }
}

/*---------------------------------------------------------------------------
 * Drives
 *-------------------------------------------------------------------------*/

enum drive_kind_t { DK_DISKETTE, DK_LOCAL, DK_CDROM, DK_REMOTE };

/*---------------------------------------------------------------------------
 * DriveAttach - which file system is attached, and is it local?
 *
 * DosQueryFSAttach reports the attachment, not the volume, so it answers
 * without touching the media.  'data', when given, receives the attach data
 * if it is printable text - which for a redirected drive is usually the
 * \\server\share it is connected to.
 *-------------------------------------------------------------------------*/

static BOOL DriveAttach( int drive, int* kind, char* fsd, int fsd_size,
                         char* data, int data_size )
{
    BYTE        buf[ sizeof( FSQBUFFER2 ) + 3 * CCHMAXPATH ];
    PFSQBUFFER2 pfsq = (PFSQBUFFER2)buf;
    ULONG       cb   = sizeof( buf );
    char        dev[ 3 ];
    const char* name;

    dev[ 0 ] = (char)( 'A' + drive );
    dev[ 1 ] = ':';
    dev[ 2 ] = '\0';
    fsd[ 0 ] = '\0';
    if ( data != NULL ) data[ 0 ] = '\0';

    memset( buf, 0, sizeof( buf ) );
    if ( DosQueryFSAttach( (PCSZ)dev, 0, FSAIL_QUERYNAME, pfsq, &cb ) != NO_ERROR ) {
        *kind = ( drive < 2 ) ? DK_DISKETTE : DK_LOCAL;
        return FALSE;
    }

    /* the three names follow one another in the packed tail */
    name = (const char*)pfsq->szName + pfsq->cbName + 1;
    CopyStr( fsd, name, fsd_size );

    if ( data != NULL && pfsq->cbFSAData > 0 ) {
        const char* attach = name + pfsq->cbFSDName + 1;
        int         n      = (int)pfsq->cbFSAData;
        int         i;
        BOOL        printable = TRUE;

        if ( n > data_size - 1 ) n = data_size - 1;
        for ( i = 0; i < n && attach[ i ] != '\0'; ++i ) {
            if ( (unsigned char)attach[ i ] < 0x20 || (unsigned char)attach[ i ] > 0x7E )
                printable = FALSE;
        }
        if ( printable && i > 0 ) {
            memcpy( data, attach, (size_t)i );
            data[ i ] = '\0';
        }
    }

    if ( pfsq->iType == FSAT_REMOTEDRV )      *kind = DK_REMOTE;
    else if ( stricmp( fsd, "CDFS" ) == 0 )   *kind = DK_CDROM;
    else if ( drive < 2 )                     *kind = DK_DISKETTE;
    else                                      *kind = DK_LOCAL;
    return TRUE;
}

static BOOL DriveSpace( int drive, double* total, double* freespace )
{
    FSALLOCATE fsa;
    double     unit;

    if ( DosQueryFSInfo( (ULONG)( drive + 1 ), FSIL_ALLOC, &fsa, sizeof( fsa ) ) != NO_ERROR )
        return FALSE;

    unit       = (double)fsa.cSectorUnit * (double)fsa.cbSector;
    *total     = (double)fsa.cUnit * unit;
    *freespace = (double)fsa.cUnitAvail * unit;
    return TRUE;
}

/*---------------------------------------------------------------------------
 * AddFixedDisks - the physical disks, sized from their geometry
 *
 * DosPhysicalDisk counts them and hands out a handle per disk for the
 * physical-disk IOCtls.  The geometry's cylinder count is a USHORT, so a
 * disk past 8 GB shows the size the BIOS geometry can describe - which is
 * what OS/2 2.x and Warp themselves address through it.
 *-------------------------------------------------------------------------*/

static void AddFixedDisks( void )
{
    USHORT disks = 0;
    int    unit;

    if ( DosPhysicalDisk( INFO_COUNT_PARTITIONABLE_DISKS, &disks, sizeof( disks ),
                          NULL, 0 ) != NO_ERROR ) {
        RNote( "The disk count is not available." );
        return;
    }

    RFmt( RK_ITEM, "Physical drives", "%u", (unsigned)disks );

    for ( unit = 1; unit <= (int)disks && unit <= 24; ++unit ) {
        char                    spec[ 8 ];
        USHORT                  handle = 0;
        BYTE                    command = 0;
        ULONG                   parm_len = sizeof( command );
        ULONG                   data_len;
        DEVICEPARAMETERBLOCK    dpb;
        char                    label[ 16 ];

        sprintf( spec, "%d:", unit );
        if ( DosPhysicalDisk( INFO_GETIOCTLHANDLE, &handle, sizeof( handle ),
                              spec, (ULONG)strlen( spec ) + 1 ) != NO_ERROR )
            continue;

        memset( &dpb, 0, sizeof( dpb ) );
        data_len = sizeof( dpb );
        if ( DosDevIOCtl( (HFILE)handle, IOCTL_PHYSICALDISK, PDSK_GETPHYSDEVICEPARAMS,
                          &command, sizeof( command ), &parm_len,
                          &dpb, sizeof( dpb ), &data_len ) == NO_ERROR ) {
            double bytes = (double)dpb.cCylinders * (double)dpb.cHeads
                           * (double)dpb.cSectorsPerTrack * 512.0;

            sprintf( label, "Drive %d", unit );
            RFmt( RK_SUBITEM, label, "%s  (%u cylinders, %u heads, %u sectors)",
                  ByteSize( bytes ), (unsigned)dpb.cCylinders,
                  (unsigned)dpb.cHeads, (unsigned)dpb.cSectorsPerTrack );
        }

        DosPhysicalDisk( INFO_FREEIOCTLHANDLE, NULL, 0, &handle, sizeof( handle ) );
    }
}

static void BuildDrives( void )
{
    ULONG   current = 0;
    ULONG   map     = 0;
    int     drive;
    int     found   = 0;
    int     cdroms  = 0;
    int     unit;

    /* Total and Free are sizes: right-justified, so the figures line up
       under their headings with the units in one column */
    const int size_cols = RIGHT_COL( 3 ) | RIGHT_COL( 4 );

    RHeading( "Drive letters" );
    RColumns( RK_COLHDR, size_cols, "Drive\tType\tFile system\tTotal\tFree" );

    if ( DosQueryCurrentDisk( &current, &map ) != NO_ERROR ) map = 0;

    for ( drive = 0; drive < 26; ++drive ) {
        int         kind;
        char        fsd[ 16 ];
        const char* type_name;
        double      total, freespace;
        char        line[ RPT_VALUE_MAX ];

        if ( !( map & ( 1UL << drive ) ) ) continue;

        /* the phantom B: of a one-diskette machine - see DosDevConfig */
        if ( drive == 1 && g_floppies < 2 ) continue;

        DriveAttach( drive, &kind, fsd, sizeof( fsd ), NULL, 0 );
        ++found;

        switch ( kind ) {
            case DK_DISKETTE: type_name = "Diskette"; break;
            case DK_CDROM:    type_name = "CD-ROM";   ++cdroms; break;
            case DK_REMOTE:   type_name = "Network";  break;
            default:          type_name = "Local";    break;
        }

        if ( DriveSpace( drive, &total, &freespace ) && total > 0.0 ) {
            sprintf( line, "%c:\t%s\t%s\t%s\t%s", (char)( 'A' + drive ), type_name,
                     fsd[ 0 ] ? fsd : "-", ByteSize( total ), ByteSize( freespace ) );
        }
        else {
            sprintf( line, "%c:\t%s\t%s\t-\t-", (char)( 'A' + drive ), type_name,
                     fsd[ 0 ] ? fsd : "-" );
        }
        RColumns( RK_COLROW, size_cols, line );
    }

    if ( found == 0 ) RNote( "No drives reported." );

    RBlank();
    RHeading( "Diskette drives" );
    for ( unit = 0; unit < 2; ++unit ) {
        char label[ 16 ];

        sprintf( label, "Drive %c:", (char)( 'A' + unit ) );
        RAdd( RK_ITEM, label, ( unit < (int)g_floppies ) ? "Installed" : "Not present" );
    }

    RBlank();
    RHeading( "Fixed disks" );
    AddFixedDisks();

    if ( cdroms > 0 ) {
        RBlank();
        RHeading( "CD-ROM" );
        RFmt( RK_ITEM, "CD-ROM drives", "%d", cdroms );
    }
}

/*---------------------------------------------------------------------------
 * Network
 *
 * The LAN Requester (and Peer, which is the same code) keeps its settings in
 * \IBMLAN\IBMLAN.INI on the boot drive; TCP/IP puts the host name in the
 * environment from CONFIG.SYS.  Either may be missing, and the page says
 * only what it found.
 *-------------------------------------------------------------------------*/

static BOOL LanIniValue( FILE* fp, const char* key, char* out, int size )
{
    char line[ 256 ];

    out[ 0 ] = '\0';
    fseek( fp, 0L, SEEK_SET );

    while ( fgets( line, sizeof( line ), fp ) != NULL ) {
        char* p = (char*)SkipSpace( line );
        char* eq;
        char  kw[ 48 ];
        int   len = 0;

        eq = strchr( p, '=' );
        if ( eq == NULL || *p == ';' ) continue;

        while ( p < eq && len < (int)sizeof( kw ) - 1 && *p != ' ' && *p != '\t' )
            kw[ len++ ] = *p++;
        kw[ len ] = '\0';

        if ( stricmp( kw, key ) == 0 ) {
            CopyStr( out, SkipSpace( eq + 1 ), size );
            TrimRight( out );
            return out[ 0 ] != '\0';
        }
    }
    return FALSE;
}

static void BuildNetwork( void )
{
    char    path[ 32 ];
    char    value[ 128 ];
    FILE*   fp;
    PSZ     env = NULL;
    ULONG   current = 0;
    ULONG   map = 0;
    int     drive;
    int     connections = 0;
    BOOL    any = FALSE;

    RHeading( "Network software" );

    sprintf( path, "%c:\\IBMLAN\\IBMLAN.INI", BootLetter() );
    fp = fopen( path, "r" );
    if ( fp != NULL ) {
        RAdd( RK_ITEM, "LAN Requester", "Installed" );
        if ( LanIniValue( fp, "computername", value, sizeof( value ) ) )
            RAdd( RK_ITEM, "Computer name", value );
        if ( LanIniValue( fp, "domain", value, sizeof( value ) ) )
            RAdd( RK_ITEM, "Domain", value );
        fclose( fp );
        any = TRUE;
    }

    if ( DosScanEnv( (PCSZ)"HOSTNAME", &env ) == NO_ERROR && env != NULL && *env != '\0' ) {
        RAdd( RK_ITEM, "TCP/IP host name", (const char*)env );
        any = TRUE;
    }
    if ( DosScanEnv( (PCSZ)"USER", &env ) == NO_ERROR && env != NULL && *env != '\0' ) {
        RAdd( RK_ITEM, "User name", (const char*)env );
    }

    if ( !any ) RNote( "No LAN Requester or TCP/IP configuration was found." );

    RBlank();
    RHeading( "Redirected drives" );
    RAdd( RK_COLHDR, "", "Drive\tFile system\tConnected to" );

    if ( DosQueryCurrentDisk( &current, &map ) != NO_ERROR ) map = 0;

    for ( drive = 2; drive < 26; ++drive ) {
        int  kind;
        char fsd[ 16 ];
        char remote[ 64 ];
        char line[ RPT_VALUE_MAX ];

        if ( !( map & ( 1UL << drive ) ) ) continue;
        if ( !DriveAttach( drive, &kind, fsd, sizeof( fsd ), remote, sizeof( remote ) ) )
            continue;
        if ( kind != DK_REMOTE ) continue;

        sprintf( line, "%c:\t%s\t%s", (char)( 'A' + drive ), fsd,
                 remote[ 0 ] ? remote : "(connected)" );
        RAdd( RK_COLROW, "", line );
        ++connections;
    }

    if ( connections == 0 ) RNote( "No redirected drives." );
}

/*---------------------------------------------------------------------------
 * Ports
 *-------------------------------------------------------------------------*/

/* Multimedia device counts, through MCI's string interface - which needs no
   structure layouts, only strings.  MDM.DLL is loaded by name, and only when
   MMPM/2 is installed (MMBASE is set), so Reveal still starts on a system
   without it. */
typedef ULONG ( APIENTRY *PFN_MCISENDSTRING )( PSZ, PSZ, USHORT, HWND, USHORT );

static HMODULE              hMdm = NULLHANDLE;
static PFN_MCISENDSTRING    pMciSendString = NULL;
static BOOL                 mdm_tried = FALSE;

static BOOL MciAvailable( void )
{
    PSZ  env = NULL;
    PFN  pfn = NULL;
    char failed[ 16 ];

    if ( mdm_tried ) return pMciSendString != NULL;
    mdm_tried = TRUE;

    if ( DosScanEnv( (PCSZ)"MMBASE", &env ) != NO_ERROR || env == NULL ) return FALSE;
    if ( DosLoadModule( (PSZ)failed, sizeof( failed ), (PCSZ)"MDM", &hMdm ) != NO_ERROR )
        return FALSE;
    if ( DosQueryProcAddr( hMdm, 0, (PCSZ)"mciSendString", &pfn ) != NO_ERROR ) {
        DosFreeModule( hMdm );
        hMdm = NULLHANDLE;
        return FALSE;
    }
    pMciSendString = (PFN_MCISENDSTRING)pfn;
    return TRUE;
}

static BOOL MciCount( const char* device, ULONG* count )
{
    char command[ 64 ];
    char answer[ 32 ];

    if ( pMciSendString == NULL ) return FALSE;

    sprintf( command, "sysinfo %s quantity wait", device );
    answer[ 0 ] = '\0';
    if ( pMciSendString( (PSZ)command, (PSZ)answer, (USHORT)sizeof( answer ),
                         NULLHANDLE, 0 ) != 0 )
        return FALSE;

    *count = strtoul( answer, NULL, 10 );
    return TRUE;
}

static void BuildPorts( void )
{
    const char* v;
    ULONG       cp[ 8 ];
    ULONG       cb = 0;
    COUNTRYCODE cc;
    COUNTRYINFO ci;
    ULONG       ci_len = 0;
    ULONG       n;

    RHeading( "Serial ports" );
    RFmt( RK_ITEM, "Ports reported", "%u", (unsigned)g_serial );
    for ( n = 0; n < g_serial && n < 8; ++n ) {
        char label[ 8 ];
        sprintf( label, "COM%lu", n + 1 );
        RAdd( RK_SUBITEM, label, "Installed" );
    }
    v = ConfigDriverLine( "DEVICE", "COM.SYS" );
    if ( v != NULL ) RAdd( RK_ITEM, "Driver", BaseName( v ) );

    RBlank();
    RHeading( "Parallel ports" );
    RFmt( RK_ITEM, "Ports reported", "%u", (unsigned)g_parallel );
    for ( n = 0; n < g_parallel && n < 4; ++n ) {
        char label[ 8 ];
        sprintf( label, "LPT%lu", n + 1 );
        RAdd( RK_SUBITEM, label, "Installed" );
    }

    RBlank();
    RHeading( "Pointing device" );
    RAdd( RK_ITEM, "Mouse present",
          YesNo( WinQuerySysValue( HWND_DESKTOP, SV_MOUSEPRESENT ) ) );
    if ( WinQuerySysValue( HWND_DESKTOP, SV_MOUSEPRESENT ) ) {
        LONG buttons = WinQuerySysValue( HWND_DESKTOP, SV_CMOUSEBUTTONS );

        if ( buttons > 0 ) RFmt( RK_ITEM, "Buttons", "%ld", buttons );
        RAdd( RK_ITEM, "Buttons swapped",
              YesNo( WinQuerySysValue( HWND_DESKTOP, SV_SWAPBUTTON ) ) );
    }
    v = ConfigDriverLine( "DEVICE", "MOUSE.SYS" );
    if ( v != NULL ) RAdd( RK_ITEM, "Driver", v );

    RBlank();
    RHeading( "Keyboard and country" );
    {
        int line;

        /* DEVINFO=KBD,UK,C:\OS2\KEYBOARD.DCP - the second field is the layout */
        for ( line = 0; line < cfg_lines; ++line ) {
            v = ConfigValue( line, "DEVINFO" );
            if ( v != NULL && strnicmp( v, "KBD,", 4 ) == 0 ) {
                char layout[ 16 ];
                int  len = 0;

                v += 4;
                while ( v[ len ] != '\0' && v[ len ] != ',' && len < (int)sizeof( layout ) - 1 ) {
                    layout[ len ] = v[ len ];
                    ++len;
                }
                layout[ len ] = '\0';
                if ( layout[ 0 ] != '\0' ) RAdd( RK_ITEM, "Keyboard layout", layout );
                break;
            }
        }
    }

    if ( DosQueryCp( sizeof( cp ), cp, &cb ) == NO_ERROR && cb >= sizeof( ULONG ) ) {
        char list[ 64 ];
        ULONG i;

        RFmt( RK_ITEM, "Code page", "%lu", cp[ 0 ] );
        list[ 0 ] = '\0';
        for ( i = 1; i < cb / sizeof( ULONG ) && i < 8; ++i ) {
            char one[ 12 ];
            sprintf( one, ( i > 1 ) ? ", %lu" : "%lu", cp[ i ] );
            strcat( list, one );
        }
        if ( list[ 0 ] != '\0' ) RAdd( RK_ITEM, "Prepared code pages", list );
    }

    memset( &cc, 0, sizeof( cc ) );
    memset( &ci, 0, sizeof( ci ) );
    if ( DosQueryCtryInfo( sizeof( ci ), &cc, &ci, &ci_len ) == NO_ERROR )
        RFmt( RK_ITEM, "Country code", "%lu", ci.country );

    RBlank();
    RHeading( "Multimedia" );
    if ( MciAvailable() ) {
        static const struct { const char* device; const char* label; } mci[] = {
            { "waveaudio", "Waveform audio devices" },
            { "sequencer", "MIDI sequencers" },
            { "cdaudio",   "CD audio devices" },
            { "ampmix",    "Amplifier-mixers" }
        };
        int i;

        for ( i = 0; i < (int)( sizeof( mci ) / sizeof( mci[ 0 ] ) ); ++i ) {
            ULONG count;
            if ( MciCount( mci[ i ].device, &count ) )
                RFmt( RK_ITEM, mci[ i ].label, "%lu", count );
        }
    }
    else {
        RNote( "MMPM/2 is not installed." );
    }
}

/*---------------------------------------------------------------------------
 * OS/2 - the version, the system limits, what is running, what is loaded
 *
 * "What is running" is the Window List - WinQuerySwitchList, which every
 * PM there has ever been can answer, and which is what the user sees on
 * Ctrl+Esc.  DosQuerySysState would list every process, but its buffer
 * layout differs between releases and a wrong guess reads wild pointers.
 *-------------------------------------------------------------------------*/

#define MAX_PROGRAM_ROWS    100
#define MAX_DRIVER_ROWS     150

static const char* ProgramType( ULONG type )
{
    switch ( type ) {
        case PROG_PM:                   return "PM";
        case PROG_WINDOWABLEVIO:        return "OS/2 win";
        case PROG_FULLSCREEN:           return "OS/2 full";
        case PROG_VDM:                  return "DOS full";
        case PROG_WINDOWEDVDM:          return "DOS win";
        case PROG_WINDOW_REAL:
        case PROG_WINDOW_PROT:
        case PROG_WINDOW_AUTO:
        case PROG_31_STDSEAMLESSVDM:
        case PROG_31_STDSEAMLESSCOMMON:
        case PROG_31_ENHSEAMLESSVDM:
        case PROG_31_ENHSEAMLESSCOMMON:
        case PROG_31_ENH:
        case PROG_31_STD:               return "WIN-OS/2";
        default:                        break;
    }
    return "Other";
}

static void AddWindowList( void )
{
    ULONG       count;
    ULONG       bytes;
    PSWBLOCK    block;
    ULONG       i;
    int         shown = 0;

    count = WinQuerySwitchList( hab, NULL, 0 );
    if ( count == 0 ) {
        RNote( "The Window List is empty." );
        return;
    }

    bytes = sizeof( SWBLOCK ) + count * sizeof( SWENTRY );
    block = (PSWBLOCK)malloc( bytes );
    if ( block == NULL ) return;

    count = WinQuerySwitchList( hab, block, bytes );

    RAdd( RK_COLHDR, "", "Process\tType\tTitle" );
    for ( i = 0; i < count && i < block->cswentry && shown < MAX_PROGRAM_ROWS; ++i ) {
        const SWCNTRL* sw = &block->aswentry[ i ].swctl;
        char           line[ RPT_VALUE_MAX ];

        if ( sw->uchVisibility != SWL_VISIBLE ) continue;
        if ( sw->szSwtitle[ 0 ] == '\0' ) continue;

        _snprintf( line, sizeof( line ) - 1, "%lu\t%s\t%s", (ULONG)sw->idProcess,
                   ProgramType( sw->bProgType ), sw->szSwtitle );
        line[ sizeof( line ) - 1 ] = '\0';
        RAdd( RK_COLROW, "", line );
        ++shown;
    }
    free( block );

    RFmt( RK_ITEM, "Programs listed", "%d", shown );
}

static void BuildOs2( void )
{
    ULONG v;

    RHeading( "Version" );
    RAdd( RK_ITEM, "Operating system", OsName() );
    RFmt( RK_ITEM, "Version", "%lu.%lu", g_ver_major, g_ver_minor );
    if ( g_ver_rev != 0 ) RFmt( RK_ITEM, "Revision", "%lu", g_ver_rev );
    RFmt( RK_ITEM, "Boot drive", "%c:", BootLetter() );

    RBlank();
    RHeading( "System limits" );
    if ( SysValue( QSV_MAX_PATH_LENGTH, &v ) )   RFmt( RK_SUBITEM, "Longest path", "%lu", v );
    if ( SysValue( QSV_MAX_TEXT_SESSIONS, &v ) ) RFmt( RK_SUBITEM, "Text sessions", "%lu", v );
    if ( SysValue( QSV_MAX_PM_SESSIONS, &v ) )   RFmt( RK_SUBITEM, "PM sessions", "%lu", v );
    if ( SysValue( QSV_MAX_VDM_SESSIONS, &v ) )  RFmt( RK_SUBITEM, "DOS sessions", "%lu", v );
    if ( SysValue( QSV_MAXPROCESSES, &v ) )      RFmt( RK_SUBITEM, "Processes", "%lu", v );

    RBlank();
    RHeading( "Scheduler" );
    if ( SysValue( QSV_MIN_SLICE, &v ) )  RFmt( RK_SUBITEM, "Shortest time slice", "%lu ms", v );
    if ( SysValue( QSV_MAX_SLICE, &v ) )  RFmt( RK_SUBITEM, "Longest time slice", "%lu ms", v );
    if ( SysValue( QSV_TIMER_INTERVAL, &v ) )
        RFmt( RK_SUBITEM, "Timer interval", "%lu.%lu ms", v / 10, v % 10 );
    if ( SysValue( QSV_DYN_PRI_VARIATION, &v ) )
        RAdd( RK_SUBITEM, "Dynamic priorities", YesNo( v ) );
    if ( SysValue( QSV_MAX_WAIT, &v ) )   RFmt( RK_SUBITEM, "Starvation boost after", "%lu s", v );

    RBlank();
    RHeading( "Window List" );
    AddWindowList();

    if ( cfg_lines > 0 ) {
        int n;

        RBlank();
        RHeading( "Base device drivers" );
        n = AddDriverList( "BASEDEV", MAX_DRIVER_ROWS );
        if ( n == 0 ) RNote( "None in CONFIG.SYS." );

        RBlank();
        RHeading( "Installable file systems" );
        n = AddDriverList( "IFS", MAX_DRIVER_ROWS );
        if ( n == 0 ) RNote( "None in CONFIG.SYS." );

        RBlank();
        RHeading( "Device drivers" );
        n = AddDriverList( "DEVICE", MAX_DRIVER_ROWS );
        if ( n == 0 ) RNote( "None in CONFIG.SYS." );
    }
    else {
        RBlank();
        RNote( "CONFIG.SYS could not be read, so its drivers are not listed." );
    }
}

/*---------------------------------------------------------------------------
 * Environment
 *-------------------------------------------------------------------------*/

static void BuildEnvironment( void )
{
    PTIB        ptib = NULL;
    PPIB        ppib = NULL;
    const char* env;
    const char* v;
    char        path[ CCHMAXPATH ];
    int         shown = 0;

    RHeading( "Operating system" );
    RAdd( RK_ITEM, "OS/2", OsLine() );
    sprintf( path, "%c:\\OS2", BootLetter() );
    RAdd( RK_ITEM, "OS/2 directory", path );

    DosGetInfoBlocks( &ptib, &ppib );
    if ( ppib != NULL &&
         DosQueryModuleName( ppib->pib_hmte, sizeof( path ), path ) == NO_ERROR )
        RAdd( RK_ITEM, "This program", path );

    v = ConfigFind( "LIBPATH" );
    if ( v != NULL ) RAdd( RK_ITEM, "LIBPATH", v );

    RBlank();
    RHeading( "Environment" );

    /* A run of NUL-terminated NAME=VALUE strings closed by an empty one. */
    env = ( ppib != NULL ) ? ppib->pib_pchenv : NULL;
    if ( env == NULL ) {
        RNote( "The environment block is not available." );
        return;
    }

    while ( *env != '\0' && shown < 150 ) {
        char        entry[ RPT_VALUE_MAX ];
        char        name[ RPT_LABEL_MAX ];
        char*       eq;

        CopyStr( entry, env, sizeof( entry ) );

        eq = strchr( entry, '=' );
        if ( eq != NULL ) {
            *eq = '\0';
            CopyStr( name, entry, sizeof( name ) );
            RAdd( RK_ITEM, name, eq + 1 );
        }
        else {
            RAdd( RK_ITEM, "", entry );
        }
        ++shown;

        env += strlen( env ) + 1;
    }

    if ( shown == 0 ) RNote( "The environment is empty." );
}

/*===========================================================================
 * Benchmark
 *
 * The run happens on a worker thread (COMMON\PMWORKER.C).  The contract:
 *
 *   * the body makes no PM calls - RunBenchmark is arithmetic and clock();
 *   * it writes only g_bench_work, which the UI thread never reads while it
 *     runs; RvBenchmarkPoll copies it into g_bench once the worker is done;
 *   * a cancel (Reveal closing) ends it within one batch, and the scores of a
 *     cancelled run are thrown away.
 *=========================================================================*/

static PMWORKER             g_worker;
static BOOL                 g_worker_ready = FALSE;
static benchmark_results_t  g_bench_work;
static BOOL                 g_bench_has_fpu = FALSE;

static void BenchmarkBody( void* arg )
{
    (void)arg;

    memset( &g_bench_work, 0, sizeof( g_bench_work ) );
    RunBenchmark( &g_bench_work, g_bench_has_fpu,
                  (const volatile unsigned long*)&g_worker.fCancel );

    PmWorkerEnd( &g_worker, 0 );
}

BOOL RvBenchmarkStart( HWND hwndNotify )
{
    if ( !g_worker_ready ) {
        PmWorkerInit( &g_worker );
        g_worker_ready = TRUE;
    }
    if ( PmWorkerIsBusy( &g_worker ) ) return FALSE;

    g_bench_has_fpu = ( g_cpu.coprocessor_family != 0 );
    return PmWorkerStart( &g_worker, BenchmarkBody, NULL, PMWORKER_STACK, hwndNotify );
}

BOOL RvBenchmarkRunning( void )
{
    return g_worker_ready && PmWorkerIsBusy( &g_worker );
}

BOOL RvBenchmarkPoll( void )
{
    ULONG result    = 0;
    BOOL  cancelled = FALSE;

    if ( !g_worker_ready ) return FALSE;
    if ( !PmWorkerPollDone( &g_worker, &result, &cancelled ) ) return FALSE;

    if ( !cancelled ) {
        g_bench      = g_bench_work;
        g_bench_done = TRUE;
    }
    return TRUE;
}

BOOL RvBenchmarkStop( ULONG ms )
{
    if ( !g_worker_ready || !PmWorkerIsBusy( &g_worker ) ) return TRUE;
    return PmWorkerWait( &g_worker, ms );
}

BOOL RvBenchmarkDone( void )
{
    return g_bench_done;
}

/*---------------------------------------------------------------------------
 * The three scores are reported as the measurements they are, in their own
 * units - see the Win16 build for why they are not gauges.
 *-------------------------------------------------------------------------*/

static void BuildBenchmark( void )
{
    RHeading( "Results" );

    if ( RvBenchmarkRunning() ) {
        RNote( "The benchmark is running.  It takes about six seconds." );
        RBlank();
        RNote( "Reveal can be used meanwhile, but anything else the" );
        RNote( "processor is given to do will lower the scores." );
        return;
    }

    if ( !g_bench_done ) {
        RNote( "This machine has not been measured yet." );
        RBlank();
        RNote( "The tests run for about two seconds each.  Other programs" );
        RNote( "running at the same time will lower the scores." );
        RBlank();
        /* The pane turns this into a real push button.  Same work as
           View, Run Benchmark (F9). */
        RAdd( RK_BUTTON, "Run Benchmark Now", "" );
        return;
    }

    RFmt( RK_ITEM, "Integer",        "%s KIPS",   Comma( g_bench.cpu_kips ) );
    if ( g_cpu.coprocessor_family != 0 )
        RFmt( RK_ITEM, "Floating point", "%s KFLOPS", Comma( g_bench.fpu_kflops ) );
    else
        RAdd( RK_ITEM, "Floating point", "No coprocessor" );
    RFmt( RK_ITEM, "Memory",         "%s KOPS",   Comma( g_bench.mem_kops ) );

    RBlank();
    RAdd( RK_ITEM, "Processor", ProcessorName() );
    if ( g_cpu_mhz > 0 ) RFmt( RK_ITEM, "Clock speed", "%lu MHz", g_cpu_mhz );

    RBlank();
    RNote( "32-bit scores: compare them with the 32-bit Reveals only." );
}

/*===========================================================================
 * RvBuildReport
 *=========================================================================*/

int RvBuildReport( int category, report_row_t* rows, int max_rows )
{
    if ( rows == NULL || max_rows <= 1 ) return 0;

    RStart( rows, max_rows );

    switch ( category ) {
        case CAT_SYSTEM:        BuildSystem();      break;
        case CAT_PROCESSOR:     BuildProcessor();   break;
        case CAT_MEMORY:        BuildMemory();      break;
        case CAT_FIRSTMEG:      BuildFirstMeg();    break;
        case CAT_DISPLAY:       BuildDisplay();     break;
        case CAT_DRIVES:        BuildDrives();      break;
        case CAT_NETWORK:       BuildNetwork();     break;
        case CAT_PORTS:         BuildPorts();       break;
        case CAT_OS2:           BuildOs2();         break;
        case CAT_ENVIRONMENT:   BuildEnvironment(); break;
        case CAT_BENCHMARK:     BuildBenchmark();   break;
        default:                                    break;
    }

    RAdd( RK_END, "", "" );
    return r_count;
}

/*---------------------------------------------------------------------------
 * RvStatusText - the one-line summary along the bottom of the window
 *-------------------------------------------------------------------------*/

void RvStatusText( char* buffer, int size )
{
    char line[ 200 ];
    char speed[ 24 ];
    char ram[ 40 ];

    if ( buffer == NULL || size <= 0 ) return;

    if ( g_cpu_mhz > 0 ) sprintf( speed, " at %lu MHz", g_cpu_mhz );
    else                 speed[ 0 ] = '\0';

    if ( g_mem.known ) sprintf( ram, "   %s RAM", ByteSize( (double)g_mem.total_phys ) );
    else               ram[ 0 ] = '\0';

    sprintf( line, "%s%s%s   %s", ProcessorName(), speed, ram, OsName() );
    CopyStr( buffer, line, size );
}
