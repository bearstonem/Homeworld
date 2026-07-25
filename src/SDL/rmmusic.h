/*=============================================================================
    RMMUSIC.H: Remastered-soundtrack playback.

    Plays the Homeworld Remastered music in place of the original .wxd
    streams. The files are produced by tools/rm_music.py as trackNN.wav,
    already at the mixer's rate (FQ_RATE, 22050 Hz stereo S16), so playback is
    a straight PCM read with no resampling.

    This deliberately does not go through the FQ stream machinery. Those
    streams carry compressed data with per-stream bitrates and a checksummed
    lookup table generated alongside the .wxd; feeding plain PCM through them
    would mean re-encoding. Mixing a second PCM source into the audio callback
    is far less invasive, and leaves every existing sound effect and speech
    path untouched.
=============================================================================*/

#ifndef ___RMMUSIC_H
#define ___RMMUSIC_H

#include "Types.h"

/* Started once the sound system is up. dir is where trackNN.wav live (the
   game's own data directory). Safe to call when the files are absent - every
   other entry point then reports "no track" and the original music plays. */
void rmMusicStartup(char const *dir);
void rmMusicShutdown(void);

/* TRUE if a remastered file exists for this HW1 track number, i.e. whether
   the caller should override. */
bool32 rmMusicHasTrack(sdword tracknum);

/* fadetime is in seconds, 0 for immediate. */
void rmMusicPlay(sdword tracknum, bool32 loop, real32 fadetime);
void rmMusicStop(real32 fadetime);

/* 0..255, matching the engine's SOUND_VOL_MAX scale. */
void rmMusicSetVolume(sdword vol);

bool32 rmMusicPlaying(void);
sdword rmMusicCurrentTrack(void);

/* Called from the SDL audio callback with the already-mixed game audio;
   adds the music on top. len is in bytes, S16 stereo. */
void rmMusicMix(void *stream, sdword len);

#endif
