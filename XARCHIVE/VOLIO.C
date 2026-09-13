/*===========================================================================
 * VOLIO.C  -  N split volumes presented as one seekable stream
 * Target: MSVC 2.2  Win32s
 *
 * See VOLIO.H for what this is for and why RAR does not come through here.
 *
 * VOLUME NAMING.  A split set is <base>.<digits>, the digits starting at 1 and
 * zero-padded to a fixed width: 7z writes .7z.001, .7z.002 and keeps three
 * digits until it needs a fourth.  The width is therefore a property of the
 * SET, not of the number, so the next volume's name is made by incrementing
 * the digit field IN PLACE rather than by printf-ing a number - that way .009
 * goes to .010 and not to .10, and a set written four digits wide stays wide.
 *
 * Discovery stops at the first name that is not there.  A set with a HOLE in
 * it therefore looks SHORT rather than broken, which is exactly the state the
 * parsers need: the join reports honestly how many bytes it has and lets the
 * format layer, which is the only part that knows how many it EXPECTED, be
 * the one to call it a missing volume.
 *===========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volio.h"

#define VOL_MAX_VOLUMES  999    /* .001 .. .999; past that the set is refused */
#define VOL_MAX_PATH     260

struct VolFile {
    FILE  *fp;          /* the ONE open handle, on volume cur          */
    int    cur;         /* which volume fp is on, -1 when none         */
    int    n;           /* volumes joined (1 = ordinary single file)   */
    int    isSet;       /* the name is <base>.<digits>, set or not      */
    long   total;       /* bytes across all of them                    */
    long   pos;         /* logical position in the joined stream       */
    long  *start;       /* [n] logical offset each volume begins at    */
    long  *len;         /* [n] length of each volume                   */
    char **name;        /* [n] path of each volume                     */
};

/*--- helpers --------------------------------------------------------------*/

static long VolFileLength( const char *path )
{
    FILE *f = fopen( path, "rb" );
    long  n;

    if ( !f ) return -1L;
    if ( fseek( f, 0L, SEEK_END ) != 0 ) { fclose( f ); return -1L; }
    n = ftell( f );
    fclose( f );
    return n;
}

/* Is path of the form <something>.<digits> with at least three digits?
 * Returns the offset of the first digit, or -1.  Three is the minimum because
 * .001 is the shortest any splitter writes, and insisting on it keeps ordinary
 * names that merely end in a number - chapter.2 - off the volume path. */
static int VolDigitField( const char *path, int *widthOut )
{
    int len = (int)strlen( path );
    int i   = len;

    while ( i > 0 && path[i-1] >= '0' && path[i-1] <= '9' )
        i--;
    if ( i == len || i == 0 || path[i-1] != '.' ) return -1;
    if ( len - i < 3 )                            return -1;
    *widthOut = len - i;
    return i;
}

/* Write the num'th name of the set into buf, keeping the field width.
 * Returns 0 if num does not fit in width digits. */
static int VolMakeName( const char *path, int at, int width, int num,
                        char *buf, int buflen )
{
    int i;

    if ( at + width >= buflen ) return 0;
    memcpy( buf, path, (size_t)at );
    for ( i = width - 1; i >= 0; i-- )
    {
        buf[at+i] = (char)( '0' + ( num % 10 ) );
        num /= 10;
    }
    buf[at+width] = '\0';
    return num == 0;
}

static void VolFreeArrays( VolFile *v )
{
    int i;

    if ( v->name )
    {
        for ( i = 0; i < v->n; i++ ) free( v->name[i] );
        free( v->name );
    }
    free( v->start );
    free( v->len );
    v->name  = NULL;
    v->start = NULL;
    v->len   = NULL;
}

/* Case-insensitive compares.  Written out rather than using stricmp/strcasecmp
 * because the three compilers this builds under spell that differently. */
static int VolLowerCh( int c )
{
    return ( c >= 'A' && c <= 'Z' ) ? c - 'A' + 'a' : c;
}

static int VolLowerCmpN( const char *a, const char *b, int n )
{
    int i;

    for ( i = 0; i < n; i++ )
    {
        int d = VolLowerCh( (unsigned char)a[i] ) - VolLowerCh( (unsigned char)b[i] );
        if ( d ) return d;
    }
    return 0;
}

static int VolLowerCmp( const char *a, const char *b )
{
    return VolLowerCmpN( a, b, (int)strlen( b ) );
}

static int VolIsAlpha( int c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' );
}

