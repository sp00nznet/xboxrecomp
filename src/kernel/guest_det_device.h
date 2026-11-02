/*
 * guest_det_device.h - the device side of the deterministic guest scheduler
 * (kernel_guest_det.c; the guest side is in kernel.h).
 *
 * A device thread that writes guest memory or raises interrupts (the GPU
 * walker, the timer thread, the APU frame thread) runs a pass only when the
 * scheduler grants one, while every guest thread is parked, so what it
 * writes lands at the same point of guest progress on every run. Kept apart
 * from kernel.h so the device models (src/apu) can use it without the
 * kernel's types.
 */
#ifndef XBOX_GUEST_DET_DEVICE_H
#define XBOX_GUEST_DET_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XBOX_DET_DEV_NV2A,          /* the push-buffer walker (nv2a-ack thread) */
    XBOX_DET_DEV_TIMER,         /* the timer thread: vblank, DPCs, timers */
    XBOX_DET_DEV_APU,           /* the APU frame thread: one frame a pass */
    XBOX_DET_DEV_COUNT
};

/* The APU's frames per vblank step: 48 kHz in 32-sample frames is 1500 a
 * second, 25 a 60 Hz vblank. */
#define XBOX_DET_APU_FRAMES_PER_VBLANK 25

/* Every file time the guest reads while the mode is on: 2004-01-01, the
 * virtual clock's epoch. A file the run creates (the fresh HDD tree, a
 * cache file) otherwise carries the host's wall clock, which differs on
 * every run and lands in guest memory at the first directory query. */
#define XBOX_DET_FILETIME 127173888000000000LL

int  xbox_GuestDetOn(void);
int  xbox_GuestDetUndecided(void);       /* 1 until the first guest join decides the mode */
void xbox_GuestDetDeviceAdd(int id);        /* before the device's thread starts */
void xbox_GuestDetDeviceRemove(int id);     /* the device thread is leaving */
void xbox_GuestDetDeviceWait(int id);       /* the device thread: until granted a pass */
void xbox_GuestDetDeviceDone(int id, int worked);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_GUEST_DET_DEVICE_H */
