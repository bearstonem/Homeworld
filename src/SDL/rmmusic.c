/*=============================================================================
    RMMUSIC.C: Remastered-soundtrack playback. See rmmusic.h.
=============================================================================*/

#include "rmmusic.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>      /* getcwd, for the startup probe */

#include <SDL2/SDL.h>

#include "Debug.h"
#include "fqcodec.h"       /* FQ_RATE - the mixer rate the files match */
#include "soundlow.h"

/*=============================================================================
    Definitions:
=============================================================================*/

#define RM_RingBytes        (256 * 1024)    //~1.5s at 22050 stereo S16
#define RM_ReadChunk        (16 * 1024)
#define RM_FeedSleepMs      10
#define RM_PathMax          512

/*=============================================================================
    Data:
=============================================================================*/

typedef struct
{
    bool32       started;
    char         dir[RM_PathMax];

    SDL_mutex   *lock;                      //guards everything below
    SDL_Thread  *feeder;
    volatile bool32 quit;

    FILE        *fp;
    sdword       dataStart;                 //byte offset of the PCM
    sdword       dataBytes;                 //length of the PCM
    sdword       readPos;                   //how far into the PCM we have read

    ubyte       *ring;
    sdword       head;                      //write cursor (feeder)
    sdword       tail;                      //read cursor (audio callback)

    sdword       track;
    bool32       playing;
    bool32       loop;
    bool32       draining;                  //no more file, let the ring empty

    real32       vol;                       //0..1, what the mixer applies
    real32       volTarget;
    real32       volStep;                   //per mixed sample-frame

    /* The engine runs two music streams, NISSTREAM and AMBIENTSTREAM, and
       expects a cutscene cue to play over (or instead of) the level ambient.
       This player has a single output, so a cutscene cue wins and the
       ambient request that arrives underneath it is parked here and started
       when the cue ends. Without this the ambient overwrites the cue within
       a frame - which is what silenced the Mothership launch. */
    sdword       pendingTrack;
    bool32       pendingLoop;
}
rmmusicdata;

static rmmusicdata rm;

/*=============================================================================
    Private:
=============================================================================*/

static sdword rmRingUsed(void)
{
    sdword used = rm.head - rm.tail;

    if (used < 0)
    {
        used += RM_RingBytes;
    }
    return used;
}

/*-----------------------------------------------------------------------------
    Find the PCM in a RIFF/WAVE file by walking the chunks, rather than
    assuming the canonical 44-byte header - encoders are free to insert LIST
    or fact chunks, and ffmpeg sometimes does.
-----------------------------------------------------------------------------*/
static bool32 rmWavOpen(char const *path)
{
    ubyte hdr[12];
    FILE *fp = fopen(path, "rb");

    if (fp == NULL)
    {
        return FALSE;
    }
    if (fread(hdr, 1, 12, fp) != 12
        || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0)
    {
        fclose(fp);
        return FALSE;
    }

    for (;;)
    {
        ubyte  chunk[8];
        udword size;

        if (fread(chunk, 1, 8, fp) != 8)
        {
            fclose(fp);
            return FALSE;                   //ran out before finding data
        }
        size = (udword)chunk[4] | ((udword)chunk[5] << 8)
             | ((udword)chunk[6] << 16) | ((udword)chunk[7] << 24);

        if (memcmp(chunk, "data", 4) == 0)
        {
            rm.fp        = fp;
            rm.dataStart = (sdword)ftell(fp);
            rm.dataBytes = (sdword)size;
            rm.readPos   = 0;
            return TRUE;
        }
        if (fseek(fp, (long)size + (size & 1), SEEK_CUR) != 0)
        {
            fclose(fp);
            return FALSE;
        }
    }
}

static bool32 rmOpenLocked(sdword tracknum, bool32 loop, real32 fadetime);

static void rmCloseLocked(void)
{
    if (rm.fp != NULL)
    {
        fclose(rm.fp);
        rm.fp = NULL;
    }
    rm.playing  = FALSE;
    rm.draining = FALSE;
    rm.track    = -1;
    rm.head     = rm.tail = 0;
}

