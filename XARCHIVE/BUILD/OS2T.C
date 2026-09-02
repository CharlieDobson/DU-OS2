/*===========================================================================
 * OS2T.C - unit test for the two backend fixes, compiled from the OS/2 TREE'S
 * OWN SZARC.C through the OS/2 tree's own windows.h shim.
 *
 * Why a unit test and not an end-to-end extraction: the root-of-a-drive bug
 * is a doubled path separator, and NEITHER of the two places this can be run
 * (DOSBox, and Windows NT here) reproduces the failure - both forgive
 * "D:\\FILE.TXT" where real DOS reads it as a UNC name and OS/2 rejects it.
 * So the observable thing is the STRING BuildPath builds, not the file that
 * appears afterwards.  SZARC.C is #included rather than linked so that its
 * statics - BuildPath and SzDictCost are both static - are reachable.
 *
 *   wcl386 -bt=nt -l=nt -zq -i=<os2tree> -i=<os2tree>\..\..\COMMON os2t.c
 *===========================================================================*/
#include <stdio.h>
#include <string.h>

/* The backend under test, sources and all. */
#include "SZARC.C"

/*---- Stubs, per the szt.c recipe in the project notes ---------------------- *
 * ARCFILE.C is deliberately NOT linked: on this host it compiles its _WIN32
 * branch, which collides with the OS/2 tree's own windows.h shim - and the
 * shim is the whole point of building against these sources.
 *
 * Stubbing ArcFsName also ISOLATES what is under test.  BuildPath's job is
 * the JOIN; ArcFsName's is the name.  A verbatim stub means a failure here
 * can only be the join, which is the thing that changed.
 *---------------------------------------------------------------------------*/
void ArcFsName( char *dst, int dstSize, const char *name, int isDir )
{
    int i = 0;
    (void)isDir;                /* the 8.3 folder rule is ARCFILE's, not ours */
    while ( name[i] && i < dstSize - 1 ) { dst[i] = name[i]; i++; }
    dst[i] = '\0';
}

/* The verdict on the name ArcFsName just produced.  The real one lives in
 * ARCFILE.C, which cannot be linked here (see above); this stub is settable so
 * the test can put SZARC into each of the three states and check that it does
 * the right thing in each.  SZARC calling this AT ALL is new - the link error
 * this stub fixes is how the OS/2 tree found out about the change, which is
 * what MKTEST is for. */
static int g_verdict = ARC_NAME_OK;
int    ArcNameVerdict( void )             { return g_verdict; }

int    ArcWantWrite( const char *path )   { (void)path; return 1; }
int    ArcFlattenPaths( void )            { return 0; }
int    ArcCheckEntryCount( UInt32 n )     { (void)n; return 1; }

/* COMPAT.C cannot be linked either - its calendar half calls DosGetDateTime,
 * which does not exist on this host.  This is the only piece of it SZARC
 * reaches. */
char *xa_lstrcpyn( char *dst, const char *src, int count )
{
    int i;
    if ( count <= 0 ) return dst;
    for ( i = 0; i < count - 1 && src[i]; i++ ) dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}

int WideCharToMultiByte( unsigned cp, DWORD flags,
                         const WCHAR *wstr, int wlen,
                         char *dst, int dstBytes,
                         const char *defChar, int *usedDef )
{
    int i = 0;
    (void)cp; (void)flags; (void)wlen; (void)defChar; (void)usedDef;
    while ( wstr[i] && i < dstBytes - 1 )
    { dst[i] = (char)( wstr[i] & 0xFF ); i++; }
    dst[i] = '\0';
    return i + 1;
}

UInt32 ArcMaxDictSize( void )             { return 32UL * 1024 * 1024; }
UInt32 ArcMaxBufferSize( void )           { return 32UL * 1024 * 1024; }
void   SetFileMTime( const char *path, const FILETIME *ft )
{ (void)path; (void)ft; }

static int g_fail = 0;

static void CheckPath( const char *destDir, const char *name,
                       const char *want )
{
    char got[SZ_MAX_NAME * 4];

    /* 0 = "the last component is a file".  BuildPath's job is the JOIN and it
     * passes this straight through to ArcFsName, which is stubbed above - so
     * which one is used makes no difference here, and a file is the honest
     * description of "FILE.TXT". */
    BuildPath( got, sizeof( got ), destDir, name, 0 );
    if ( strcmp( got, want ) == 0 )
        printf( "  ok    dest=%-12s name=%-12s -> %s\n", destDir, name, got );
    else
    {
        printf( "  FAIL  dest=%-12s name=%-12s -> %s   (wanted %s)\n",
                destDir, name, got, want );
        ++g_fail;
    }
}

