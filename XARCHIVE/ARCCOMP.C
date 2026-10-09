/*===========================================================================
 * ARCCOMP.C  -  Making an archive: finding the files, leaving some out, and
 *               deciding how each one is packed
 * Target: MSVC 2.2  Win32s
 *
 * See ARCCOMP.H for the interface and ARCCOMPI.H for the entry table the two
 * writers (ZIPWRITE.C, SZWRITE.C) are handed.
 *
 * WHAT GOES IN.  Each item names a file, a folder or a wildcard.  A folder is
 * taken whole.  A wildcard is matched in its own folder, against folders as
 * well as files, and a folder it matches is taken whole too - so "*" archives
 * a directory tree the way it does in 7-Zip.  With recursion on, a wildcard
 * is ALSO looked for in every folder below, which is PKZIP's -r.  Names are
 * stored relative to the folder the item was named in: "C:\WORK\SRC" goes in
 * as SRC\..., never as WORK\SRC\... or C:\WORK\SRC\....
 *
 * WHAT IS LEFT OUT.  Anything an exclusion matches (a folder takes its whole
 * tree with it), the archive being written, and - only in a flat archive or
 * when two items bring the same name - a second entry under a name already
 * used.  Hidden and system files are NOT left out: the point of keeping
 * attributes is to keep files like these as they were.
 *
 * HOW EACH FILE IS PACKED is the writer's decision, made from CompSniff's
 * reading of the file's first 16 KB: compressed formats it knows by their
 * signatures, anything statistically indistinguishable from noise, x86
 * executables (PE, LE, LX) that the 7z branch filter helps, and text.
 *
 * NO RECURSION ON THE STACK.  A folder walk descends one level per folder,
 * and each level's search record and two path buffers come from the heap, so
 * a deep tree costs the 128 KB DOS stack (and the OS/2 worker thread's)
 * nothing but a few dozen bytes a level.
 *===========================================================================*/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arccompi.h"
#include "platform.h"
#include "crc32.h"

#define WALK_MAX_DEPTH   100
#define SNIFF_BYTES      16384U

/*===========================================================================
 * Small string helpers
 *===========================================================================*/
static int IsSep( char c ) { return c == '\\' || c == '/'; }

static int LowerC( int c )
{
    return ( c >= 'A' && c <= 'Z' ) ? c - 'A' + 'a' : c;
}

/* Case-insensitive compare that treats '/' and '\' as the same character -
 * one archive can hold both. */
static int PathCmp( const char *a, const char *b )
{
    for ( ;; )
    {
        int ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if ( IsSep( (char)ca ) ) ca = '\\';
        if ( IsSep( (char)cb ) ) cb = '\\';
        ca = LowerC( ca );
        cb = LowerC( cb );
        if ( ca != cb ) return ca - cb;
        if ( ca == 0 ) return 0;
    }
}

static char *DupStr( const char *s )
{
    size_t n = strlen( s ) + 1;
    char  *d = (char *)malloc( n );
    if ( d ) memcpy( d, s, n );
    return d;
}

static const char *LeafOf( const char *path )
{
    const char *p, *leaf = path;
    for ( p = path; *p; p++ )
        if ( IsSep( *p ) || *p == ':' ) leaf = p + 1;
    return leaf;
}

/* Join a folder and a name with exactly one separator between them. */
static void JoinPath( char *dst, int size, const char *dir, const char *name )
{
    int n;
    lstrcpyn( dst, dir, size );
    n = (int)strlen( dst );
    if ( n > 0 && !IsSep( dst[n - 1] ) && dst[n - 1] != ':' && n < size - 1 )
    {
        dst[n++] = '\\';
        dst[n] = '\0';
    }
    lstrcpyn( dst + n, name, size - n );
}

/*===========================================================================
 * Wildcards
 *
 * '*' is any run of characters (separators included, so "C:\TEMP\*" means
 * everything under TEMP), '?' is one, case does not count and '/' is '\'.
 * A name with no '.' is also tried with one on the end, which is the DOS
 * reading: "*.*" matches README, and "*." matches only names like it.
 *===========================================================================*/
static int WildRaw( const char *p, const char *s )
{
    const char *starP = 0, *starS = 0;

    for ( ;; )
    {
        if ( *p == '*' )
        {
            while ( *p == '*' ) p++;
            if ( !*p ) return 1;
            starP = p;
            starS = s;
            continue;
        }
        if ( !*s ) return *p == '\0';
        if ( *p == '?' ||
             LowerC( (unsigned char)( IsSep( *p ) ? '\\' : *p ) ) ==
             LowerC( (unsigned char)( IsSep( *s ) ? '\\' : *s ) ) )
        {
            p++;
            s++;
            continue;
        }
        if ( !starP ) return 0;
        p = starP;
        s = ++starS;
    }
}

