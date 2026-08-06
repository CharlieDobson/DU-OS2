/*===========================================================================
 * ICO2OS2.C  -  build-time tool: Windows .ICO  ->  OS/2 2.x colour icon
 *
 * Host tool, not part of any application -- it has its own main() and is
 * never linked into one.  Shared by all the ports: each BUILD\MKOS2.CMD
 * builds it for the build machine (wcl386 -bt=nt) and runs it, passing its
 * own icon in and out, and the OS/2 icon it emits is what the .RC compiles
 * in.  It lives under COMMON\TOOLS rather than COMMON so that nothing which
 * sweeps COMMON for application sources can pick up a second main().
 *
 * Why this exists: "wrc -bt=os2" accepts an ICON statement pointing at a
 * Windows .ICO and embeds it without complaint, but it does not convert it --
 * the resource ends up holding an ICONDIR + Windows BITMAPINFOHEADER.  PM
 * validates icon resources strictly and rejects that outright: WinLoadPointer
 * fails, which fails FCF_ICON, which fails the whole WinCreateStdWindow with
 * 0x1034 PMERR_INVALID_RESOURCE_FORMAT and no other clue.
 *
 * The layout below is not reconstructed from prose -- it is copied from a
 * genuine OS/2 Warp 3 icon (\OS2\BITMAP), parsed field by field.  Three
 * things about it are easy to get wrong, and all three are fatal:
 *
 *  1. ONE 'BA' array entry per device form, holding TWO BITMAPFILEHEADERs --
 *     the mono mask first, then the colour bitmap -- each followed straight
 *     away by its own colour table.  offNext skips the whole pair.  A colour
 *     icon is NOT two array entries; put a second 'BA' where PM expects the
 *     colour bitmap's file header and it refuses the resource.
 *
 *  2. The VERSION 1 structures, not the "2" long forms: BITMAPARRAYFILEHEADER
 *     is 40 bytes, BITMAPFILEHEADER 26, and BITMAPINFOHEADER 12 with cbFix=12
 *     and USHORT cx/cy/cPlanes/cBitCount.  (The 92/78/64 long forms exist,
 *     but IBM's own icons are v1 and that is what is known to load.)
 *
 *  3. A v1 colour table is 3-byte RGB, so a Windows RGBQUAD does NOT copy
 *     verbatim -- its 4th padding byte has to be dropped.
 *
 *   Windows .ICO                     OS/2 colour icon ('CI')
 *   ---------------------------      -----------------------------------
 *   ICONDIR + ICONDIRENTRY           BITMAPARRAYFILEHEADER ('BA', 40)
 *   BITMAPINFOHEADER (40)            BITMAPFILEHEADER x2 ('CI', 26 each)
 *   RGBQUAD table (B,G,R,pad)        RGB table (B,G,R) - pad dropped
 *   XOR bits, then AND bits          mask bitmap (cy = 2h, 1 bpp):
 *                                        file rows 0..h-1  = XOR half (0)
 *                                        file rows h..2h-1 = AND half
 *                                    colour bitmap (cy = h)
 *
 * Both formats are bottom-up DIBs with DWORD-aligned rows, so the pixel
 * bytes themselves copy across untouched.
 *
 *   Usage:  ico2os2 <input.ico> <output.ptr>
 *===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

/* Packed on-disk sizes.  These are the sizes the OS/2 file format specifies,
   not sizeof() of the (compiler-padded) structs in <pmgpi.h>. */
#define SZ_BMIH         12      /* BITMAPINFOHEADER, cbFix = 12              */
#define SZ_BFH          ( 2 + 4 + 2 + 2 + 4 + SZ_BMIH )         /* 26        */
#define SZ_BAFH         ( 2 + 4 + 4 + 2 + 2 + SZ_BFH )          /* 40        */

#define BFT_BITMAPARRAY 0x4142  /* 'BA' */
#define BFT_COLORICON   0x4943  /* 'CI' */

/* ------------------------------------------------------------------------ */
/* little-endian readers / writers                                          */
/* ------------------------------------------------------------------------ */

static u16 rd16( const u8 *p ) { return (u16)( p[0] | ( p[1] << 8 ) ); }

static u32 rd32( const u8 *p )
{
    return (u32)p[0] | ( (u32)p[1] << 8 )
         | ( (u32)p[2] << 16 ) | ( (u32)p[3] << 24 );
}

static u8 *out;                 /* output buffer   */
static u32 outpos;              /* write cursor    */

static void wr8 ( u8  v ) { out[outpos++] = v; }
static void wr16( u16 v ) { wr8( (u8)( v & 0xFF ) ); wr8( (u8)( v >> 8 ) ); }
static void wr32( u32 v ) { wr16( (u16)( v & 0xFFFF ) ); wr16( (u16)( v >> 16 ) ); }