/* Make fp point at volume idx, reopening if it is not the one we hold. */
static int VolSelect( VolFile *v, int idx )
{
    if ( idx < 0 || idx >= v->n ) return 0;
    if ( v->cur == idx && v->fp )  return 1;
    if ( v->fp ) { fclose( v->fp ); v->fp = NULL; }
    v->fp = fopen( v->name[idx], "rb" );
    if ( !v->fp ) { v->cur = -1; return 0; }
    v->cur = idx;
    return 1;
}

/*--- open / close ---------------------------------------------------------*/

int VolOpen( const char *path, VolFile **out )
{
    VolFile *v;
    char     buf[VOL_MAX_PATH];
    int      at, width, i, count;
    long     lenOne;

    *out = NULL;

    lenOne = VolFileLength( path );
    if ( lenOne < 0 ) return SZ_ERR_OPEN;

    v = (VolFile *)calloc( 1, sizeof( VolFile ) );
    if ( !v ) return SZ_ERR_MEMORY;
    v->cur = -1;

    at    = VolDigitField( path, &width );
    count = 0;
    v->isSet = ( at > 0 );

    if ( at > 0 && (int)strlen( path ) < VOL_MAX_PATH )
    {
        /* Count forwards from .001 whichever volume was actually named: a
         * user who opens .003 means the archive, not the fragment. */
        for ( i = 1; i <= VOL_MAX_VOLUMES; i++ )
        {
            if ( !VolMakeName( path, at, width, i, buf, VOL_MAX_PATH ) ) break;
            if ( VolFileLength( buf ) < 0 ) break;
            count = i;
        }
    }

    if ( count < 2 )
    {
        /* Not a split set (or a set of one): behave as the plain file. */
        v->n     = 1;
        v->name  = (char **)calloc( 1, sizeof( char * ) );
        v->start = (long *)calloc( 1, sizeof( long ) );
        v->len   = (long *)calloc( 1, sizeof( long ) );
        if ( !v->name || !v->start || !v->len )
        { VolFreeArrays( v ); free( v ); return SZ_ERR_MEMORY; }
        v->name[0] = (char *)malloc( strlen( path ) + 1 );
        if ( !v->name[0] )
        { VolFreeArrays( v ); free( v ); return SZ_ERR_MEMORY; }
        strcpy( v->name[0], path );
        v->start[0] = 0;
        v->len[0]   = lenOne;
        v->total    = lenOne;
    }
    else
    {
        v->n     = count;
        v->name  = (char **)calloc( (size_t)count, sizeof( char * ) );
        v->start = (long *)calloc( (size_t)count, sizeof( long ) );
        v->len   = (long *)calloc( (size_t)count, sizeof( long ) );
        if ( !v->name || !v->start || !v->len )
        { VolFreeArrays( v ); free( v ); return SZ_ERR_MEMORY; }

        v->total = 0;
        for ( i = 0; i < count; i++ )
        {
            long l;

            VolMakeName( path, at, width, i + 1, buf, VOL_MAX_PATH );
            v->name[i] = (char *)malloc( strlen( buf ) + 1 );
            if ( !v->name[i] )
            { VolFreeArrays( v ); free( v ); return SZ_ERR_MEMORY; }
            strcpy( v->name[i], buf );
            l = VolFileLength( buf );
            if ( l < 0 ) l = 0;
            v->start[i] = v->total;
            v->len[i]   = l;
            v->total   += l;
        }
    }

    v->pos = 0;
    if ( !VolSelect( v, 0 ) )
    { VolFreeArrays( v ); free( v ); return SZ_ERR_OPEN; }
    *out = v;
    return SZ_OK;
}

