#pragma once
#include <rfb/rfb.h>

int egl_capture_init(void);
void egl_capture_cleanup(void);
void egl_capture_frame(const char *unused, int width, int height, char *output);
void egl_capture_configure_vnc(rfbScreenInfoPtr server);