/* ------------------------------------------------------------------------ */
/* WriteFileHeader - one BITMAPFILEHEADER, info header included             */
/*                                                                          */
/*   Two of these go into a colour icon: the mono mask, then the colour      */
/*   bitmap.  Only the first is wrapped in the BITMAPARRAYFILEHEADER.        */
/* ------------------------------------------------------------------------ */

static void WriteFileHeader( u32 offBits, u32 cx, u32 cy, u16 bits,
                             u16 xhot, u16 yhot )
{
    wr16( BFT_COLORICON );
    wr32( SZ_BFH );             /* cbSize - the structure, not the file      */
    wr16( xhot );
    wr16( yhot );
    wr32( offBits );

    wr32( SZ_BMIH );            /* cbFix = 12                                */
    wr16( (u16)cx );            /* v1 keeps these as USHORTs                 */
    wr16( (u16)cy );
    wr16( 1 );                  /* cPlanes                                   */
    wr16( bits );               /* cBitCount                                 */
}

/* ------------------------------------------------------------------------ */

static u32 RowBytes( u32 cx, u32 bits )
{
    return ( ( cx * bits + 31 ) / 32 ) * 4;     /* DWORD-aligned rows        */
}

int main( int argc, char *argv[] )
{
    FILE *f;
    u8   *ico;
    long  icolen;
    u32   count, i, best = 0, bestpix = 0;
    u32   imgoff, cx, cy, bits, clrused;
    u32   xorstride, andstride, xorsize, andsize;
    const u8 *hdr, *pal, *xorbits, *andbits;
    u32   masktab_off, clrbfh_off, clrtab_off, maskbits_off, clrbits_off;
    u32   total, maskstride, masksize, row;
    u16   xhot, yhot;

    if ( argc != 3 ) {
        fprintf( stderr, "usage: ico2os2 <input.ico> <output.ptr>\n" );
        return 2;
    }

    /* ---- slurp the .ICO ---- */

    f = fopen( argv[1], "rb" );
    if ( !f ) { perror( argv[1] ); return 1; }

    fseek( f, 0, SEEK_END );
    icolen = ftell( f );
    fseek( f, 0, SEEK_SET );

    if ( icolen < 22 ) {
        fprintf( stderr, "%s: too small to be an icon\n", argv[1] );
        fclose( f );
        return 1;
    }

    ico = (u8 *)malloc( (size_t)icolen );
    if ( !ico || fread( ico, 1, (size_t)icolen, f ) != (size_t)icolen ) {
        fprintf( stderr, "%s: read failed\n", argv[1] );
        fclose( f );
        return 1;
    }
    fclose( f );

    if ( rd16( ico ) != 0 || rd16( ico + 2 ) != 1 ) {
        fprintf( stderr, "%s: not a Windows .ICO\n", argv[1] );
        return 1;
    }

    /* ---- pick the largest image in the directory ---- */

    count = rd16( ico + 4 );
    if ( count == 0 ) {
        fprintf( stderr, "%s: no images\n", argv[1] );
        return 1;
    }

    for ( i = 0; i < count; i++ ) {
        const u8 *e = ico + 6 + i * 16;
        u32 w = e[0] ? e[0] : 256;
        u32 h = e[1] ? e[1] : 256;
        if ( w * h >= bestpix ) { bestpix = w * h; best = i; }
    }

    imgoff = rd32( ico + 6 + best * 16 + 12 );
    if ( imgoff + 40 > (u32)icolen ) {
        fprintf( stderr, "%s: truncated image data\n", argv[1] );
        return 1;
    }

    /* ---- parse the embedded Windows BITMAPINFOHEADER ---- */

    hdr  = ico + imgoff;
    cx   = rd32( hdr + 4 );
    cy   = rd32( hdr + 8 ) / 2;     /* .ICO stores XOR+AND stacked           */
    bits = rd16( hdr + 14 );

    if ( rd32( hdr ) != 40 ) {
        fprintf( stderr, "%s: unsupported header size %lu (need 40)\n",
                 argv[1], (unsigned long)rd32( hdr ) );
        return 1;
    }
    if ( rd32( hdr + 16 ) != 0 ) {
        fprintf( stderr, "%s: compressed icons are not supported\n", argv[1] );
        return 1;
    }
    if ( bits > 8 ) {
        fprintf( stderr, "%s: %lu bpp icons are not supported "
                         "(need a palettised 1/4/8 bpp icon)\n",
                 argv[1], (unsigned long)bits );
        return 1;
    }
    if ( cx > 65535 || cy > 65535 ) {
        fprintf( stderr, "%s: %lux%lu is too large for the v1 headers\n",
                 argv[1], (unsigned long)cx, (unsigned long)cy );
        return 1;
    }

    clrused = rd32( hdr + 32 );
    if ( clrused == 0 ) clrused = 1UL << bits;

    pal     = hdr + 40;
    xorbits = pal + clrused * 4;

    xorstride = RowBytes( cx, bits );
    andstride = RowBytes( cx, 1 );
    xorsize   = xorstride * cy;
    andsize   = andstride * cy;
    andbits   = xorbits + xorsize;

    if ( (u32)( andbits + andsize - ico ) > (u32)icolen ) {
        fprintf( stderr, "%s: truncated bitmap data\n", argv[1] );
        return 1;
    }

    /* ---- lay the OS/2 file out ----

       One device form, marked device-independent, holding the mask/colour
       pair.  Every header and colour table comes first, then both blocks of
       pels, exactly as a real OS/2 icon is arranged:

         0   [ BITMAPARRAYFILEHEADER + the mask's BITMAPFILEHEADER ]  40
         40  [ mask colour table, 2 RGB entries                    ]   6
         46  [ the colour bitmap's BITMAPFILEHEADER                ]  26
         72  [ colour table, clrused RGB entries                   ]  48
         120 [ mask pels,   cx x 2cy, 1 bpp                        ] 256
         376 [ colour pels, cx x cy                                ] 512
                                                        (32x32, 4 bpp)     */

    maskstride   = andstride;
    masksize     = maskstride * cy * 2;

    masktab_off  = SZ_BAFH;                     /* mask's colour table      */
    clrbfh_off   = masktab_off + 2 * 3;         /* colour's file header     */
    clrtab_off   = clrbfh_off + SZ_BFH;         /* colour's colour table    */
    maskbits_off = clrtab_off + clrused * 3;    /* then the pels            */
    clrbits_off  = maskbits_off + masksize;
    total        = clrbits_off + xorsize;

    out = (u8 *)calloc( 1, total );
    if ( !out ) { fprintf( stderr, "out of memory\n" ); return 1; }
    outpos = 0;

    /* Hotspots are meaningless for an icon, but real ones carry the centre
       rather than 0,0, so match that.                                      */
    xhot = (u16)( cx / 2 );
    yhot = (u16)( cy / 2 );

    /* BITMAPARRAYFILEHEADER - the only device form, so offNext = 0 and the
       display size is left at 0x0 meaning "any device".                    */
    wr16( BFT_BITMAPARRAY );
    wr32( SZ_BAFH );
    wr32( 0 );                                  /* offNext                  */
    wr16( 0 );                                  /* cxDisplay                */
    wr16( 0 );                                  /* cyDisplay                */

    /* ...wrapping the mask's file header, then the mask's colour table */
    WriteFileHeader( maskbits_off, cx, cy * 2, 1, xhot, yhot );
    wr8( 0x00 ); wr8( 0x00 ); wr8( 0x00 );      /* black                    */
    wr8( 0xFF ); wr8( 0xFF ); wr8( 0xFF );      /* white                    */

    /* the colour bitmap's file header, then its colour table.  RGBQUAD is
       B,G,R,pad and RGB is B,G,R, so the pad byte is dropped per entry.    */
    WriteFileHeader( clrbits_off, cx, cy, (u16)bits, xhot, yhot );
    for ( i = 0; i < clrused; i++ ) {
        wr8( pal[i * 4 + 0] );
        wr8( pal[i * 4 + 1] );
        wr8( pal[i * 4 + 2] );
    }

    /* mask pels: bottom half = XOR (all zero), top half = the .ICO AND mask.
       Both halves are bottom-up, so the AND rows copy across in file order. */
    outpos = maskbits_off + maskstride * cy;        /* skip the zeroed XOR   */
    for ( row = 0; row < cy; row++ ) {
        memcpy( out + outpos, andbits + row * andstride, andstride );
        outpos += maskstride;
    }

    /* colour pels, verbatim */
    memcpy( out + clrbits_off, xorbits, xorsize );

    /* ---- write it out ---- */

    f = fopen( argv[2], "wb" );
    if ( !f ) { perror( argv[2] ); return 1; }

    if ( fwrite( out, 1, total, f ) != total ) {
        fprintf( stderr, "%s: write failed\n", argv[2] );
        fclose( f );
        return 1;
    }
    fclose( f );

    printf( "ico2os2: %s -> %s  (%lux%lu, %lu bpp, %lu bytes)\n",
            argv[1], argv[2],
            (unsigned long)cx, (unsigned long)cy,
            (unsigned long)bits, (unsigned long)total );
    return 0;
}