int VolOpenList( const char *const *paths, int n, VolFile **out )
{
    VolFile *v;
    int      i;

    *out = NULL;
    if ( n < 1 ) return SZ_ERR_OPEN;

    v = (VolFile *)calloc( 1, sizeof( VolFile ) );
    if ( !v ) return SZ_ERR_MEMORY;
    v->cur   = -1;
    v->n     = n;
    v->isSet = ( n > 1 );
    v->name  = (char **)calloc( (size_t)n, sizeof( char * ) );
    v->start = (long *)calloc( (size_t)n, sizeof( long ) );
    v->len   = (long *)calloc( (size_t)n, sizeof( long ) );
    if ( !v->name || !v->start || !v->len )
    { VolFreeArrays( v ); free( v ); return SZ_ERR_MEMORY; }

    v->total = 0;
    for ( i = 0; i < n; i++ )
    {
        long l;

        v->name[i] = (char *)malloc( strlen( paths[i] ) + 1 );
        if ( !v->name[i] )
        { VolFreeArrays( v ); free( v ); return SZ_ERR_MEMORY; }
        strcpy( v->name[i], paths[i] );
        l = VolFileLength( paths[i] );
        if ( l < 0 ) { VolFreeArrays( v ); free( v ); return SZ_ERR_OPEN; }
        v->start[i] = v->total;
        v->len[i]   = l;
        v->total   += l;
    }

    v->pos = 0;
    if ( !VolSelect( v, 0 ) )
    { VolFreeArrays( v ); free( v ); return SZ_ERR_OPEN; }
    *out = v;
    return SZ_OK;
}

void VolClose( VolFile *v )
{
    if ( !v ) return;
    if ( v->fp ) fclose( v->fp );
    VolFreeArrays( v );
    free( v );
}

/*--- reading --------------------------------------------------------------*/

UInt32 VolRead( void *buf, UInt32 size, UInt32 count, VolFile *v )
{
    Byte  *p    = (Byte *)buf;
    UInt32 want;
    UInt32 got  = 0;

    if ( !v || size == 0 || count == 0 ) return 0;
    want = size * count;

    while ( got < want )
    {
        int    idx = -1;
        int    i;
        long   inVol;
        UInt32 chunk;
        UInt32 n;

        if ( v->pos >= v->total ) break;

        for ( i = 0; i < v->n; i++ )
            if ( v->pos >= v->start[i] && v->pos < v->start[i] + v->len[i] )
            { idx = i; break; }
        if ( idx < 0 ) break;
        if ( !VolSelect( v, idx ) ) break;

        inVol = v->pos - v->start[idx];
        if ( fseek( v->fp, inVol, SEEK_SET ) != 0 ) break;

        chunk = (UInt32)( v->len[idx] - inVol );   /* left in this volume */
        if ( chunk > want - got ) chunk = want - got;

        n = (UInt32)fread( p + got, 1, chunk, v->fp );
        got    += n;
        v->pos += (long)n;
        if ( n != chunk ) break;                   /* short: stop here */
    }

    return got / size;
}

int VolGetc( VolFile *v )
{
    Byte b;

    if ( VolRead( &b, 1, 1, v ) != 1 ) return -1;
    return (int)b;
}

/*--- RAR volume names -----------------------------------------------------*
 *
 * RAR has TWO naming schemes and a set uses one or the other:
 *
 *   NEW (the default since WinRAR 3, and the only one RAR5 writes)
 *       base.part1.rar, base.part2.rar, ...
 *     The number is zero-padded to the width the FINAL count needs, so a
 *     six-volume set is part1..part6 but a fifteen-volume set is part01..
 *     part15.  The width is fixed for the whole set, which is why the next
 *     name is made by incrementing the digits IN PLACE - printf-ing "%d"
 *     would turn part09 into part10 correctly but part01 into part2.
 *
 *   OLD (-vn, and everything RAR 2.x wrote)
 *       base.rar, base.r00, base.r01, ... base.r99, base.s00, ...
 *     The FIRST volume has the ordinary .rar extension and the rest count in
 *     base-100 with a letter carry, so .r99 is followed by .s00.
 *
 * Telling them apart matters: ".part2.rar" ends in .rar too, so the new
 * scheme has to be tested first or every new-style volume looks like the
 * first volume of an old-style set.
 *--------------------------------------------------------------------------*/

/* Find the digit run of a ".partNNN.rar" name.  Returns its offset and width,
 * or -1 when the name is not of that form. */
static int VolRarPartField( const char *path, int *widthOut )
{
    int len = (int)strlen( path );
    int end, i;

    if ( len < 10 ) return -1;
    if ( VolLowerCmp( path + len - 4, ".rar" ) != 0 ) return -1;

    end = len - 4;                       /* just past the digits */
    i   = end;
    while ( i > 0 && path[i-1] >= '0' && path[i-1] <= '9' )
        i--;
    if ( i == end ) return -1;           /* no digits before .rar */
    if ( i < 5 ) return -1;
    if ( VolLowerCmpN( path + i - 5, ".part", 5 ) != 0 ) return -1;

    *widthOut = end - i;
    return i;
}

