/**
 * Shared FMV player: Media Foundation decode onto a D3D8 texture.
 *
 * For a title whose video is a container Windows can already decode, the
 * decoder does not have to be emulated for the video to be watchable. The
 * recompiled title still decides when: it opens the file as its own logic
 * dictates, and the runtime plays the file it opened.
 */

#ifndef XBOXRECOMP_VIDEO_PLAYER_H
#define XBOXRECOMP_VIDEO_PLAYER_H

#include <stdint.h>

/* Initialize/shutdown Media Foundation (call once at app start/end) */
int  video_init(void);
void video_shutdown(void);

/* Open a video file for playback. Returns 0 on success. */
int  video_open(const char *path);

/* Advance playback by dt seconds, decode next frame if needed.
 * Returns 1 if a new frame is ready, 0 if unchanged, -1 if finished. */
int  video_update(float dt);

/* Render the current video frame as a fullscreen quad.
 * Call between BeginScene/EndScene. */
void video_render(void);

/* Check if the video has finished playing. */
int  video_is_finished(void);

/* Close the current video and release resources. */
void video_close(void);

/* === Boot sequence state machine === */

/* Boot phases */

/* Get current boot phase */

/* Advance boot state machine. Call once per frame.
 * skip=1 if user pressed a button to skip current video.
 * Returns the new phase. */

/* Render current boot phase (video frame or press-start screen). */


/* Play one video file, start to finish, in its own window on its own thread.
 * Returns 0 if the pump started. Blocking work happens on that thread, so the
 * guest carries on running while the video plays. */
int  xbox_VideoPlayFile(const char *host_path);
int  xbox_VideoIsPlaying(void);

/* Write the current decoded frame to a 24-bit BMP -- evidence that real
 * pixels reached the renderer, which a log line cannot give. */
int  video_dump_frame_bmp(const char *path);

/* Show the guest framebuffer in its own window (RECOMP_FB_WINDOW). Whatever
 * the title renders into guest RAM appears there; nothing else scans it out. */
/* Non-zero while that virtual key is held in the framebuffer window. Always
 * zero when there is no window, which is the right answer: with nothing to
 * focus there is nothing to type into. */
int xbox_FramebufferKeyDown(int vk);

void xbox_FramebufferWindowStart(void);
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
int  xbox_FramebufferDumpBmp(const char *path);

#endif /* BURNOUT3_VIDEO_PLAYER_H */
