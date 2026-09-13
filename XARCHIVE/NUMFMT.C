/*===========================================================================
 * NUMFMT.C  -  File sizes written the way this machine writes numbers
 * Target: MSVC 2.2 Win32s / Open Watcom DOS32A / Open Watcom OS/2 PM
 *
 * See NUMFMT.H for what this is for.  The interesting part is the DOS half.
 *
 * WHY DOS GOES THE LONG WAY ROUND.  The obvious way to read the country
 * table from a 32-bit DOS program is int386x with AH=38h and DS:DX pointing
 * at a buffer.  That does not work under DOS/32A, and it does not FAIL
 * either: the call returns with carry CLEAR and the buffer completely
 * untouched.  Measured, not guessed - a probe filled the buffer with 0xEE
 * first, and every byte was still 0xEE afterwards.  Believing the carry flag
 * there would mean reading 0xEE as the thousands separator and printing it.
 *
 * So the extender is asked to do it properly: allocate a block of real
 * memory (DPMI 0100h), simulate the interrupt in real mode (DPMI 0300h) with
 * DS pointing at that block, and read the result back.  Under that route the
 * same probe returned date format 0, currency "$", separator "," - which is
 * what DIR prints on the same machine, so the answer is confirmed against
 * something independent rather than merely non-empty.
 *
 * The layout of the returned block is from the MS-DOS 6 source, not from a
 * reference book: DOS6SRC\inc\intnat.inc gives a WORD date format, then a
 * five-byte currency symbol, putting the two-byte ASCIIZ thousands separator
 * at OFFSET 7.
 *===========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__OS2__)
#  define INCL_DOSNLS
#  include <os2.h>
#else
#  include <dos.h>
#endif

#include "numfmt.h"

/* g_haveSep is separate from g_sep[0] because "" is a real answer - it is how
 * the caller switches grouping off - and must not be mistaken for "not asked
 * yet", which would re-query the system on every row of a listing. */
static char g_sep[NUM_SEP_MAX] = "";
static int  g_haveSep          = 0;

/*---------------------------------------------------------------------------
 * Asking the system
 *-------------------------------------------------------------------------*/

#if defined(_WIN32)

static void NumQuerySystem( void )
{
    char buf[NUM_SEP_MAX + 8];

    buf[0] = '\0';
    if ( GetLocaleInfoA( LOCALE_USER_DEFAULT, LOCALE_STHOUSAND,
                         buf, sizeof( buf ) ) > 0 &&
         strlen( buf ) < NUM_SEP_MAX )
        strcpy( g_sep, buf );
    else
        strcpy( g_sep, "," );        /* the call is documented not to fail,
                                      * but a default beats an empty column */
}

#elif defined(__OS2__)

static void NumQuerySystem( void )
{
    COUNTRYCODE cc;
    COUNTRYINFO ci;
    ULONG       len = 0;

    memset( &cc, 0, sizeof( cc ) );      /* 0,0 = the process's own country */
    memset( &ci, 0, sizeof( ci ) );

    if ( DosQueryCtryInfo( sizeof( ci ), &cc, &ci, &len ) == 0 &&
         len >= sizeof( ci ) &&
         strlen( ci.szThousandsSeparator ) < NUM_SEP_MAX )
        strcpy( g_sep, ci.szThousandsSeparator );
    else
        strcpy( g_sep, "," );
}

#else   /*---- DOS ----------------------------------------------------------*/

/* The register block DPMI 0300h fills in and reads back.  Packed: the host
 * reads it field by field at fixed offsets. */
#pragma pack(1)
typedef struct {
    UInt32         edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} NumRmRegs;
#pragma pack()