/* Is this an old-style continuation - ".r00" upwards?  Returns the offset of
 * the letter, or -1.  ".rar" itself is deliberately NOT matched: it is the
 * first volume, not a continuation. */
static int VolRarOldField( const char *path )
{
    int len = (int)strlen( path );

    if ( len < 4 ) return -1;
    if ( path[len-4] != '.' ) return -1;
    if ( !VolIsAlpha( path[len-3] ) ) return -1;
    if ( path[len-2] < '0' || path[len-2] > '9' ) return -1;
    if ( path[len-1] < '0' || path[len-1] > '9' ) return -1;
    return len - 3;
}

int VolRarFirstName( const char *cur, char *buf, int buflen )
{
    int at, width, len;

    len = (int)strlen( cur );
    if ( len + 1 > buflen ) return 0;

    at = VolRarPartField( cur, &width );
    if ( at > 0 )
    {
        int i, num = 1;

        memcpy( buf, cur, (size_t)len );
        buf[len] = '\0';
        for ( i = width - 1; i >= 0; i-- )
        {
            buf[at+i] = (char)( '0' + ( num % 10 ) );
            num /= 10;
        }
        return num == 0;
    }

    at = VolRarOldField( cur );
    if ( at > 0 )
    {
        /* base.rNN -> base.rar */
        memcpy( buf, cur, (size_t)( at ) );
        buf[at]   = 'r';
        buf[at+1] = 'a';
        buf[at+2] = 'r';
        buf[at+3] = '\0';
        return 1;
    }

    strcpy( buf, cur );                  /* already the first volume */
    return 1;
}

int VolRarNextName( const char *cur, char *buf, int buflen )
{
    int at, width, len;

    len = (int)strlen( cur );
    if ( len + 1 > buflen ) return 0;

    at = VolRarPartField( cur, &width );
    if ( at > 0 )
    {
        int i, carry = 1;

        memcpy( buf, cur, (size_t)len );
        buf[len] = '\0';
        for ( i = width - 1; i >= 0 && carry; i-- )
        {
            int d = buf[at+i] - '0' + carry;
            buf[at+i] = (char)( '0' + ( d % 10 ) );
            carry = d / 10;
        }
        return carry == 0;               /* overflowing the width ends the set */
    }

    at = VolRarOldField( cur );
    if ( at > 0 )
    {
        int n;

        memcpy( buf, cur, (size_t)len );
        buf[len] = '\0';
        n = ( buf[at+1] - '0' ) * 10 + ( buf[at+2] - '0' ) + 1;
        if ( n > 99 )
        {
            /* .r99 -> .s00, and on up the alphabet. */
            if ( buf[at] == 'z' || buf[at] == 'Z' ) return 0;
            buf[at]++;
            n = 0;
        }
        buf[at+1] = (char)( '0' + n / 10 );
        buf[at+2] = (char)( '0' + n % 10 );
        return 1;
    }

    /* base.rar -> base.r00 */
    if ( len >= 4 && VolLowerCmp( cur + len - 4, ".rar" ) == 0 )
    {
        memcpy( buf, cur, (size_t)len );
        buf[len-3] = 'r';
        buf[len-2] = '0';
        buf[len-1] = '0';
        buf[len]   = '\0';
        return 1;
    }

    return 0;
}


/*--- positioning ----------------------------------------------------------*/

int VolSeek( VolFile *v, long off, int whence )
{
    long target;

    if ( !v ) return -1;
    if ( whence == SEEK_SET )      target = off;
    else if ( whence == SEEK_CUR ) target = v->pos + off;
    else if ( whence == SEEK_END ) target = v->total + off;
    else                           return -1;

    if ( target < 0 ) return -1;
    v->pos = target;            /* past the end is legal, as with stdio */
    return 0;
}

long VolTell( VolFile *v )  { return v ? v->pos   : -1L; }
long VolSize( VolFile *v )  { return v ? v->total : 0L;  }
int  VolCount( VolFile *v ) { return v ? v->n     : 0;   }
int  VolIsSet( VolFile *v )  { return v ? v->isSet : 0;   }

long VolVolumeStart( VolFile *v, int i )
{
    return ( v && i >= 0 && i < v->n ) ? v->start[i] : 0L;
}

long VolVolumeLen( VolFile *v, int i )
{
    return ( v && i >= 0 && i < v->n ) ? v->len[i] : 0L;
}

const char *VolPath( VolFile *v )
{
    return ( v && v->name && v->name[0] ) ? v->name[0] : "";
}