static int WildMatch( const char *pattern, const char *name )
{
    char withDot[SZ_MAX_NAME + 2];
    int  n;

    if ( WildRaw( pattern, name ) ) return 1;
    if ( strchr( LeafOf( name ), '.' ) ) return 0;
    n = (int)strlen( name );
    if ( n >= SZ_MAX_NAME ) return 0;
    memcpy( withDot, name, (size_t)n );
    withDot[n] = '.';
    withDot[n + 1] = '\0';
    return WildRaw( pattern, withDot );
}

static int HasWild( const char *s )
{
    return strchr( s, '*' ) != 0 || strchr( s, '?' ) != 0;
}

/*===========================================================================
 * The job
 *===========================================================================*/
ArcCompJob *ArcCompCreate( void )
{
    ArcCompJob *j = (ArcCompJob *)calloc( 1, sizeof( ArcCompJob ) );
    if ( !j ) return 0;
    j->fmt = ARC_CF_ZIP;
    j->keepPaths = 1;
    return j;
}

static void FreeList( char **list, int n )
{
    int i;
    if ( !list ) return;
    for ( i = 0; i < n; i++ ) free( list[i] );
    free( list );
}

static void FreeEntries( ArcCompJob *j )
{
    UInt32 i;
    if ( j->ents )
        for ( i = 0; i < j->nEnts; i++ ) free( j->ents[i].rel );
    free( j->ents );
    free( j->order );
    j->ents = 0;
    j->order = 0;
    j->nEnts = j->capEnts = j->nOrder = 0;
    FreeList( j->bases, j->nBases );
    j->bases = 0;
    j->nBases = 0;
}

void ArcCompFree( ArcCompJob *j )
{
    if ( !j ) return;
    FreeEntries( j );
    FreeList( j->items, j->nItems );
    FreeList( j->excl, j->nExcl );
    free( j );
}

int ArcCompFormatFromName( const char *archivePath )
{
    const char *leaf = LeafOf( archivePath ? archivePath : "" );
    const char *dot = strrchr( leaf, '.' );
    return ( dot && PathCmp( dot, ".7z" ) == 0 ) ? ARC_CF_7Z : ARC_CF_ZIP;
}

const char *ArcCompFormatName( int fmt )
{
    return ( fmt == ARC_CF_7Z ) ? "7z" : "Zip";
}

void ArcCompSetFormat( ArcCompJob *j, int fmt )   { j->fmt = fmt; }
void ArcCompSetPaths( ArcCompJob *j, int keep )   { j->keepPaths = keep ? 1 : 0; }
void ArcCompSetRecurse( ArcCompJob *j, int on )   { j->recurse = on ? 1 : 0; }
void ArcCompSetClearArchive( ArcCompJob *j, int on ) { j->clearArc = on ? 1 : 0; }

static int ListAdd( char ***list, int *n, const char *s )
{
    char **nl = (char **)realloc( *list, ( (size_t)*n + 1 ) * sizeof( char * ) );
    if ( !nl ) return SZ_ERR_MEMORY;
    *list = nl;
    if ( ( nl[*n] = DupStr( s ) ) == 0 ) return SZ_ERR_MEMORY;
    ( *n )++;
    return SZ_OK;
}

void CompSetProblem( ArcCompJob *j, const char *name )
{
    if ( !j->problem[0] ) lstrcpyn( j->problem, name ? name : "", SZ_MAX_NAME );
}

int ArcCompAdd( ArcCompJob *j, const char *path )
{
    if ( !path || !path[0] ) return SZ_OK;
    return ListAdd( &j->items, &j->nItems, path );
}

/* One pattern: trimmed, separators made '\', empty ones dropped. */
static int AddOneExclusion( ArcCompJob *j, const char *s, int len )
{
    char pat[SZ_MAX_NAME];
    int  i;

    while ( len > 0 && ( *s == ' ' || *s == '\t' ) ) { s++; len--; }
    while ( len > 0 && ( s[len - 1] == ' ' || s[len - 1] == '\t' ||
                         s[len - 1] == '\r' || s[len - 1] == '\n' ) ) len--;
    if ( len <= 0 ) return SZ_OK;
    if ( len >= SZ_MAX_NAME ) len = SZ_MAX_NAME - 1;
    for ( i = 0; i < len; i++ ) pat[i] = (char)( s[i] == '/' ? '\\' : s[i] );
    pat[len] = '\0';
    return ListAdd( &j->excl, &j->nExcl, pat );
}