static void CheckDict( UInt32 dict, UInt32 unpack, UInt32 want )
{
    UInt32 got = SzDictCost( dict, unpack );

    if ( got == want )
        printf( "  ok    dict=%-10lu unpack=%-10lu -> %lu\n",
                (unsigned long)dict, (unsigned long)unpack,
                (unsigned long)got );
    else
    {
        printf( "  FAIL  dict=%-10lu unpack=%-10lu -> %lu   (wanted %lu)\n",
                (unsigned long)dict, (unsigned long)unpack,
                (unsigned long)got, (unsigned long)want );
        ++g_fail;
    }
}

int main( void )
{
    printf( "\nBuildPath - exactly one separator, whatever the destination\n" );

    /* The bug: a destination that already ends in a separator.  The root of a
     * drive is the only path you cannot write without one. */
    CheckPath( "C:\\",     "FILE.TXT", "C:\\FILE.TXT" );
    CheckPath( "C:/",      "FILE.TXT", "C:/FILE.TXT"  );
    CheckPath( "\\",       "FILE.TXT", "\\FILE.TXT"   );
    CheckPath( "D:\\SUB\\", "FILE.TXT", "D:\\SUB\\FILE.TXT" );

    /* And the ordinary case, which must not change. */
    CheckPath( "C:\\SUB",  "FILE.TXT", "C:\\SUB\\FILE.TXT" );
    CheckPath( "TOUT",     "FILE.TXT", "TOUT\\FILE.TXT" );
    CheckPath( "C:",       "FILE.TXT", "C:\\FILE.TXT" );

    printf( "\nSzDictCost - what the decode ALLOCATES, not what it declares\n" );

    /* The report: 64 MB declared over a folder that unpacks to 6 MB. */
    CheckDict( 67108864UL,  6000000UL,  6000000UL  );
    /* A folder bigger than its dictionary really does cost the dictionary. */
    CheckDict( 67108864UL,  200000000UL, 67108864UL );
    /* Equal, and the degenerate "not known here" case. */
    CheckDict( 8388608UL,   8388608UL,  8388608UL  );
    CheckDict( 67108864UL,  0UL,        67108864UL );

    /* The name verdict.  WriteDirEntry is the smallest caller that acts on it
     * and the one the "." bug went through: an archive entry named "." made a
     * directory called "_" under the destination, because a name that reduces
     * to nothing had nowhere to say so and fell back on the never-empty rule.
     *
     * MakeTree is what must not be reached.  There is no return value to check
     * so the test checks the filesystem: OK creates the directory, SKIP and
     * ABORT do not.  Cheap, and it fails for the right reason if the verdict
     * is ignored - which is exactly what the old code did. */
    printf( "\nArcNameVerdict - a name that produces nothing produces nothing\n" );
    {
        static const struct { int verdict; const char *label; int wantDir; }
        cases[] = {
            { ARC_NAME_OK,    "OK",    1 },
            { ARC_NAME_SKIP,  "SKIP",  0 },
            { ARC_NAME_ABORT, "ABORT", 0 },
        };
        SzEntry e;
        int     i;

        memset( &e, 0, sizeof( e ) );
        strcpy( e.name, "VERDICT" );
        e.isDir = 1;

        for ( i = 0; i < 3; i++ )
        {
            int made;
            _rmdir( "TVERD\\VERDICT" );
            _rmdir( "TVERD" );
            _mkdir( "TVERD" );

            g_verdict = cases[i].verdict;
            WriteDirEntry( &e, "TVERD" );
            g_verdict = ARC_NAME_OK;

            made = ( _rmdir( "TVERD\\VERDICT" ) == 0 );
            if ( made == cases[i].wantDir )
                printf( "  ok    verdict=%-5s -> directory %s\n",
                        cases[i].label, made ? "created" : "not created" );
            else
            {
                printf( "  FAIL  verdict=%-5s -> directory %s   (wanted %s)\n",
                        cases[i].label, made ? "created" : "not created",
                        cases[i].wantDir ? "created" : "not created" );
                ++g_fail;
            }
            _rmdir( "TVERD" );
        }
    }

    printf( "\n%s  (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS",
            g_fail, ( g_fail == 1 ) ? "" : "s" );
    return g_fail ? 1 : 0;
}
