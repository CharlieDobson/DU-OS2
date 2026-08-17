/*===========================================================================
 * ARCPREF.C  -  Persisted user preferences (XARCHIVE.INI).  See ARCPREF.H.
 * Target: MSVC 2.2 Win32s / Open Watcom DOS / Open Watcom OS/2 PM
 *
 * Compiled byte-identical into the DU32 (Win32s), DOSSPIKE (DOS) and DU-OS2
 * (PM) builds, so all three read and write the same file the same way.  No
 * platform headers are used at all here - only stdio/string - which is what
 * keeps it portable across the three toolchains.
 *===========================================================================*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "arcdefs.h"     /* ArcSetFlattenPaths / ArcFlattenPaths */
#include "arcpref.h"

#define PREF_NAME      "XARCHIVE.INI"
#define PREF_SECTION   "[Options]"
#define PREF_PATH_MAX  260
#define PREF_LINE_MAX  128
#define PREF_KEEP_MAX  32       /* foreign lines preserved across a save */

static char g_ini[PREF_PATH_MAX] = PREF_NAME;

/*---- Path -----------------------------------------------------------------*/

void ArcPrefSetFileFromExe( const char *exePath )
{
    int cut = -1, i;

    if ( !exePath || !exePath[0] )
    {
        strcpy( g_ini, PREF_NAME );
        return;
    }

    /* Last separator wins; ':' covers a bare "C:PROG.EXE" DOS drive prefix. */
    for ( i = 0; exePath[i] && i < PREF_PATH_MAX - 1; i++ )
        if ( exePath[i] == '\\' || exePath[i] == '/' || exePath[i] == ':' )
            cut = i;

    if ( cut < 0 || cut + 1 + (int)strlen( PREF_NAME ) >= PREF_PATH_MAX )
    {
        strcpy( g_ini, PREF_NAME );      /* no directory part, or too long */
        return;
    }

    for ( i = 0; i <= cut; i++ ) g_ini[i] = exePath[i];
    g_ini[cut + 1] = '\0';
    strcat( g_ini, PREF_NAME );
}

const char *ArcPrefFile( void )
{
    return g_ini;
}

/*---- Parsing helpers ------------------------------------------------------*/

/* Strip CR/LF and trailing blanks, in place. */
static void TrimTail( char *s )
{
    int n = (int)strlen( s );
    while ( n > 0 && ( s[n-1] == '\n' || s[n-1] == '\r' ||
                       s[n-1] == ' '  || s[n-1] == '\t' ) )
        s[--n] = '\0';
}

static const char *SkipBlanks( const char *s )
{
    while ( *s == ' ' || *s == '\t' ) ++s;
    return s;
}

/* Case-insensitive compare of 'line's key part (up to '=') against 'key'. */
static int KeyIs( const char *line, const char *key )
{
    int i = 0;
    line = SkipBlanks( line );
    for ( i = 0; key[i]; i++ )
        if ( toupper( (unsigned char)line[i] ) != toupper( (unsigned char)key[i] ) )
            return 0;
    line = SkipBlanks( line + i );
    return *line == '=';
}

static const char *ValueOf( const char *line )
{
    const char *eq = strchr( line, '=' );
    return eq ? SkipBlanks( eq + 1 ) : "";
}

/* Is this one of ours?  Foreign keys are preserved verbatim on save. */
static int IsKnownKey( const char *line )
{
    return KeyIs( line, "ExtractFolders" );
}

/*---- Load -----------------------------------------------------------------*/

void ArcPrefLoad( void )
{
    FILE *f = fopen( g_ini, "r" );
    char  line[PREF_LINE_MAX];

    if ( !f ) return;                 /* no file yet: defaults stand */

    while ( fgets( line, sizeof( line ), f ) )
    {
        const char *p;
        TrimTail( line );
        p = SkipBlanks( line );
        if ( !*p || *p == ';' || *p == '#' || *p == '[' ) continue;

        if ( KeyIs( p, "ExtractFolders" ) )
        {
            /* Stored as the USER'S view - 1 = keep folder names - which is
             * the inverse of the backend's flatten flag.  Storing the
             * positive reading keeps the file readable by hand. */
            const char *v = ValueOf( p );
            ArcSetFlattenPaths( ( *v == '0' ) ? 1 : 0 );
        }
    }
    fclose( f );
}

/*---- Save -----------------------------------------------------------------*/

int ArcPrefSave( void )
{
    char  keep[PREF_KEEP_MAX][PREF_LINE_MAX];
    int   nkeep = 0;
    FILE *f;

    /* Read back anything we do not own, so a newer build's keys survive a
     * save by this one. */
    f = fopen( g_ini, "r" );
    if ( f )
    {
        char line[PREF_LINE_MAX];
        while ( fgets( line, sizeof( line ), f ) && nkeep < PREF_KEEP_MAX )
        {
            const char *p;
            TrimTail( line );
            p = SkipBlanks( line );
            if ( !*p || *p == ';' || *p == '#' || *p == '[' ) continue;
            if ( IsKnownKey( p ) ) continue;
            strcpy( keep[nkeep++], line );
        }
        fclose( f );
    }

    f = fopen( g_ini, "w" );
    if ( !f ) return 0;               /* read-only medium / full disk */

    fprintf( f, "; XArchive preferences\n" );
    fprintf( f, "%s\n", PREF_SECTION );
    fprintf( f, "ExtractFolders=%d\n", ArcFlattenPaths() ? 0 : 1 );
    {
        int i;
        for ( i = 0; i < nkeep; i++ )
            fprintf( f, "%s\n", keep[i] );
    }

    return ( fclose( f ) == 0 );
}