static void NumQuerySystem( void )
{
    union  REGS    r;
    struct SREGS   sr;
    NumRmRegs      rm;
    unsigned short rmSeg, selector;
    const char    *p;
    int            i;

    strcpy( g_sep, "," );                     /* if anything below fails */

    /* DPMI 0100h - allocate DOS memory.  Four paragraphs is 64 bytes; the
     * country block is 34 and the call writes no more than we asked for. */
    memset( &r, 0, sizeof( r ) );
    r.x.eax = 0x0100;
    r.x.ebx = 4;
    int386( 0x31, &r, &r );
    if ( r.x.cflag ) return;
    rmSeg    = (unsigned short)r.x.eax;
    selector = (unsigned short)r.x.edx;

    /* DPMI 0300h - simulate INT 21h AH=38h in real mode, with DS:DX aimed at
     * the block we just obtained. */
    memset( &rm, 0, sizeof( rm ) );
    rm.eax = 0x3800;
    rm.ds  = rmSeg;
    rm.edx = 0;

    memset( &r, 0, sizeof( r ) );
    segread( &sr );
    r.x.eax = 0x0300;
    r.x.ebx = 0x21;
    r.x.ecx = 0;
    r.x.edi = (UInt32)&rm;
    sr.es   = sr.ds;
    int386x( 0x31, &r, &r, &sr );

    if ( !r.x.cflag && !( rm.flags & 1 ) )
    {
        /* Low memory is mapped flat by the extender, so the block is simply
         * at segment*16.  Offset 7 is the separator - see the banner. */
        p = (const char *)( (UInt32)rmSeg * 16 + 7 );
        for ( i = 0; i < NUM_SEP_MAX - 1 && p[i]; i++ ) ;
        if ( i > 0 && !p[i] )                 /* a terminated, non-empty string */
        {
            memcpy( g_sep, p, (size_t)i );
            g_sep[i] = '\0';
        }
    }

    /* Hand the memory back.  DPMI 0101h takes the SELECTOR, not the segment -
     * passing the segment here frees something else or nothing at all. */
    memset( &r, 0, sizeof( r ) );
    r.x.eax = 0x0101;
    r.x.edx = selector;
    int386( 0x31, &r, &r );
}

#endif

/*---------------------------------------------------------------------------
 * The separator in force
 *-------------------------------------------------------------------------*/

const char *NumGroupSep( void )
{
    if ( !g_haveSep )
    {
        NumQuerySystem();
        g_haveSep = 1;
    }
    return g_sep;
}

void NumSetGroupSep( const char *sep )
{
    if ( !sep )                               /* back to whatever DOS says */
    {
        g_haveSep = 0;
        g_sep[0]  = '\0';
        return;
    }
    if ( strlen( sep ) >= NUM_SEP_MAX ) return;   /* refuse, do not truncate */
    strcpy( g_sep, sep );
    g_haveSep = 1;
}

/* Case-insensitive compare, written out because the three compilers this
 * builds under spell the library one differently (stricmp / strcmpi). */
static int NumSameWord( const char *a, const char *b )
{
    int i;
    for ( i = 0; a[i] && b[i]; i++ )
    {
        char ca = a[i], cb = b[i];
        if ( ca >= 'A' && ca <= 'Z' ) ca = (char)( ca - 'A' + 'a' );
        if ( cb >= 'A' && cb <= 'Z' ) cb = (char)( cb - 'A' + 'a' );
        if ( ca != cb ) return 0;
    }
    return a[i] == b[i];
}

void NumSetSepSpelling( const char *spelling )
{
    if ( !spelling ) return;

    /* "none" and "off" switch grouping off.  Spelling them out matters: an
     * empty XARCSEP= is ambiguous between "no separator" and "unset" on some
     * shells, so the words are what actually turn it off - and an empty value
     * is treated the same way rather than being left to the shell to decide. */
    if ( !spelling[0] ||
         NumSameWord( spelling, "none" ) || NumSameWord( spelling, "off" ) )
    { NumSetGroupSep( "" ); return; }

    /* "space" spelt out, because a trailing space does not survive being
     * typed on a command line or set in an environment variable. */
    if ( NumSameWord( spelling, "space" ) )
    { NumSetGroupSep( " " ); return; }

    NumSetGroupSep( spelling );               /* refused if it is too long */
}