/* A list file: one pattern a line, PKZIP's comment rules. */
static int AddExclusionFile( ArcCompJob *j, const char *path )
{
    FILE *f = fopen( path, "r" );
    char  line[SZ_MAX_NAME + 64];
    int   rc = SZ_OK;

    if ( !f )
    {
        CompSetProblem( j, path );
        return SZ_ERR_OPEN;
    }
    while ( rc == SZ_OK && fgets( line, sizeof( line ), f ) )
    {
        char *p = line, *c;
        while ( *p == ' ' || *p == '\t' ) p++;
        if ( *p == ';' || *p == '\0' || *p == '\n' || *p == '\r' ) continue;
        /* "name.txt   ;comment" - the comment starts at a ';' after blanks */
        for ( c = p; *c; c++ )
            if ( *c == ';' && c > p && ( c[-1] == ' ' || c[-1] == '\t' ) )
            {
                *c = '\0';
                break;
            }
        rc = AddOneExclusion( j, p, (int)strlen( p ) );
    }
    fclose( f );
    return rc;
}

int ArcCompExclude( ArcCompJob *j, const char *spec )
{
    const char *s = spec;
    int rc = SZ_OK;

    if ( !spec ) return SZ_OK;
    while ( *s && rc == SZ_OK )
    {
        const char *e = s;
        int len;
        while ( *e && *e != ';' ) e++;
        len = (int)( e - s );
        while ( len > 0 && ( *s == ' ' || *s == '\t' ) ) { s++; len--; }
        if ( len > 0 && *s == '@' )
        {
            char path[SZ_MAX_NAME];
            int  n = len - 1;
            if ( n >= SZ_MAX_NAME ) n = SZ_MAX_NAME - 1;
            memcpy( path, s + 1, (size_t)n );
            path[n] = '\0';
            while ( n > 0 && ( path[n - 1] == ' ' || path[n - 1] == '\t' ) ) path[--n] = '\0';
            rc = AddExclusionFile( j, path );
        }
        else
            rc = AddOneExclusion( j, s, len );
        s = *e ? e + 1 : e;
    }
    return rc;
}

/* Does an exclusion take this file or folder out?  'rel' is its path inside
 * the archive (before any flattening) and 'full' its path on disk. */
static int Excluded( ArcCompJob *j, const char *rel, const char *full )
{
    int i;
    for ( i = 0; i < j->nExcl; i++ )
    {
        const char *p = j->excl[i];
        if ( p[0] == '\\' || ( p[0] && p[1] == ':' ) )
        {
            if ( WildMatch( p, full ) ) return 1;
        }
        else if ( strchr( p, '\\' ) )
        {
            if ( WildMatch( p, rel ) ) return 1;
        }
        else if ( WildMatch( p, LeafOf( rel ) ) )
            return 1;
    }
    return 0;
}

/*===========================================================================
 * Progress
 *===========================================================================*/
int CompProgress( ArcCompJob *j, int index, int count, const char *name )
{
    if ( j->cancelled ) return 0;
    if ( j->prog &&
         !j->prog( j->user, index, count, name ? name : "",
                   j->doneBytes / 1024, j->totalBytes / 1024 ) )
    {
        j->cancelled = 1;
        return 0;
    }
    return 1;
}

/*===========================================================================
 * The entry table
 *===========================================================================*/
static int BaseIndex( ArcCompJob *j, const char *dir )
{
    int i;
    for ( i = 0; i < j->nBases; i++ )
        if ( PathCmp( j->bases[i], dir ) == 0 ) return i;
    if ( j->nBases >= 65535 ) return -1;
    if ( ListAdd( &j->bases, &j->nBases, dir ) != SZ_OK ) return -1;
    return j->nBases - 1;
}

