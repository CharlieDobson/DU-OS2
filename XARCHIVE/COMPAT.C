/*===========================================================================
 * COMPAT.C  -  OS/2 implementations of the Win32 shims declared in the
 *              local windows.h compatibility header.
 * Target: OS/2 2.x / Warp, Open Watcom 1.9  (wcc386 -bt=os2)
 *
 * Open Watcom has 64-bit integers, so the FILETIME calendar math here is
 * straightforward.  Calendar conversions use Howard Hinnant's civil<->days
 * algorithm.
 *
 * FileTimeToLocalFileTime is real rather than a stub: DosGetDateTime reports
 * the system's offset from UTC, so the UTC timestamps 7z and RAR5 store are
 * shown and restored in local time.
 *===========================================================================*/
#define INCL_DOSDATETIME
#include <os2.h>

#include <windows.h>     /* the local OS/2 shim */
#include <string.h>

char *xa_lstrcpyn( char *dst, const char *src, int count )
{
    int i;
    if ( count <= 0 ) return dst;
    for ( i = 0; i < count - 1 && src[i]; i++ )
        dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}

/* 7z stores its names as UTF-16.  OS/2's codepage 850 / 437 has no clean
 * mapping for anything above Latin-1, so this is best-effort: pass through
 * the low byte where the code point fits in a byte, otherwise substitute
 * '_'. */
int WideCharToMultiByte( unsigned cp, DWORD flags,
                         const WCHAR *wstr, int wlen,
                         char *dst, int dstBytes,
                         const char *defChar, int *usedDef )
{
    int i = 0;
    (void)cp; (void)flags; (void)wlen; (void)defChar; (void)usedDef;

    while ( wstr[i] && i < dstBytes - 1 )
    {
        WCHAR w = wstr[i];
        dst[i] = ( w < 0x100 ) ? (char)w : '_';
        i++;
    }
    dst[i] = '\0';
    return i + 1;
}

/*---- Calendar math -------------------------------------------------------- */
#define EPOCH_1601_TO_1970  116444736000000000LL   /* 100-ns ticks */
#define TICKS_PER_SEC       10000000LL

/* Days since 1970-01-01 for a proleptic-Gregorian y/m/d. */
static long DaysFromCivil( long y, unsigned m, unsigned d )
{
    long era, yoe, doy, doe;
    y -= ( m <= 2 );
    era = ( ( y >= 0 ) ? y : y - 399 ) / 400;
    yoe = y - era * 400;
    doy = ( 153 * ( (long)m + ( m > 2 ? -3 : 9 ) ) + 2 ) / 5 + (long)d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Inverse: days since 1970-01-01 -> y/m/d. */
static void CivilFromDays( long z, long *py, unsigned *pm, unsigned *pd )
{
    long era, doe, yoe, y, doy, mp;
    z += 719468;
    era = ( ( z >= 0 ) ? z : z - 146096 ) / 146097;
    doe = z - era * 146097;
    yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
    y   = yoe + era * 400;
    doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
    mp  = ( 5 * doy + 2 ) / 153;
    *pd = (unsigned)( doy - ( 153 * mp + 2 ) / 5 + 1 );
    *pm = (unsigned)( mp < 10 ? mp + 3 : mp - 9 );
    *py = y + ( *pm <= 2 );
}

BOOL SystemTimeToFileTime( const SYSTEMTIME *st, FILETIME *ft )
{
    long           days = DaysFromCivil( st->wYear, st->wMonth, st->wDay );
    long long      secs = (long long)days * 86400
                        + (long long)st->wHour * 3600
                        + (long long)st->wMinute * 60
                        + (long long)st->wSecond;
    unsigned long long ticks =
        (unsigned long long)( secs * TICKS_PER_SEC + EPOCH_1601_TO_1970 );

    ft->dwLowDateTime  = (DWORD)( ticks & 0xFFFFFFFFULL );
    ft->dwHighDateTime = (DWORD)( ticks >> 32 );
    return TRUE;
}

BOOL FileTimeToSystemTime( const FILETIME *ft, SYSTEMTIME *st )
{
    unsigned long long ticks =
        ( (unsigned long long)ft->dwHighDateTime << 32 ) | ft->dwLowDateTime;
    long long secs = (long long)( ticks - EPOCH_1601_TO_1970 ) / TICKS_PER_SEC;
    long      days = (long)( secs / 86400 );
    long      tod  = (long)( secs % 86400 );
    long      y;
    unsigned  m, d;

    if ( tod < 0 ) { tod += 86400; days--; }
    CivilFromDays( days, &y, &m, &d );

    st->wYear   = (WORD)y;
    st->wMonth  = (WORD)m;
    st->wDay    = (WORD)d;
    st->wHour   = (WORD)( tod / 3600 );
    st->wMinute = (WORD)( ( tod % 3600 ) / 60 );
    st->wSecond = (WORD)( tod % 60 );
    st->wMilliseconds = 0;
    st->wDayOfWeek    = (WORD)( ( ( days % 7 ) + 4 + 7 ) % 7 );  /* 1970-01-01 = Thu */
    return TRUE;
}

/* UTC -> local.  DATETIME.timezone is the offset in minutes WEST of UTC, and
 * is 0xFFFF (-1 as a SHORT) when the system does not know it -- in which case
 * we leave the time alone rather than invent a shift. */
BOOL FileTimeToLocalFileTime( const FILETIME *ft, FILETIME *lft )
{
    DATETIME           dt;
    unsigned long long ticks =
        ( (unsigned long long)ft->dwHighDateTime << 32 ) | ft->dwLowDateTime;

    if ( DosGetDateTime( &dt ) == 0 && dt.timezone != (SHORT)0xFFFF )
    {
        long long shift = -(long long)dt.timezone * 60LL * TICKS_PER_SEC;
        if ( shift < 0 && (unsigned long long)(-shift) > ticks )
            ticks = 0;
        else
            ticks = (unsigned long long)( (long long)ticks + shift );
    }

    lft->dwLowDateTime  = (DWORD)( ticks & 0xFFFFFFFFULL );
    lft->dwHighDateTime = (DWORD)( ticks >> 32 );
    return TRUE;
}