/*-----------------------------------------------------------------------------
    Feeder thread: keeps the ring topped up. File I/O must not happen in the
    audio callback - a blocking read there is a dropout.
-----------------------------------------------------------------------------*/
static int rmFeedThread(void *unused)
{
    (void)unused;

    while (!rm.quit)
    {
        bool32 didWork = FALSE;

        SDL_LockMutex(rm.lock);
        /* the cue ended and an ambient was parked underneath it */
        if (!rm.playing && rm.pendingTrack >= 0)
        {
            sdword want = rm.pendingTrack;

            rm.pendingTrack = -1;
            rmOpenLocked(want, rm.pendingLoop, 1.0f);
        }
        while (rm.playing && rm.fp != NULL && !rm.draining
               && rmRingUsed() < RM_RingBytes - RM_ReadChunk - 1)
        {
            sdword want = RM_ReadChunk;
            sdword left = rm.dataBytes - rm.readPos;
            sdword got;

            if (left <= 0)
            {
                if (rm.loop)
                {
                    rm.readPos = 0;
                    fseek(rm.fp, rm.dataStart, SEEK_SET);
                    left = rm.dataBytes;
                }
                else
                {
                    rm.draining = TRUE;     //play out what is already buffered
                    break;
                }
            }
            if (want > left)
            {
                want = left;
            }
            /* the ring is a circle; never read across the wrap in one go */
            if (rm.head + want > RM_RingBytes)
            {
                want = RM_RingBytes - rm.head;
            }

            got = (sdword)fread(rm.ring + rm.head, 1, (size_t)want, rm.fp);
            if (got <= 0)
            {
                rm.draining = TRUE;
                break;
            }
            rm.readPos += got;
            rm.head = (rm.head + got) % RM_RingBytes;
            didWork = TRUE;
        }
        SDL_UnlockMutex(rm.lock);

        if (!didWork)
        {
            SDL_Delay(RM_FeedSleepMs);
        }
    }
    return 0;
}

static void rmTrackPath(sdword tracknum, char *out, sdword outLen)
{
    snprintf(out, (size_t)outLen, "%s/track%02d.wav", rm.dir, (int)tracknum);
}

/*=============================================================================
    Public:
=============================================================================*/

void rmMusicStartup(char const *dir)
{
    if (rm.started)
    {
        return;
    }
    memset(&rm, 0, sizeof(rm));
    rm.track = -1;
    rm.pendingTrack = -1;

    if (dir != NULL)
    {
        strncpy(rm.dir, dir, sizeof(rm.dir) - 1);
    }

    rm.ring = (ubyte *)SDL_malloc(RM_RingBytes);
    rm.lock = SDL_CreateMutex();
    if (rm.ring == NULL || rm.lock == NULL)
    {
        dbgMessage("rmMusic: out of memory, remastered music disabled");
        return;
    }
    rm.quit   = FALSE;
    rm.feeder = SDL_CreateThread(rmFeedThread, "rmmusic", NULL);
    if (rm.feeder == NULL)
    {
        dbgMessage("rmMusic: cannot start feeder thread, remastered music disabled");
        return;
    }
    rm.started = TRUE;
    {
        /* Report where we are actually looking. The path is relative and the
           game chdir()s on Android, so a wrong working directory is the most
           likely reason an override silently never fires. */
        char cwd[RM_PathMax];
        char probe[RM_PathMax];
        FILE *t;

        if (getcwd(cwd, sizeof(cwd)) == NULL)
        {
            strcpy(cwd, "?");
        }
        rmTrackPath(10, probe, sizeof(probe));
        t = fopen(probe, "rb");
        dbgMessagef("rmMusic: ready. cwd=%s dir=%s probe=%s -> %s",
                    cwd, rm.dir, probe, (t != NULL) ? "OK" : "MISSING");
        if (t != NULL)
        {
            fclose(t);
        }
    }
}

void rmMusicShutdown(void)
{
    if (!rm.started)
    {
        return;
    }
    rm.quit = TRUE;
    SDL_WaitThread(rm.feeder, NULL);
    rm.feeder = NULL;

    SDL_LockMutex(rm.lock);
    rmCloseLocked();
    SDL_UnlockMutex(rm.lock);

    SDL_DestroyMutex(rm.lock);
    SDL_free(rm.ring);
    rm.lock = NULL;
    rm.ring = NULL;
    rm.started = FALSE;
}

bool32 rmMusicHasTrack(sdword tracknum)
{
    char path[RM_PathMax];
    FILE *fp;

    if (!rm.started || tracknum < 0)
    {
        return FALSE;
    }
    rmTrackPath(tracknum, path, sizeof(path));
    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return FALSE;
    }
    fclose(fp);
    return TRUE;
}

static bool32 rmOpenLocked(sdword tracknum, bool32 loop, real32 fadetime)
{
    char path[RM_PathMax];

    rmTrackPath(tracknum, path, sizeof(path));
    rmCloseLocked();
    if (!rmWavOpen(path))
    {
        dbgMessagef("rmMusic: cannot open %s", path);
        return FALSE;
    }
    rm.track     = tracknum;
    rm.loop      = loop;
    rm.playing   = TRUE;
    rm.volTarget = 1.0f;
    if (fadetime > 0.0f)
    {
        rm.vol     = 0.0f;
        rm.volStep = 1.0f / (fadetime * (real32)FQ_RATE);
    }
    else
    {
        rm.vol     = 1.0f;
        rm.volStep = 0.0f;
    }
    dbgMessagef("rmMusic: track %d%s", (int)tracknum, loop ? " (looping)" : "");
    return TRUE;
}

