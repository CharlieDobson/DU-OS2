/*===========================================================================
 * ROMSRCHR.CPP  -  Scan ROM address ranges for names, dates and versions
 *
 * The OS/2 copy of DUSOURCE\REVEAL\ROMSRCHR.CPP (by way of the Win32s one).
 * The three searches are the same; what they search is not.  Under DOS the
 * ROM is a far pointer away, and under Win16 a selector; here it is
 * LOWMEM.CPP's copy of C0000h-FFFFFh, fetched once at start-up through the
 * TESTCFG$ driver, so a search is plain memory access with nothing that can
 * fault.
 *
 * The DOS original lets its 40-byte copies run past the end of the area it
 * scans - harmless there, where the read wraps within the segment.  The copy
 * here ends where the ROM area ends, so every scan stops early enough for
 * its last copy to fit (the same rule the Win16 build learned for its 64 KB
 * selector window).
 *
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *===========================================================================*/

#include <os2.h>
#include "romsrchr.hpp"
#include <string.h>
#include <ctype.h>
#include "lowmem.h"

#define ROM_COPY_BYTES      40      /* the longest read past the match      */

/* --------------------------------------------------------------------------
 * RomArea - base, length and how far a scan may go
 * ------------------------------------------------------------------------*/

static const BYTE* RomArea( const rom_search_area_t search, unsigned int* scan_end )
{
    ULONG        linear;
    ULONG        length;
    unsigned int end;
    const BYTE*  base;

    switch ( search ) {
        case C000:  linear = 0x000C0000UL; length = 0x8000UL;  end = 0x7FF6u; break;
        case D000:  linear = 0x000D0000UL; length = 0x10000UL; end = 0xFFF5u; break;
        case E000:  linear = 0x000E0000UL; length = 0x10000UL; end = 0xFFF5u; break;
        case F800:
        default:    linear = 0x000F8000UL; length = 0x8000UL;  end = 0x7FF6u; break;
    }

    base = LowRom( linear, length );
    if ( base == NULL ) return NULL;

    if ( end > (unsigned int)( length - ROM_COPY_BYTES ) )
        end = (unsigned int)( length - ROM_COPY_BYTES );

    *scan_end = end;
    return base;
}

/*********************************************************************
 * RomStringSearch - Finds a name in the computer/video card's ROM.
 *
 * search       - area of memory to search
 * strings      - Pointer to a pointer of a list of names to search for.
 * buffer       - pointer to char buffer to receive strings
 * BufferSize   - size of buffer
 *
 * RETURNS      - buffer == string || "Unknown"
 *********************************************************************/
void RomStringSearch( const rom_search_area_t search, const char* strings[], char* buffer, const unsigned int BufferSize )
{
    const BYTE*  bios;
    unsigned int scan_end = 0;
    unsigned int i;

    if ( NULL == buffer || NULL == strings || NULL == *strings || BufferSize == 0 ) return;
    buffer[0] = '\0';

    bios = RomArea( search, &scan_end );
    if ( NULL == bios ) return;

    for ( i = 0; i < scan_end; ++i ) {
        int j;

        // skip ahead over non-ASCII bytes: most ROM strings are packed
        // along boundaries, and this is what keeps the scan quick
        if ( bios[i] < 0x20 || bios[i] > 0x7E ) {
            i += 4;
            continue;
        }

        for ( j = 0; strings[j] != NULL; ++j ) {
            // match the first 10 characters or less to the string
            size_t len = ( strlen( strings[j] ) < 10 ) ? strlen( strings[j] ) : 10;

            if ( memcmp( &bios[i], strings[j], len ) == 0 ) {
                char   bios_string[40];
                char*  split;
                size_t k;

                memcpy( bios_string, &bios[i], sizeof( bios_string ) );
                bios_string[sizeof( bios_string ) - 1] = '\0';

                // ensure only ASCII chars are in string after raw memcpy
                for ( k = 0; k < strlen( bios_string ); ++k ) {
                    if ( bios_string[k] < 0x20 || bios_string[k] > 0x7E ) {
                        bios_string[k] = '\0';
                    }
                }

                // check if string is not what we want
                if ( strstr( bios_string, "IBM COMPATIBLE" ) != NULL ) continue;
                if ( strstr( bios_string, "IBM AT Compatible" ) != NULL ) continue;
                if ( strstr( bios_string, "IBM IS A TRADEMARK" ) != NULL ) continue;
                // Correct string searches for manufacturers we know
                if ( strstr( bios_string, "M5Pi" ) != NULL ) {
                    strcpy( bios_string, "Micron PC" );
                }
                // cut off unrelated characters after the name
                split = strstr( bios_string, "." );
                if ( split ) {
                    if ( isalpha( (unsigned char)*( split + 1 ) ) ) *( split + 1 ) = '\0';
                    else if ( isspace( (unsigned char)*( split + 1 ) ) ) *( split + 1 ) = '\0';
                }
                split = strstr( bios_string, "-" );
                if ( split && split > bios_string ) {
                    if ( isspace( (unsigned char)*( split - 1 ) ) &&
                         isspace( (unsigned char)*( split + 1 ) ) ) *( split - 1 ) = '\0';
                }
                split = strstr( bios_string, "VGA BIOS" );
                if ( split && split > bios_string ) {
                    *( split - 1 ) = '\0';
                }

                strncpy( buffer, bios_string, BufferSize );
                buffer[BufferSize - 1] = '\0';
                return;
            }
        }
    }

    // if the string is not found, set it to "unknown"
    strncpy( buffer, "Unknown", BufferSize );
    buffer[BufferSize - 1] = '\0';
}

