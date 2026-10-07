#pragma once
/*
 * Intro videos (popcap_logo.mp4, then optionally soe_logo.mp4) played with SceAvPlayer before libmain.so is touched.
 *
 *   intro_video_play()     blocks until the video has played to the end
 *                          (returns 0), or returns <0 if it could not be played
 *                          (missing/unreadable file etc.) so the loader carries on.
 *   intro_video_release()  frees the display buffers. intro_video_play() already calls this, so
 *                          no CDRAM is held when vitaGL starts; calling it again is harmless.
 */
#define INTRO_VIDEO_PATH      "app0:USRDIR/movies/popcap_logo.mp4"   /* bundled in the VPK */
#define INTRO_VIDEO_PATH_ALT  "ux0:data/insaniquarium/movies/popcap_logo.mp4"
/* Second video, played right after the first. Optional: skipped if the file is not there. */
#define INTRO_VIDEO2_PATH     "app0:USRDIR/movies/soe_logo.mp4"
#define INTRO_VIDEO2_PATH_ALT "ux0:data/insaniquarium/movies/soe_logo.mp4"

int  intro_video_play(void);
void intro_video_release(void);

extern int g_intro_bgm;      /* 1 = intro audio on the BGM port (loader.cfg intro_bgm=) */
extern int g_intro_audio_buffer; /* ms of audio allowed to queue ahead of the speaker, loader.cfg intro_audio_buffer= (100-700) */
extern int g_intro_volume;   /* 0-100, set from loader.cfg (intro_volume=) */