static int NewEntry( ArcCompJob *j, int base, const char *rel,
                     const PlatFind *f, int isDir )
{
    CompEntry *e;

    if ( j->nEnts >= ARC_MAX_ENTRIES ) return SZ_ERR_TOOBIG;
    if ( j->nEnts == j->capEnts )
    {
        UInt32 cap = j->capEnts ? j->capEnts * 2 : 256;
        CompEntry *n = (CompEntry *)realloc( j->ents, cap * sizeof( CompEntry ) );
        if ( !n ) return SZ_ERR_MEMORY;
        j->ents = n;
        j->capEnts = cap;
    }
    e = &j->ents[j->nEnts];
    memset( e, 0, sizeof( *e ) );
    if ( ( e->rel = DupStr( rel ) ) == 0 ) return SZ_ERR_MEMORY;
    e->base    = (UInt16)base;
    e->isDir   = (Byte)( isDir ? 1 : 0 );
    e->attr    = f->attr | ( isDir ? 0x10 : 0 );
    if ( !isDir ) e->attr &= ~0x10UL;
    e->size    = isDir ? 0 : f->size;
    e->dosDate = f->dosDate;
    e->dosTime = f->dosTime;
    e->mtLo    = f->mtime.dwLowDateTime;
    e->mtHi    = f->mtime.dwHighDateTime;
    j->nEnts++;
    if ( !isDir ) j->totalBytes += e->size;
    return SZ_OK;
}

const char *CompStoredName( ArcCompJob *j, const CompEntry *e )
{
    return j->keepPaths ? e->rel : LeafOf( e->rel );
}

void CompSourcePath( ArcCompJob *j, const CompEntry *e, char *dst, int size )
{
    JoinPath( dst, size, j->bases[e->base], e->rel );
}

FILE *CompOpenSource( ArcCompJob *j, CompEntry *e )
{
    char  path[SZ_MAX_NAME * 2];
    FILE *f;

    CompSourcePath( j, e, path, sizeof( path ) );
    f = fopen( path, "rb" );
    if ( !f )
    {
        e->state = CS_SKIPPED;
        j->nUnread++;
        CompSetProblem( j, path );
    }
    return f;
}

/*===========================================================================
 * Walking
 *===========================================================================*/
static int WalkFolder( ArcCompJob *j, int base, const char *rel,
                       const char *full, const char *pattern, int depth );

/* A file or folder found at 'rel' under 'base' ('full' on disk). */
static int AddFound( ArcCompJob *j, int base, const char *rel,
                     const char *full, const PlatFind *f, int depth )
{
    int rc;

    if ( Excluded( j, rel, full ) ) return SZ_OK;

    if ( !( f->attr & 0x10 ) )
    {
        if ( PathCmp( full, j->outFull ) == 0 ) return SZ_OK;  /* ourselves */
        if ( f->tooBig )
        {
            j->nBig++;
            CompSetProblem( j, full );
            return SZ_OK;
        }
        if ( strlen( rel ) >= SZ_MAX_NAME - 1 )
        {
            j->nLong++;
            CompSetProblem( j, full );
            return SZ_OK;
        }
        if ( ( j->nEnts & 31 ) == 0 &&
             !CompProgress( j, (int)j->nEnts, ARC_COMP_SCANNING, rel ) )
            return SZ_ERR_CANCEL;
        return NewEntry( j, base, rel, f, 0 );
    }

    /* A folder: its own entry (kept only when folder names are), then all
     * of it. */
    if ( strlen( rel ) >= SZ_MAX_NAME - 2 )
    {
        j->nLong++;
        CompSetProblem( j, full );
        return SZ_OK;
    }
    if ( j->keepPaths )
    {
        rc = NewEntry( j, base, rel, f, 1 );
        if ( rc != SZ_OK ) return rc;
    }
    return WalkFolder( j, base, rel, full, 0, depth + 1 );
}

/* Everything in folder 'full' (archived as 'rel', "" at a base), or with a
 * pattern, the files and folders matching it - and with recursion on, the
 * matches in the folders below as well. */
static int WalkFolder( ArcCompJob *j, int base, const char *rel,
                       const char *full, const char *pattern, int depth )
{
    PlatFind *f;
    char     *spec, *crel, *cfull;
    void     *h;
    int       rc = SZ_OK;

    if ( depth > WALK_MAX_DEPTH ) return SZ_OK;

    f     = (PlatFind *)malloc( sizeof( PlatFind ) );
    spec  = (char *)malloc( SZ_MAX_NAME * 2 );
    crel  = (char *)malloc( SZ_MAX_NAME * 2 );
    cfull = (char *)malloc( SZ_MAX_NAME * 2 );
    if ( !f || !spec || !crel || !cfull )
    {
        free( f ); free( spec ); free( crel ); free( cfull );
        return SZ_ERR_MEMORY;
    }

    JoinPath( spec, SZ_MAX_NAME * 2, full, "*.*" );
    h = PlatFindFirst( spec, f );
    if ( h )
    {
        do {
            if ( f->name[0] == '.' &&
                 ( f->name[1] == '\0' || ( f->name[1] == '.' && f->name[2] == '\0' ) ) )
                continue;
            if ( rel[0] ) JoinPath( crel, SZ_MAX_NAME * 2, rel, f->name );
            else          lstrcpyn( crel, f->name, SZ_MAX_NAME * 2 );
            JoinPath( cfull, SZ_MAX_NAME * 2, full, f->name );

            if ( !pattern || WildMatch( pattern, f->name ) )
                rc = AddFound( j, base, crel, cfull, f, depth );
            else if ( ( f->attr & 0x10 ) && j->recurse &&
                      !Excluded( j, crel, cfull ) )
                rc = WalkFolder( j, base, crel, cfull, pattern, depth + 1 );
        } while ( rc == SZ_OK && PlatFindNext( h, f ) );
        PlatFindClose( h );
    }

    free( f ); free( spec ); free( crel ); free( cfull );
    return rc;
}