void rmMusicPlay(sdword tracknum, bool32 loop, real32 fadetime)
{
    if (!rm.started)
    {
        return;
    }
    SDL_LockMutex(rm.lock);

    /* Re-requesting the track already playing must not restart it. The game
       asks for the current ambient track every frame - the original absorbs
       that via "if (!soundstreamover(handle))". Without this the file seeks
       back to the start ~70 times a second and never plays more than one
       frame, which sounds like silence. */
    if (rm.playing && rm.track == tracknum)
    {
        SDL_UnlockMutex(rm.lock);
        return;
    }

    /* A looping request is level ambient; a one-shot is a cutscene cue. The
       cue outranks the ambient, so park the ambient rather than cutting the
       cue off after a single frame. */
    if (rm.playing && !rm.loop && loop)
    {
        rm.pendingTrack = tracknum;
        rm.pendingLoop  = TRUE;
        SDL_UnlockMutex(rm.lock);
        return;
    }
    if (loop)
    {
        rm.pendingTrack = -1;               //this ambient supersedes any parked one
    }

    rmOpenLocked(tracknum, loop, fadetime);
    SDL_UnlockMutex(rm.lock);
}

void rmMusicStop(real32 fadetime)
{
    if (!rm.started)
    {
        return;
    }
    SDL_LockMutex(rm.lock);
    if (rm.playing)
    {
        rm.volTarget = 0.0f;
        rm.volStep   = (fadetime > 0.0f)
                     ? (rm.vol / (fadetime * (real32)FQ_RATE))
                     : rm.vol;              //immediate
        if (rm.volStep <= 0.0f)
        {
            rmCloseLocked();
        }
    }
    SDL_UnlockMutex(rm.lock);
}

void rmMusicSetVolume(sdword vol)
{
    if (!rm.started)
    {
        return;
    }
    if (vol < 0)
    {
        vol = 0;
    }
    if (vol > SOUND_VOL_MAX)
    {
        vol = SOUND_VOL_MAX;
    }
    SDL_LockMutex(rm.lock);
    /* only chase the level; a stop in progress owns volTarget */
    if (rm.playing && rm.volTarget > 0.0f)
    {
        rm.volTarget = (real32)vol / (real32)SOUND_VOL_MAX;
        rm.volStep   = 0.0f;                //snap: the caller is ramping already
        rm.vol       = rm.volTarget;
    }
    SDL_UnlockMutex(rm.lock);
}

bool32 rmMusicPlaying(void)
{
    return rm.started && rm.playing;
}

sdword rmMusicCurrentTrack(void)
{
    return rm.started ? rm.track : -1;
}

void rmMusicMix(void *stream, sdword len)
{
    sword  *out    = (sword *)stream;
    sdword  frames = len / (sdword)(2 * sizeof(sword));   //stereo S16
    sdword  i;

    if (!rm.started)
    {
        return;
    }
    SDL_LockMutex(rm.lock);
    if (!rm.playing)
    {
        SDL_UnlockMutex(rm.lock);
        return;
    }

    /* Only report trouble. On device the ring sits around 95% full, so a
       starved ring means the feeder is not keeping up and the music will
       stutter - worth knowing about, unlike the steady state. */
    if (rmRingUsed() < (frames * 4) && !rm.draining)
    {
        static sdword complaints = 0;

        if (complaints < 8)
        {
            complaints++;
            dbgMessagef("rmMusic: ring starved (%d bytes for %d frames), track %d",
                        (int)rmRingUsed(), (int)frames, (int)rm.track);
        }
    }

    for (i = 0; i < frames; i++)
    {
        sword  src[2];
        sdword ch;

        if (rmRingUsed() < (sdword)sizeof(src))
        {
            if (rm.draining)
            {
                rmCloseLocked();            //track finished
            }
            break;                          //underrun: leave the rest as-is
        }

        /* pull one stereo frame out of the ring */
        for (ch = 0; ch < 2; ch++)
        {
            ubyte lo = rm.ring[rm.tail];
            ubyte hi = rm.ring[(rm.tail + 1) % RM_RingBytes];

            src[ch] = (sword)((uword)lo | ((uword)hi << 8));
            rm.tail = (rm.tail + 2) % RM_RingBytes;
        }

        /* volume ramp, one step per frame */
        if (rm.volStep != 0.0f)
        {
            if (rm.vol < rm.volTarget)
            {
                rm.vol += rm.volStep;
                if (rm.vol >= rm.volTarget)
                {
                    rm.vol = rm.volTarget;
                    rm.volStep = 0.0f;
                }
            }
            else if (rm.vol > rm.volTarget)
            {
                rm.vol -= rm.volStep;
                if (rm.vol <= rm.volTarget)
                {
                    rm.vol = rm.volTarget;
                    rm.volStep = 0.0f;
                    if (rm.volTarget == 0.0f)
                    {
                        rmCloseLocked();    //faded out
                        break;
                    }
                }
            }
        }

        for (ch = 0; ch < 2; ch++)
        {
            sdword mixed = (sdword)out[i * 2 + ch]
                         + (sdword)((real32)src[ch] * rm.vol);

            if (mixed > 32767)
            {
                mixed = 32767;
            }
            else if (mixed < -32768)
            {
                mixed = -32768;
            }
            out[i * 2 + ch] = (sword)mixed;
        }
    }
    SDL_UnlockMutex(rm.lock);
}
