#pragma once
#include <rfb/rfb.h>

/*
 * own_device: GBM/EGL на собственном fd render node с правом записи. Карта
 * открыта только на чтение, а iris (Intel) пишет в служебный буфер при
 * создании устройства и на таком fd падает. NVIDIA работает с fd карты.
 */
int egl_capture_init(int own_device);
void egl_capture_cleanup(void);
void egl_capture_frame(const char *unused, int width, int height, char *output);
void egl_capture_configure_vnc(rfbScreenInfoPtr server);