/* "C:\" and "\\SERVER\SHARE\" have no entry of their own to find. */
static int IsRoot( const char *full )
{
    int n = (int)strlen( full );
    if ( n == 3 && full[1] == ':' && IsSep( full[2] ) ) return 1;
    if ( n >= 2 && IsSep( full[0] ) && IsSep( full[1] ) )
    {
        int seps = 0;
        const char *p;
        for ( p = full + 2; *p; p++ )
            if ( IsSep( *p ) && p[1] ) seps++;
        return seps <= 1;
    }
    return 0;
}

static int ScanItem( ArcCompJob *j, const char *item )
{
    char     full[SZ_MAX_NAME * 2], dir[SZ_MAX_NAME * 2];
    PlatFind *f;
    const char *leaf;
    void    *h;
    int      n, base, rc = SZ_OK;
    UInt32   before = j->nEnts;

    if ( !_fullpath( full, item, sizeof( full ) ) )
        lstrcpyn( full, item, sizeof( full ) );
    for ( n = 0; full[n]; n++ )
        if ( full[n] == '/' ) full[n] = '\\';

    /* A trailing separator names the folder itself - except on a root. */
    n = (int)strlen( full );
    while ( n > 1 && IsSep( full[n - 1] ) && !IsRoot( full ) ) full[--n] = '\0';

    f = (PlatFind *)malloc( sizeof( PlatFind ) );
    if ( !f ) return SZ_ERR_MEMORY;

    if ( IsRoot( full ) )
    {
        /* A whole drive: its contents, with nothing in front of their names. */
        base = BaseIndex( j, full );
        rc = ( base < 0 ) ? SZ_ERR_MEMORY : WalkFolder( j, base, "", full, 0, 0 );
        free( f );
        return rc;
    }

    leaf = LeafOf( full );
    lstrcpyn( dir, full, (int)( leaf - full ) + 1 );
    base = BaseIndex( j, dir );
    if ( base < 0 ) { free( f ); return SZ_ERR_MEMORY; }

    if ( HasWild( leaf ) )
    {
        /* Missing only if the wildcard matched nothing at all: one whose
         * matches were all excluded was not missing, and with recursion a
         * match in a folder below counts. */
        int found;
        h = PlatFindFirst( full, f );
        found = ( h != 0 );
        if ( h ) PlatFindClose( h );
        rc = WalkFolder( j, base, "", dir, leaf, 0 );
        if ( rc == SZ_OK && !found && j->nEnts == before )
        {
            j->nMissing++;
            CompSetProblem( j, item );
        }
    }
    else
    {
        h = PlatFindFirst( full, f );
        if ( !h )
        {
            j->nMissing++;
            CompSetProblem( j, item );
        }
        else
        {
            PlatFindClose( h );
            rc = AddFound( j, base, leaf, full, f, 0 );
        }
    }
    free( f );
    return rc;
}

/*===========================================================================
 * Duplicate names
 *
 * Sorted by stored name (scan order breaking ties), so each run of equal
 * names is adjacent and the first one found is the one kept.  Folders that
 * coincide are merged silently - two items bringing the same folder name is
 * one folder in the archive, not a loss - but a file is counted, because a
 * file that went missing from the archive is something the user must hear.
 *===========================================================================*/
static ArcCompJob *g_sortJob;

static int DupCmp( const void *a, const void *b )
{
    UInt32 x = *(const UInt32 *)a, y = *(const UInt32 *)b;
    int c = PathCmp( CompStoredName( g_sortJob, &g_sortJob->ents[x] ),
                     CompStoredName( g_sortJob, &g_sortJob->ents[y] ) );
    if ( c ) return c;
    return ( x < y ) ? -1 : ( x > y ) ? 1 : 0;
}

