/* UNZIP.CPP - PKZIP 2.04g compatible extractor, Open Watcom 1.9, OS/2
 *
 * Supports: stored (method 0), deflated (method 8).
 * Encrypted entries are skipped / reported as UZ_ERR_METHOD.
 * No console output whatsoever.
 *
 * The OS/2 copy of the Windows installers' extractor.  The byte-moving is
 * plain C stdio and is unchanged; the one platform-specific piece is the
 * timestamp restore, which goes through DosSetPathInfo.  OS/2's FDATE and
 * FTIME are bit-fields with exactly the MS-DOS packing a ZIP entry carries
 * (day:5 month:4 year:7 / twosecs:5 minutes:6 hours:5), so the stamp drops
 * straight in.
 */

#define INCL_DOSFILEMGR
#define INCL_DOSERRORS
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>

#include "unzip.hpp"

/* ---- ZIP structure constants -------------------------------------------- */

#define SIG_LOCAL   0x04034B50UL   /* local file header */
#define SIG_DATA    0x08074B50UL   /* data descriptor */
#define SIG_CENT    0x02014B50UL   /* central directory entry */
#define SIG_END     0x06054B50UL   /* end of central directory */

#define METHOD_STORE  0
#define METHOD_DEFLATE 8

#define FLAG_DATADESC  0x0008      /* sizes in data descriptor */
#define FLAG_ENCRYPTED 0x0001

#pragma pack(1)
typedef struct {
    unsigned long  sig;
    unsigned short verNeeded;
    unsigned short flags;
    unsigned short method;
    unsigned short modTime;
    unsigned short modDate;
    unsigned long  crc32;
    unsigned long  compSize;
    unsigned long  uncompSize;
    unsigned short fnLen;
    unsigned short extraLen;
} LocalHdr;

typedef struct {
    unsigned long  sig;
    unsigned short verMade;
    unsigned short verNeeded;
    unsigned short flags;
    unsigned short method;
    unsigned short modTime;
    unsigned short modDate;
    unsigned long  crc32;
    unsigned long  compSize;
    unsigned long  uncompSize;
    unsigned short fnLen;
    unsigned short extraLen;
    unsigned short commentLen;
    unsigned short diskStart;
    unsigned short intAttr;
    unsigned long  extAttr;
    long           localOffset;
} CentralHdr;

typedef struct {
    unsigned long  sig;
    unsigned short diskNum;
    unsigned short diskStart;
    unsigned short entriesThis;
    unsigned short entriesTotal;
    unsigned long  dirSize;
    unsigned long  dirOffset;
    unsigned short commentLen;
} EndRec;
#pragma pack()

/* ---- CRC-32 table -------------------------------------------------------- */

static unsigned long crcTable[256];
static int crcInited = 0;

static void BuildCrcTable(void)
{
    unsigned long c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
        crcTable[n] = c;
    }
    crcInited = 1;
}

