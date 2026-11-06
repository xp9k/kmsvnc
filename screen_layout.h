#pragma once

struct kmsvnc_output_layout {
    char name[32];
    int x, y, width, height;
};

int screen_layout_read(struct kmsvnc_output_layout *outputs, int capacity);