static int DropDuplicates( ArcCompJob *j )
{
    UInt32 *idx, i;

    if ( j->nEnts < 2 ) return SZ_OK;
    idx = (UInt32 *)malloc( j->nEnts * sizeof( UInt32 ) );
    if ( !idx ) return SZ_ERR_MEMORY;
    for ( i = 0; i < j->nEnts; i++ ) idx[i] = i;
    g_sortJob = j;
    qsort( idx, j->nEnts, sizeof( UInt32 ), DupCmp );
    for ( i = 1; i < j->nEnts; i++ )
    {
        CompEntry *prev = &j->ents[idx[i - 1]];
        CompEntry *cur  = &j->ents[idx[i]];

        if ( PathCmp( CompStoredName( j, prev ), CompStoredName( j, cur ) ) != 0 )
            continue;
        /* The first of each run of equal names was kept; this one goes.  A
         * FILE that goes is counted and can be named, since its data is not
         * in the archive.  A folder that goes loses nothing: what is in it
         * still goes in under the same name. */
        cur->state = CS_DROPPED;
        if ( !cur->isDir )
        {
            char path[SZ_MAX_NAME * 2];
            j->nDup++;
            CompSourcePath( j, cur, path, sizeof( path ) );
            CompSetProblem( j, path );
        }
    }
    free( idx );
    return SZ_OK;
}

/*===========================================================================
 * Scan
 *===========================================================================*/
int ArcCompScan( ArcCompJob *j, const char *archivePath,
                 ArcCompProgress prog, void *user )
{
    int    i, rc = SZ_OK;
    UInt32 k, files = 0;

    FreeEntries( j );
    j->scanned = 0;
    j->totalBytes = 0;
    j->nDup = j->nUnread = j->nLong = j->nBig = j->nMissing = 0;
    j->problem[0] = '\0';
    j->cancelled = 0;
    j->prog = prog;
    j->user = user;
    j->doneBytes = 0;

    if ( !archivePath || !_fullpath( j->outFull, archivePath, sizeof( j->outFull ) ) )
        lstrcpyn( j->outFull, archivePath ? archivePath : "", sizeof( j->outFull ) );

    for ( i = 0; i < j->nItems && rc == SZ_OK; i++ )
        rc = ScanItem( j, j->items[i] );
    if ( rc == SZ_OK && j->cancelled ) rc = SZ_ERR_CANCEL;
    if ( rc == SZ_OK ) rc = DropDuplicates( j );
    if ( rc != SZ_OK ) return rc;

    /* The order the writers go through: scan order, without the dropped. */
    j->order = (UInt32 *)malloc( ( j->nEnts ? j->nEnts : 1 ) * sizeof( UInt32 ) );
    if ( !j->order ) return SZ_ERR_MEMORY;
    j->nOrder = 0;
    j->totalBytes = 0;
    for ( k = 0; k < j->nEnts; k++ )
    {
        if ( j->ents[k].state == CS_DROPPED ) continue;
        j->order[j->nOrder++] = k;
        if ( !j->ents[k].isDir )
        {
            files++;
            j->totalBytes += j->ents[k].size;
        }
    }

    if ( files == 0 ) return SZ_ERR_NOFILES;

    /* Past what one archive can hold here: a zip's directory counts entries
     * in 16 bits, and every offset this program writes is a signed 'long'.
     * The data limit allows for the archive coming out no smaller than what
     * went in, which is what happens to data that will not compress. */
    if ( j->fmt == ARC_CF_ZIP && j->nOrder > 65535UL ) return SZ_ERR_TOOBIG;
    if ( j->totalBytes > 0x7C000000UL ) return SZ_ERR_TOOBIG;

    j->scanned = 1;
    return SZ_OK;
}

int    ArcCompCount( ArcCompJob *j )   { return (int)j->nOrder; }
UInt32 ArcCompTotalKB( ArcCompJob *j ) { return ( j->totalBytes + 1023 ) / 1024; }

/*===========================================================================
 * Sniffing: how should this file be packed?
 *===========================================================================*/
static int StartsWith( const Byte *b, UInt32 n, const char *sig, UInt32 len )
{
    return n >= len && memcmp( b, sig, len ) == 0;
}

static UInt32 Get32( const Byte *p )
{
    return (UInt32)p[0] | ( (UInt32)p[1] << 8 ) | ( (UInt32)p[2] << 16 ) | ( (UInt32)p[3] << 24 );
}

