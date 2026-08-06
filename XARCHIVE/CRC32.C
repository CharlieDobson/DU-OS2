/*===========================================================================
 * CRC32.C  -  CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320)
 * Target: MSVC 2.2  Win32s
 *
 * Matches the CRC used by the 7z format for header and file verification.
 *===========================================================================*/

#include "crc32.h"

static UInt32 g_table[256];
static int    g_inited = 0;

static void BuildTable( void )
{
    UInt32 c;
    int    n, k;

    for ( n = 0; n < 256; n++ )
    {
        c = (UInt32)n;
        for ( k = 0; k < 8; k++ )
            c = ( c & 1 ) ? ( 0xEDB88320UL ^ ( c >> 1 ) ) : ( c >> 1 );
        g_table[n] = c;
    }
    g_inited = 1;
}

UInt32 Crc32Init( void )
{
    if ( !g_inited )
        BuildTable();
    return 0xFFFFFFFFUL;
}

UInt32 Crc32Update( UInt32 crc, const void *data, UInt32 len )
{
    const Byte *p = (const Byte *)data;

    if ( !g_inited )
        BuildTable();

    while ( len-- )
        crc = g_table[ (Byte)( crc ^ *p++ ) ] ^ ( crc >> 8 );

    return crc;
}

UInt32 Crc32Done( UInt32 crc )
{
    return crc ^ 0xFFFFFFFFUL;
}

UInt32 Crc32Calc( const void *data, UInt32 len )
{
    return Crc32Done( Crc32Update( Crc32Init(), data, len ) );
}