static unsigned long UpdateCrc(unsigned long crc,
                                const unsigned char *buf,
                                unsigned long len)
{
    if (!crcInited) BuildCrcTable();
    crc ^= 0xFFFFFFFFUL;
    while (len--) {
        crc = crcTable[(unsigned char)(crc ^ *buf++)] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* ---- Deflate / inflate -------------------------------------------------- */

/* Sliding window inflate (RFC 1951).
 * Written to avoid large stack frames.  The 32K window lives on the heap.
 */

#define WSIZE        32768U
#define WSIZE_MASK   (WSIZE - 1)

/* Compressed-bit reader state */
typedef struct {
    FILE         *fp;
    unsigned long bitsLeft;   /* bits remaining in window */
    unsigned long bitBuf;     /* accumulated bits */
    unsigned long bytesLeft;  /* compressed bytes still to read */
    int           eof;
} BitReader;

static int BrInit(BitReader *br, FILE *fp, unsigned long compSize)
{
    br->fp        = fp;
    br->bitsLeft  = 0;
    br->bitBuf    = 0;
    br->bytesLeft = compSize;
    br->eof       = 0;
    return 0;
}

static int BrFill(BitReader *br)
{
    int c;
    if (br->bytesLeft == 0) { br->eof = 1; return -1; }
    c = fgetc(br->fp);
    if (c == EOF) { br->eof = 1; return -1; }
    br->bytesLeft--;
    br->bitBuf |= ((unsigned long)(unsigned char)c) << br->bitsLeft;
    br->bitsLeft += 8;
    return 0;
}

static unsigned long BrBits(BitReader *br, unsigned n)
{
    while (br->bitsLeft < n) {
        if (BrFill(br)) return 0;
    }
    return br->bitBuf & ((1UL << n) - 1UL);
}

static void BrConsume(BitReader *br, unsigned n)
{
    br->bitBuf  >>= n;
    br->bitsLeft -= n;
}

static unsigned long BrRead(BitReader *br, unsigned n)
{
    unsigned long v = BrBits(br, n);
    BrConsume(br, n);
    return v;
}

/* Canonical Huffman decoder tables (max code length 15) */
#define MAX_BITS   15
#define MAX_CODES  288

typedef struct {
    short  vals[MAX_CODES];              /* symbols sorted by canonical code */
    unsigned short offsets[MAX_BITS + 2]; /* base index into vals[] per length */
    unsigned short maxlen;
} HuffTree;

static int BuildHuff(HuffTree *ht, const unsigned char *lens, int n)
{
    int i, len;
    unsigned short cnt[MAX_BITS + 1];
    unsigned short nxt[MAX_BITS + 1];

    memset(cnt, 0, sizeof(cnt));
    for (i = 0; i < n; i++) {
        if (lens[i]) cnt[(int)lens[i]]++;
    }

    ht->maxlen = 0;
    for (len = 1; len <= MAX_BITS; len++) {
        if (cnt[len] && (unsigned)len > ht->maxlen) {
            ht->maxlen = (unsigned short)len;
        }
    }
    if (ht->maxlen == 0) return 0;

    ht->offsets[0] = 0;
    for (len = 1; len <= MAX_BITS + 1; len++) {
        ht->offsets[len] = ht->offsets[len-1] + cnt[len-1];
    }

    memcpy(nxt, ht->offsets, sizeof(nxt));
    for (i = 0; i < n; i++) {
        if (lens[i]) {
            ht->vals[nxt[(int)lens[i]]++] = (short)i;
        }
    }

    return 0;
}

static int HuffDecode(HuffTree *ht, BitReader *br)
{
    /* Deflate codes are MSB-first within each code word, but the code words
     * are packed LSB-first in the bitstream.  We read one bit at a time,
     * shifting each new bit to the MSB side, then check against the
     * canonical table.  'base' is maintained incrementally. */
    unsigned long code, base;
    unsigned int len, count;

    code = 0; base = 0;
    for (len = 1; len <= (unsigned int)ht->maxlen; len++) {
        code = (code << 1) | (unsigned long)BrRead(br, 1);
        if (br->eof) return -1;
        count = ht->offsets[len+1] - ht->offsets[len];
        if (count && code >= base && code < base + count) {
            return ht->vals[ht->offsets[len] + (unsigned int)(code - base)];
        }
        base = (base + count) << 1;
    }
    return -1;
}

/* Static Huffman lengths (RFC 1951 3.2.6) */
static void StaticLens(unsigned char *llit, unsigned char *ldist)
{
    int i;
    for (i = 0;   i <= 143; i++) llit[i] = 8;
    for (i = 144; i <= 255; i++) llit[i] = 9;
    for (i = 256; i <= 279; i++) llit[i] = 7;
    for (i = 280; i <= 287; i++) llit[i] = 8;
    for (i = 0;   i <= 31;  i++) ldist[i] = 5;
}

/* Length / distance decode tables (RFC 1951) */
static const unsigned short lenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned char lenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short distBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};
static const unsigned char distExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* ---- Buffered output sink ----------------------------------------------- */

/* Staging the decompressed bytes here (rather than fwrite()-ing one at a time)
 * collapses the per-byte fwrite and CRC overhead into per-block work, and
 * gives the progress/yield callback a natural firing point. */
#define OBUF_SIZE 8192U

typedef struct {
    FILE          *out;     /* destination file               */
    unsigned char *buf;     /* OBUF_SIZE staging buffer        */
    unsigned int   len;     /* bytes currently staged          */
    unsigned long  crc;     /* running CRC-32                   */
    unsigned long  done;    /* bytes written to the file       */
    unsigned long  total;   /* expected uncompressed size      */
    UZ_PROGRESS    prog;    /* optional callback (may be NULL)  */
    void          *user;    /* callback context                */
    int            rc;      /* UZ_OK / UZ_ERR_WRITE / UZ_ERR_CANCEL */
} OutBuf;

/* Flush staged bytes (updating CRC) and fire the progress callback.
 * Returns -1 (and sets o->rc) on write error or user cancel. */
static int OutFlush(OutBuf *o)
{
    if (o->len) {
        o->crc = UpdateCrc(o->crc, o->buf, (unsigned long)o->len);
        if (fwrite(o->buf, 1, o->len, o->out) != o->len) {
            o->rc = UZ_ERR_WRITE;
            return -1;
        }
        o->done += o->len;
        o->len = 0;
    }
    if (o->prog) {
        if (!o->prog(o->done, o->total, o->user)) {
            o->rc = UZ_ERR_CANCEL;
            return -1;
        }
    }
    return 0;
}

/* Append one decompressed byte, flushing when the buffer fills. */
static int OutByte(OutBuf *o, unsigned char b)
{
    o->buf[o->len++] = b;
    if (o->len >= OBUF_SIZE)
        return OutFlush(o);
    return 0;
}

/* Inflate one compressed block.
 * window - 32K sliding window buffer (caller provides, WSIZE bytes)
 * wpos   - current write position in window
 * o      - buffered output sink (also carries CRC and progress)
 */
static int InflateBlock(BitReader *br, OutBuf *o,
                        unsigned char *window, unsigned int *wpos,
                        HuffTree *hl, HuffTree *hd)
{
    int sym, li, di, len;
    unsigned int dist, back;
    unsigned char b;

    for (;;) {
        sym = HuffDecode(hl, br);
        if (sym < 0) return UZ_ERR_FORMAT;
        if (sym == 256) break;
        if (sym < 256) {
            b = (unsigned char)sym;
            window[*wpos] = b;
            *wpos = (*wpos + 1) & WSIZE_MASK;
            if (OutByte(o, b)) return o->rc;
        } else {
            li = sym - 257;
            if (li < 0 || li >= 29) return UZ_ERR_FORMAT;
            len = lenBase[li] + (int)BrRead(br, lenExtra[li]);
            if (br->eof) return UZ_ERR_FORMAT;
            di = HuffDecode(hd, br);
            if (br->eof || di < 0 || di >= 30) return UZ_ERR_FORMAT;
            dist = distBase[di] + (unsigned int)BrRead(br, distExtra[di]);
            if (br->eof) return UZ_ERR_FORMAT;
            while (len-- > 0) {
                back = (*wpos + WSIZE - dist) & WSIZE_MASK;
                b = window[back];
                window[*wpos] = b;
                *wpos = (*wpos + 1) & WSIZE_MASK;
                if (OutByte(o, b)) return o->rc;
            }
        }
    }
    return UZ_OK;
}

/* clOrder: RFC 1951 code-length alphabet order */
static const int clOrder[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* Full inflate: handle stored/fixed/dynamic blocks */
static int Inflate(FILE *in, FILE *out,
                   unsigned long compSize, unsigned long uncompSize,
                   unsigned long *crcOut,
                   UZ_PROGRESS prog, void *user)
{
    unsigned char *window;
    unsigned char llit[288];
    unsigned char ldist[32];
    unsigned char clLens[19];
    unsigned char allLens[288 + 32];
    HuffTree *hl;
    HuffTree *hd;
    HuffTree *hcl;
    BitReader br;
    OutBuf o;
    unsigned int wpos;
    int rc, bfinal, btype;
    int hlit, hdist, hclen, ci, total, idx, s, rep;
    unsigned int blen, bnlen;
    unsigned char stbyte;
    unsigned int excess;

    window = NULL; hl = NULL; hd = NULL; hcl = NULL;
    wpos = 0; rc = UZ_OK;
    o.buf = NULL;

    window = (unsigned char *)malloc(WSIZE);
    hl     = (HuffTree *)malloc(sizeof(HuffTree));
    hd     = (HuffTree *)malloc(sizeof(HuffTree));
    o.buf  = (unsigned char *)malloc(OBUF_SIZE);

    if (!window || !hl || !hd || !o.buf) { rc = UZ_ERR_MEMORY; goto done; }

    /* Initialise the buffered output sink */
    o.out   = out;
    o.len   = 0;
    o.crc   = 0;
    o.done  = 0;
    o.total = uncompSize;
    o.prog  = prog;
    o.user  = user;
    o.rc    = UZ_OK;

    BrInit(&br, in, compSize);

    for (;;) {
        bfinal = (int)BrRead(&br, 1);
        btype  = (int)BrRead(&br, 2);

        if (btype == 0) {
            /* Stored block: align to byte boundary, read LEN/NLEN, then copy.
             * All reads go through BrRead so the bitbuffer stays consistent. */
            excess = (unsigned int)(br.bitsLeft & 7U);
            if (excess) BrConsume(&br, excess);
            blen  = (unsigned int)BrRead(&br, 16);
            bnlen = (unsigned int)BrRead(&br, 16);
            (void)bnlen;
            while (blen-- > 0) {
                stbyte = (unsigned char)BrRead(&br, 8);
                if (br.eof) { rc = UZ_ERR_READ; goto done; }
                window[wpos] = stbyte;
                wpos = (wpos + 1) & WSIZE_MASK;
                if (OutByte(&o, stbyte)) { rc = o.rc; goto done; }
            }
        } else if (btype == 1) {
            /* fixed / static Huffman */
            StaticLens(llit, ldist);
            BuildHuff(hl, llit, 288);
            BuildHuff(hd, ldist, 32);
            rc = InflateBlock(&br, &o, window, &wpos, hl, hd);
            if (rc) goto done;
        } else if (btype == 2) {
            /* dynamic Huffman */
            hlit  = (int)BrRead(&br, 5) + 257;
            hdist = (int)BrRead(&br, 5) + 1;
            hclen = (int)BrRead(&br, 4) + 4;

            memset(clLens, 0, sizeof(clLens));
            for (ci = 0; ci < hclen; ci++) {
                clLens[clOrder[ci]] = (unsigned char)BrRead(&br, 3);
            }

            hcl = (HuffTree *)malloc(sizeof(HuffTree));
            if (!hcl) { rc = UZ_ERR_MEMORY; goto done; }
            BuildHuff(hcl, clLens, 19);

            total = hlit + hdist;
            idx   = 0;
            while (idx < total) {
                s = HuffDecode(hcl, &br);
                if (s < 0) { rc = UZ_ERR_FORMAT; goto done; }
                if (s < 16) {
                    allLens[idx++] = (unsigned char)s;
                } else if (s == 16) {
                    rep = (int)BrRead(&br, 2) + 3;
                    stbyte = idx ? allLens[idx-1] : 0;
                    while (rep-- && idx < total) allLens[idx++] = stbyte;
                } else if (s == 17) {
                    rep = (int)BrRead(&br, 3) + 3;
                    while (rep-- && idx < total) allLens[idx++] = 0;
                } else {
                    rep = (int)BrRead(&br, 7) + 11;
                    while (rep-- && idx < total) allLens[idx++] = 0;
                }
            }
            BuildHuff(hl, allLens,       hlit);
            BuildHuff(hd, allLens + hlit, hdist);
            free(hcl); hcl = NULL;
            rc = InflateBlock(&br, &o, window, &wpos, hl, hd);
            if (rc) goto done;
        } else {
            rc = UZ_ERR_FORMAT; goto done;
        }

        if (bfinal) break;
    }

    /* Flush whatever is left in the staging buffer, then publish the CRC */
    if (OutFlush(&o)) { rc = o.rc; goto done; }
    *crcOut = o.crc;

done:
    if (window)  free(window);
    if (hl)      free(hl);
    if (hd)      free(hd);
    if (hcl)     free(hcl);
    if (o.buf)   free(o.buf);
    return rc;
}

/* ---- Directory utilities ------------------------------------------------ */

/* Create all directories in path (forward and back slashes) */
static void MakePath(const char *path)
{
    char buf[256];
    char *p;
    char c;
    int i;

    for (i = 0; i < 255 && path[i]; i++) {
        buf[i] = path[i];
    }
    buf[i] = '\0';

    for (p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            c = *p;
            *p = '\0';
            _mkdir(buf);
            *p = c;
        }
    }
}

/* ---- Basename matching -------------------------------------------------- */

/* Compare just the filename component of stored path against target */
static int MatchName(const char *stored, const char *target)
{
    const char *p = stored;
    const char *last = stored;
    while (*p) {
        if (*p == '/' || *p == '\\') last = p + 1;
        p++;
    }
    /* case-insensitive: OS/2 file names are */
    return (stricmp(last, target) == 0);
}

/* ---- Restore archived modification time --------------------------------- */

/* ZIP stores modification date/time in MS-DOS packed format (local time),
 * which is also OS/2's FDATE/FTIME packing.  Stamp the freshly written file
 * with it.  The rest of the FILESTATUS3 has to be valid, so it is read back
 * first and only the two last-write fields are altered. */
static void RestoreFileTime(const char *path,
                            unsigned short dosDate, unsigned short dosTime)
{
    FILESTATUS3 fs;

    if (dosDate == 0 && dosTime == 0)
        return;
    if (DosQueryPathInfo((PCSZ)path, FIL_STANDARD, &fs, sizeof(fs)) != NO_ERROR)
        return;

    memcpy(&fs.fdateLastWrite, &dosDate, sizeof(FDATE));
    memcpy(&fs.ftimeLastWrite, &dosTime, sizeof(FTIME));

    DosSetPathInfo((PCSZ)path, FIL_STANDARD, &fs, sizeof(fs), 0);
}

/* ---- Find end-of-central-directory record ------------------------------ */

static int FindEndRec(FILE *fp, EndRec *er, long *eocdPos)
{
    /* Scan backwards from end of file for the EOCD signature.
     * Max comment length is 65535, so search at most 65535+22 bytes back.
     * Returns the file offset of the EOCD record in *eocdPos so the caller
     * can derive the stub bias for self-extracting archives. */
    long fileLen, pos;
    unsigned char buf[4];

    fseek(fp, 0L, SEEK_END);
    fileLen = ftell(fp);
    if (fileLen < 22L) return UZ_ERR_FORMAT;

    /* Try near the end first (common case: no comment) */
    pos = fileLen - 22L;
    for (; pos >= 0L && (fileLen - pos) < 65557L; pos--) {
        fseek(fp, pos, SEEK_SET);
        if (fread(buf, 1, 4, fp) != 4) continue;
        if (buf[0]=='P' && buf[1]=='K' && buf[2]==5 && buf[3]==6) {
            fseek(fp, pos, SEEK_SET);
            if (fread(er, sizeof(EndRec), 1, fp) == 1 &&
                er->sig == SIG_END) {
                /* verify: comment follows immediately after fixed part */
                if (pos + (long)sizeof(EndRec) + er->commentLen == fileLen) {
                    *eocdPos = pos;
                    return UZ_OK;
                }
            }
        }
    }
    return UZ_ERR_FORMAT;
}

/* ---- Extract one entry -------------------------------------------------- */

static int ExtractOne(FILE *zfp, CentralHdr *ch, long bias,
                      const char *destDir,
                      UZ_PROGRESS prog, void *user)
{
    LocalHdr lh;
    char fname[256];
    char rel[256];
    char outPath[256];
    char *op;
    unsigned char buf[512];
    unsigned long crc, remain, storedCrc, wrote;
    unsigned long compSize, uncompSize;
    unsigned int fnLen, toRead;
    int rc, last, fi, n;
    FILE *out;

    rc = UZ_OK; out = NULL;

    if (fseek(zfp, ch->localOffset + bias, SEEK_SET) != 0) return UZ_ERR_READ;
    if (fread(&lh, sizeof(LocalHdr), 1, zfp) != 1)   return UZ_ERR_READ;
    if (lh.sig != SIG_LOCAL)                           return UZ_ERR_FORMAT;

    fnLen = (lh.fnLen < 255) ? lh.fnLen : 255;
    if (fread(fname, 1, fnLen, zfp) != fnLen) return UZ_ERR_READ;
    fname[fnLen] = '\0';
    if (lh.extraLen) fseek(zfp, (long)lh.extraLen, SEEK_CUR);

    if (lh.flags & FLAG_ENCRYPTED)                          return UZ_ERR_METHOD;
    if (lh.method != METHOD_STORE && lh.method != METHOD_DEFLATE) return UZ_ERR_METHOD;

    /* Convert forward slashes to build the archive-relative path */
    for (fi = 0; fname[fi]; fi++) {
        rel[fi] = (fname[fi] == '/') ? '\\' : fname[fi];
    }
    rel[fi] = '\0';

    op = rel;
    while (*op == '\\') op++;   /* strip leading separator */

    /* Prepend the destination directory, if any, ensuring one separator */
    n = 0;
    if (destDir && destDir[0]) {
        while (destDir[n] && n < 240) { outPath[n] = destDir[n]; n++; }
        if (n > 0 && outPath[n-1] != '\\' && outPath[n-1] != '/')
            outPath[n++] = '\\';
    }
    for (fi = 0; op[fi] && n < 255; fi++) outPath[n++] = op[fi];
    outPath[n] = '\0';

    MakePath(outPath);

    last = (int)strlen(outPath) - 1;
    if (last >= 0 && outPath[last] == '\\') return UZ_OK;  /* directory entry */

    out = fopen(outPath, "wb");
    if (!out) return UZ_ERR_CREATE;

    crc = 0;
    if (lh.method == METHOD_STORE) {
        remain = ch->uncompSize;
        wrote  = 0;
        while (remain > 0) {
            toRead = (remain > 512UL) ? 512U : (unsigned int)remain;
            if (fread(buf, 1, toRead, zfp) != toRead) { rc = UZ_ERR_READ; goto done; }
            crc = UpdateCrc(crc, buf, toRead);
            if (fwrite(buf, 1, toRead, out) != toRead) { rc = UZ_ERR_WRITE; goto done; }
            remain -= toRead;
            wrote  += toRead;
            if (prog && !prog(wrote, ch->uncompSize, user)) { rc = UZ_ERR_CANCEL; goto done; }
        }
    } else {
        compSize   = ch->compSize;
        uncompSize = ch->uncompSize;
        rc = Inflate(zfp, out, compSize, uncompSize, &crc, prog, user);
    }

done:
    fclose(out);
    if (rc == UZ_OK) {
        storedCrc = (lh.flags & FLAG_DATADESC) ? ch->crc32 : lh.crc32;
        if (crc != storedCrc) rc = UZ_ERR_CRC;
    }
    if (rc == UZ_OK) {
        /* Restore the archived modification date/time.  Done after the
         * close so the close cannot overwrite the stamp. */
        RestoreFileTime(outPath, lh.modDate, lh.modTime);
    } else {
        /* On cancel, CRC failure, or any write error, do not leave a
         * partially-written (and therefore corrupt) file behind. */
        remove(outPath);
    }
    return rc;
}

/* ---- Public API --------------------------------------------------------- */

int ExtractZip(const char *pszZip, const char *pszTarget,
               const char *pszDestDir,
               UZ_PROGRESS pfnProgress, void *pUser)
{
    FILE *zfp;
    EndRec er;
    CentralHdr *cent;
    char fname[256];
    unsigned int fnLen;
    long resumePos, eocdPos, bias;
    unsigned long i;
    int rc, found;

    rc = UZ_OK; found = 0; cent = NULL;

    if (!pszZip) return UZ_ERR_OPEN;
    zfp = fopen(pszZip, "rb");
    if (!zfp) return UZ_ERR_OPEN;

    rc = FindEndRec(zfp, &er, &eocdPos);
    if (rc) { fclose(zfp); return rc; }

    if (er.diskNum != 0 || er.diskStart != 0) {
        fclose(zfp); return UZ_ERR_DISK;
    }

    /* For a self-extracting EXE the archive is preceded by a stub, so the
     * offsets stored in the EOCD and central directory are short by the stub
     * size.  The central directory physically ends where the EOCD begins, so
     * its true start is (eocdPos - dirSize); the difference from the stored
     * dirOffset is the bias to add to every stored offset.  Plain ZIPs yield
     * a bias of 0. */
    bias = (eocdPos - (long)er.dirSize) - (long)er.dirOffset;

    cent = (CentralHdr *)malloc(sizeof(CentralHdr));
    if (!cent) { fclose(zfp); return UZ_ERR_MEMORY; }

    if (fseek(zfp, (long)er.dirOffset + bias, SEEK_SET) != 0) {
        rc = UZ_ERR_FORMAT; goto cleanup;
    }

    for (i = 0; i < er.entriesTotal; i++) {
        if (fread(cent, sizeof(CentralHdr), 1, zfp) != 1) {
            rc = UZ_ERR_READ; goto cleanup;
        }
        if (cent->sig != SIG_CENT) { rc = UZ_ERR_FORMAT; goto cleanup; }

        fnLen = (cent->fnLen < 255) ? cent->fnLen : 255;
        if (fread(fname, 1, fnLen, zfp) != fnLen) { rc = UZ_ERR_READ; goto cleanup; }
        fname[fnLen] = '\0';
        resumePos = ftell(zfp) + (long)cent->extraLen + (long)cent->commentLen;

        if (!pszTarget || MatchName(fname, pszTarget)) {
            found = 1;
            rc = ExtractOne(zfp, cent, bias, pszDestDir, pfnProgress, pUser);
            if (rc) goto cleanup;
            if (pszTarget) goto cleanup;
            fseek(zfp, resumePos, SEEK_SET);
        } else {
            fseek(zfp, (long)cent->extraLen + (long)cent->commentLen, SEEK_CUR);
        }
    }

    if (pszTarget && !found) rc = UZ_ERR_NOFILE;

cleanup:
    if (cent) free(cent);
    fclose(zfp);
    return rc;
}