int CompSniff( const Byte *b, UInt32 n, int *isText )
{
    static const Byte sig7z[6] = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };
    static const Byte sigXz[6] = { 0xFD, '7', 'z', 'X', 'Z', 0 };
    static const Byte sigPng[4] = { 0x89, 'P', 'N', 'G' };
    UInt32 counts[256];
    UInt32 i, ctrl = 0;

    if ( isText ) *isText = 0;
    if ( n == 0 ) return CK_DATA;

    /* Text: no NULs, hardly any control characters other than the usual
     * whitespace (and a ^Z at the end, the DOS way). */
    for ( i = 0; i < n; i++ )
    {
        Byte c = b[i];
        if ( c == 0 ) { ctrl = n; break; }
        if ( c < 32 && c != 9 && c != 10 && c != 13 && c != 12 && c != 26 ) ctrl++;
    }
    if ( isText ) *isText = ( ctrl * 100 <= n );

    /* Already compressed, by signature. */
    if ( StartsWith( b, n, "PK\3\4", 4 ) || StartsWith( b, n, "PK\5\6", 4 ) ||
         StartsWith( b, n, "PK\7\x8", 4 ) )                       return CK_PACKED;
    if ( n >= 6 && memcmp( b, sig7z, 6 ) == 0 )                    return CK_PACKED;
    if ( n >= 6 && memcmp( b, sigXz, 6 ) == 0 )                    return CK_PACKED;
    if ( StartsWith( b, n, "Rar!\x1A\x7", 6 ) )                    return CK_PACKED;
    if ( n >= 3 && b[0] == 0x1F && b[1] == 0x8B && b[2] == 8 )     return CK_PACKED;
    if ( StartsWith( b, n, "BZh", 3 ) && n > 3 && b[3] >= '1' && b[3] <= '9' )
                                                                   return CK_PACKED;
    if ( StartsWith( b, n, "MSCF", 4 ) )                           return CK_PACKED;
    if ( StartsWith( b, n, "SZDD", 4 ) || StartsWith( b, n, "KWAJ", 4 ) )
                                                                   return CK_PACKED;
    if ( n >= 2 && b[0] == 0x60 && b[1] == 0xEA )                  return CK_PACKED;  /* ARJ */
    if ( n >= 7 && b[2] == '-' && b[3] == 'l' &&
         ( b[4] == 'h' || b[4] == 'z' ) && b[6] == '-' )           return CK_PACKED;  /* LHA */
    if ( StartsWith( b, n, "ZOO ", 4 ) )                           return CK_PACKED;
    if ( n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF )  return CK_PACKED;  /* JPEG */
    if ( n >= 4 && memcmp( b, sigPng, 4 ) == 0 )                   return CK_PACKED;
    if ( StartsWith( b, n, "GIF8", 4 ) )                           return CK_PACKED;
    if ( StartsWith( b, n, "OggS", 4 ) || StartsWith( b, n, "fLaC", 4 ) )
                                                                   return CK_PACKED;
    if ( StartsWith( b, n, "ID3", 3 ) )                            return CK_PACKED;  /* MP3 */

    /* x86 code the branch filter helps: 32-bit PE (i386 / x64), and the LE
     * and LX of VxDs and OS/2.  A 16-bit MZ or NE program is left to plain
     * LZMA - its calls take 16-bit operands, which the filter does not
     * touch, and it would only cost a little. */
    if ( n >= 64 && b[0] == 'M' && b[1] == 'Z' )
    {
        UInt32 lfa = Get32( b + 0x3C );
        if ( lfa > 0 && lfa + 6 <= n )
        {
            const Byte *h = b + lfa;
            if ( h[0] == 'P' && h[1] == 'E' && h[2] == 0 && h[3] == 0 )
            {
                unsigned mach = (unsigned)h[4] | ( (unsigned)h[5] << 8 );
                if ( mach == 0x14C || mach == 0x8664 ) return CK_EXE;
            }
            else if ( h[0] == 'L' && ( h[1] == 'E' || h[1] == 'X' ) )
                return CK_EXE;
        }
    }

    /* Anything else whose bytes are as evenly spread as noise will not
     * compress, whatever it is.  A chi-square against the flat distribution:
     * about 255 for real noise, and in the thousands for anything with
     * structure.  Only meaningful over a few KB, so small files are simply
     * tried. */
    if ( n >= 4096 )
    {
        UInt32 m = n & ~255UL, s = 0, chi;
        memset( counts, 0, sizeof( counts ) );
        for ( i = 0; i < m; i++ ) counts[b[i]]++;
        for ( i = 0; i < 256; i++ ) s += counts[i] * counts[i];
        chi = s / ( m >> 8 ) - m;
        if ( chi < 1024 ) return CK_PACKED;
    }
    return CK_DATA;
}

