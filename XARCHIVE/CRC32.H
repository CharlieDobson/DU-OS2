/*===========================================================================
 * CRC32.H  -  CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320)
 * Target: MSVC 2.2  Win32s
 *===========================================================================*/

#ifndef CRC32_H
#define CRC32_H

#include "arcdefs.h"

/* Running CRC.  Start with Crc32Init(), feed buffers, finish with
 * Crc32Done() which XORs with 0xFFFFFFFF. */
UInt32 Crc32Init( void );
UInt32 Crc32Update( UInt32 crc, const void *data, UInt32 len );
UInt32 Crc32Done( UInt32 crc );

/* One-shot helper: CRC of a single buffer. */
UInt32 Crc32Calc( const void *data, UInt32 len );

#endif /* CRC32_H */