void RomDateSearch( const rom_search_area_t search, char* buffer, const unsigned int BufferSize )
{
    const BYTE*  bios;
    unsigned int scan_end = 0;
    unsigned int i;

    if ( NULL == buffer || BufferSize == 0 ) return;
    buffer[0] = '\0';

    bios = RomArea( search, &scan_end );
    if ( NULL == bios ) return;

    for ( i = 0; i < scan_end; ++i ) {
        char sep;

        // look for start of MM/DD/YYYY or MM-DD-YYYY pattern
        if ( !isdigit( bios[i] ) ) continue;

        sep = (char)bios[i + 2];
        if ( sep != '/' && sep != '-' ) continue;

        // validate full date pattern in-place before copying
        if ( isdigit( bios[i + 1] ) &&
             isdigit( bios[i + 3] ) &&
             isdigit( bios[i + 4] ) &&
             bios[i + 5] == (unsigned char)sep &&
             isdigit( bios[i + 6] ) &&
             isdigit( bios[i + 7] ) &&
             isdigit( bios[i + 8] ) &&
             isdigit( bios[i + 9] ) ) {

            char bios_string[40];

            memcpy( bios_string, &bios[i], sizeof( bios_string ) );
            bios_string[sizeof( bios_string ) - 1] = '\0';

            // just the date: the DOS original copied 40 bytes and showed
            // whatever followed it ("10/15/2001-i440BX-<P3B-F>")
            bios_string[10] = '\0';

            strncpy( buffer, bios_string, BufferSize );
            buffer[BufferSize - 1] = '\0';
            return;
        }
    }

    buffer[0] = '\0';
}

void RomVersionSearch( const rom_search_area_t search, char* buffer, const unsigned int BufferSize )
{
    const BYTE*  bios;
    unsigned int scan_end = 0;
    unsigned int i;

    if ( NULL == buffer || BufferSize == 0 ) return;
    buffer[0] = '\0';

    bios = RomArea( search, &scan_end );
    if ( NULL == bios ) return;

    for ( i = 0; i < scan_end; ++i ) {
        unsigned int d;

        // look for 'v' or 'V' not preceded by a letter (avoid "version", "driver", etc.)
        if ( bios[i] != 'v' && bios[i] != 'V' ) continue;
        if ( i > 0 && isalpha( bios[i - 1] ) ) continue;

        // determine digit offset: "v1.23" (d=1) or "v 1.23" (d=2)
        if ( isdigit( bios[i + 1] ) ) {
            d = 1;
        } else if ( bios[i + 1] == ' ' && isdigit( bios[i + 2] ) ) {
            d = 2;
        } else {
            continue;
        }

        // validate version pattern: digit . digit (e.g. "2.34")
        if ( bios[i + d + 1] == '.' && isdigit( bios[i + d + 2] ) ) {
            char bios_string[40];
            int  k;

            memcpy( bios_string, &bios[i], sizeof( bios_string ) );
            bios_string[sizeof( bios_string ) - 1] = '\0';

            // truncate at first non-printable character
            for ( k = 0; k < (int)sizeof( bios_string ); ++k ) {
                if ( bios_string[k] < 0x20 || bios_string[k] > 0x7E ) {
                    bios_string[k] = '\0';
                    break;
                }
            }

            strncpy( buffer, bios_string, BufferSize );
            buffer[BufferSize - 1] = '\0';
            return;
        }
    }

    buffer[0] = '\0';
}
