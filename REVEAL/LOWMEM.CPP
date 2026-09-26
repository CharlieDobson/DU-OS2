/*===========================================================================
 * LOWMEM.CPP  -  Copy the adapter and BIOS ROM area through TESTCFG$
 * Target: Open Watcom 1.9   OS/2 2.x / Warp  (32-bit Presentation Manager)
 *
 * See LOWMEM.H for why this is a driver call and what it can and cannot see.
 *===========================================================================*/
#define INCL_DOSFILEMGR
#define INCL_DOSDEVICES
#define INCL_DOSERRORS

#include <os2.h>
#include <stdlib.h>
#include <string.h>

#include "lowmem.h"

/* TESTCFG$ IOCtl: category 80h, function 40h - read physical memory in the
   adapter window.  The driver insists the command word is zero and the
   address falls inside C0000h-FFFFFh. */
#define TESTCFG_CATEGORY        0x80
#define TESTCFG_READ_ROM        0x40

#pragma pack( 1 )
typedef struct testcfg_param_t {
    ULONG   command;                /* always 0                             */
    ULONG   physical;               /* first byte to copy                   */
    USHORT  count;                  /* bytes to copy                        */
} testcfg_param_t;
#pragma pack()

#define CHUNK_BYTES     0x8000UL                    /* 32 KB per call       */
#define CHUNK_COUNT     ( LOWMEM_ROM_SIZE / CHUNK_BYTES )

static BYTE*    rom_copy = NULL;
static BOOL     chunk_ok[ CHUNK_COUNT ];
static BOOL     rom_ok   = FALSE;

/*---------------------------------------------------------------------------
 * LooksLikeBios - does the copy hold a PC BIOS, or something else?
 *
 * The reset vector at FFFF0h is a far jump on every PC BIOS (EAh), or on a
 * few a near one (E9h).  A driver that answered with zeros, 0FFh bus float
 * or the wrong memory would not have one there.
 *-------------------------------------------------------------------------*/

static BOOL LooksLikeBios( void )
{
    const BYTE* reset = LowRom( 0x000FFFF0UL, 16 );

    if ( reset == NULL ) return FALSE;
    return reset[ 0 ] == 0xEA || reset[ 0 ] == 0xE9;
}

BOOL LowMemInit( void )
{
    HFILE           hf     = NULLHANDLE;
    ULONG           action = 0;
    ULONG           i;
    int             copied = 0;

    if ( rom_ok ) return TRUE;

    for ( i = 0; i < CHUNK_COUNT; ++i ) chunk_ok[ i ] = FALSE;

    if ( DosOpen( (PSZ)"TESTCFG$", &hf, &action, 0L, FILE_NORMAL,
                  OPEN_ACTION_OPEN_IF_EXISTS | OPEN_ACTION_FAIL_IF_NEW,
                  OPEN_FLAGS_NOINHERIT | OPEN_SHARE_DENYNONE | OPEN_ACCESS_READONLY,
                  NULL ) != NO_ERROR ) {
        return FALSE;
    }

    rom_copy = (BYTE*)malloc( LOWMEM_ROM_SIZE );
    if ( rom_copy == NULL ) {
        DosClose( hf );
        return FALSE;
    }
    memset( rom_copy, 0, LOWMEM_ROM_SIZE );

    for ( i = 0; i < CHUNK_COUNT; ++i ) {
        testcfg_param_t param;
        ULONG           param_len = sizeof( param );
        ULONG           data_len  = CHUNK_BYTES;

        param.command  = 0;
        param.physical = LOWMEM_ROM_BASE + i * CHUNK_BYTES;
        param.count    = (USHORT)CHUNK_BYTES;

        if ( DosDevIOCtl( hf, TESTCFG_CATEGORY, TESTCFG_READ_ROM,
                          &param, sizeof( param ), &param_len,
                          rom_copy + i * CHUNK_BYTES, CHUNK_BYTES, &data_len )
             == NO_ERROR ) {
            chunk_ok[ i ] = TRUE;
            ++copied;
        }
    }

    DosClose( hf );

    rom_ok = ( copied > 0 );
    if ( rom_ok && !LooksLikeBios() ) rom_ok = FALSE;

    if ( !rom_ok ) LowMemTerm();
    return rom_ok;
}

void LowMemTerm( void )
{
    if ( rom_copy != NULL ) {
        free( rom_copy );
        rom_copy = NULL;
    }
    rom_ok = FALSE;
}

BOOL LowMemAvailable( void )
{
    return rom_ok;
}

const BYTE* LowRom( ULONG linear, ULONG len )
{
    ULONG offset;
    ULONG first, last, i;

    /* rom_copy, not rom_ok: LooksLikeBios asks before rom_ok is settled */
    if ( rom_copy == NULL || len == 0 ) return NULL;
    if ( linear < LOWMEM_ROM_BASE ) return NULL;

    offset = linear - LOWMEM_ROM_BASE;
    if ( offset >= LOWMEM_ROM_SIZE || len > LOWMEM_ROM_SIZE - offset ) return NULL;

    first = offset / CHUNK_BYTES;
    last  = ( offset + len - 1 ) / CHUNK_BYTES;
    for ( i = first; i <= last; ++i ) {
        if ( !chunk_ok[ i ] ) return NULL;
    }

    return rom_copy + offset;
}