/*===========================================================================
 * Run
 *===========================================================================*/

/* A name for the archive while it is being written: beside the target, so
 * the final rename stays on one drive. */
static void TempName( const char *target, char *dst, int size )
{
    char dir[SZ_MAX_NAME * 2];
    const char *leaf = LeafOf( target );
    int  i;

    lstrcpyn( dir, target, (int)( leaf - target ) + 1 );
    for ( i = 0; i < 1000; i++ )
    {
        char  name[16];
        FILE *t;
        sprintf( name, "XARC%04d.TMP", i );
        JoinPath( dst, size, dir, name );
        t = fopen( dst, "rb" );
        if ( !t ) return;
        fclose( t );
    }
}

/* The archive is in place: take the archive bit off each file in it whose
 * bit was set when it was found.  Each one is a directory write - DOS flushes
 * the sector on every attribute change - so a long list on an uncached disk
 * takes a while, and progress goes on being reported for it.  Cancel cannot
 * stop this part: the archive has already been made. */
static void ClearArchiveBits( ArcCompJob *j )
{
    char   path[SZ_MAX_NAME * 2];
    UInt32 k;

    for ( k = 0; k < j->nOrder; k++ )
    {
        CompEntry *e = &j->ents[j->order[k]];

        if ( e->isDir || e->state != CS_DONE || !( e->attr & 0x20 ) ) continue;
        if ( j->prog )
            j->prog( j->user, (int)k, ARC_COMP_CLEARING, CompStoredName( j, e ),
                     j->totalBytes / 1024, j->totalBytes / 1024 );
        CompSourcePath( j, e, path, sizeof( path ) );
        ClearArchiveBit( path );
    }
}

int ArcCompRun( ArcCompJob *j, const char *archivePath,
                ArcCompProgress prog, void *user )
{
    char  temp[SZ_MAX_NAME * 2];
    FILE *f;
    int   rc;

    if ( !j->scanned )
    {
        rc = ArcCompScan( j, archivePath, prog, user );
        if ( rc != SZ_OK ) return rc;
    }
    j->prog = prog;
    j->user = user;
    j->doneBytes = 0;
    j->cancelled = 0;
    j->nFiles = 0;
    j->inBytes = 0;
    j->outBytes = 0;
    j->nUnread = 0;

    TempName( j->outFull, temp, sizeof( temp ) );
    f = fopen( temp, "w+b" );
    if ( !f )
    {
        CompSetProblem( j, j->outFull );
        return SZ_ERR_WRITE;
    }

    rc = ( j->fmt == ARC_CF_7Z ) ? SzWriteArchive( j, f ) : ZipWriteArchive( j, f );
    if ( rc == SZ_OK && j->cancelled ) rc = SZ_ERR_CANCEL;

    if ( fclose( f ) != 0 && rc == SZ_OK ) rc = SZ_ERR_WRITE;
    if ( rc != SZ_OK )
    {
        remove( temp );
        return rc;
    }

    /* Only now does the old archive, if there was one, go - read-only or
     * not: the front end has already asked whether to replace it. */
    ClearFileAttr( j->outFull );
    remove( j->outFull );
    if ( rename( temp, j->outFull ) != 0 )
    {
        remove( temp );
        CompSetProblem( j, j->outFull );
        return SZ_ERR_WRITE;
    }
    if ( j->clearArc ) ClearArchiveBits( j );
    return SZ_OK;
}

/*===========================================================================
 * Results
 *===========================================================================*/
int    ArcCompFiles( ArcCompJob *j )      { return j->nFiles; }
UInt32 ArcCompInBytes( ArcCompJob *j )    { return j->inBytes; }
UInt32 ArcCompOutBytes( ArcCompJob *j )   { return j->outBytes; }
int    ArcCompDuplicates( ArcCompJob *j ) { return j->nDup; }
int    ArcCompUnreadable( ArcCompJob *j ) { return j->nUnread; }
int    ArcCompTooLong( ArcCompJob *j )    { return j->nLong; }
int    ArcCompTooBig( ArcCompJob *j )     { return j->nBig; }
int    ArcCompMissing( ArcCompJob *j )    { return j->nMissing; }
const char *ArcCompProblem( ArcCompJob *j ) { return j->problem; }
UInt32 ArcCompDictSize( ArcCompJob *j )   { return j->dictSize; }

int ArcCompSkipped( ArcCompJob *j )
{
    return j->nDup + j->nUnread + j->nLong + j->nBig;
}