void NumFmtInitFromEnv( void )
{
    const char *e = getenv( "XARCSEP" );

    if ( !e ) return;                         /* nothing said: ask the system */
    NumSetSepSpelling( e );
}

/*---------------------------------------------------------------------------
 * Formatting
 *
 * Both entry points reduce to one job: given a plain run of digits, put the
 * separator in after every third from the RIGHT.  Doing it in one place is
 * what keeps NumFmt and NumFmtD from drifting apart, and it is the only part
 * with anything to get wrong.
 *
 * Built backwards into a scratch buffer and then reversed, which needs no
 * first pass to count the digits and no second pass to place the separators.
 *-------------------------------------------------------------------------*/

static char *NumGroupDigits( const char *digits, char *buf )
{
    char        tmp[NUM_FMT_MAX];
    const char *sep    = NumGroupSep();
    int         seplen = (int)strlen( sep );
    int         len    = (int)strlen( digits );
    int         n = 0, group = 0, i, j;

    if ( len == 0 ) { buf[0] = '0'; buf[1] = '\0'; return buf; }

    for ( i = len - 1; i >= 0 && n < NUM_FMT_MAX - 1; i-- )
    {
        if ( seplen && group == 3 )
        {
            /* The separator goes in backwards too, or a multi-byte one comes
             * out reversed.  One byte is the usual case; this costs nothing
             * and is right for the others. */
            for ( j = seplen - 1; j >= 0 && n < NUM_FMT_MAX - 1; j-- )
                tmp[n++] = sep[j];
            group = 0;
        }
        tmp[n++] = digits[i];
        group++;
    }

    for ( i = n - 1, j = 0; i >= 0; i--, j++ ) buf[j] = tmp[i];
    buf[j] = '\0';
    return buf;
}

char *NumFmt( UInt32 v, char *buf )
{
    char digits[16];
    int  n = 0, i, j;
    char c;

    if ( v == 0 ) digits[n++] = '0';
    while ( v > 0 ) { digits[n++] = (char)( '0' + (int)( v % 10 ) ); v /= 10; }
    digits[n] = '\0';

    for ( i = 0, j = n - 1; i < j; i++, j-- )       /* reverse in place */
    { c = digits[i]; digits[i] = digits[j]; digits[j] = c; }

    return NumGroupDigits( digits, buf );
}

char *NumFmtD( double v, char *buf )
{
    char digits[48];
    int  i;

    /* Not (v < 0) alone: a NaN compares false against everything, so testing
     * for the GOOD range is what also catches it. */
    if ( !( v >= 0.0 ) ) { buf[0] = '0'; buf[1] = '\0'; return buf; }

    /* A double past what %.0f can spell would give "1e+30" and the grouping
     * would then be applied to an exponent.  Clamp instead: no archive holds
     * this many bytes, and a wrong number is worse than a capped one. */
    if ( v > 1.0e30 ) v = 1.0e30;

    sprintf( digits, "%.0f", v );

    /* Some libraries spell large doubles with a sign or exponent anyway.  If
     * anything but digits came back, do not pretend to group it. */
    for ( i = 0; digits[i]; i++ )
        if ( digits[i] < '0' || digits[i] > '9' )
        { strcpy( buf, digits ); return buf; }

    return NumGroupDigits( digits, buf );
}

char *NumFmtRight( UInt32 v, char *buf, int width )
{
    char text[NUM_FMT_MAX];
    int  len, pad, i;

    NumFmt( v, text );
    len = (int)strlen( text );

    pad = width - len;
    if ( pad < 0 ) pad = 0;              /* too wide: let the column grow */

    for ( i = 0; i < pad; i++ ) buf[i] = ' ';
    strcpy( buf + pad, text );
    return buf;
}
