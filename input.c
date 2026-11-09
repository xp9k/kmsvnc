#include <stdio.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>

#include "input.h"
#include "keymap.h"

extern struct kmsvnc_data *kmsvnc;
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;

struct input_client_state {
    char keys[UINPUT_MAX_KEY];
    int buttons;
};

static int client_key_held(rfbScreenInfoPtr screen, int key)
{
    int held = 0;
    rfbClientIteratorPtr iterator = rfbGetClientIterator(screen);
    rfbClientPtr other;
    while ((other = rfbClientIteratorNext(iterator))) {
        struct input_client_state *state = other->clientData;
        if (state && state->keys[key]) { held = 1; break; }
    }
    rfbReleaseClientIterator(iterator);
    return held;
}

static int client_buttons(rfbScreenInfoPtr screen)
{
    int buttons = 0;
    rfbClientIteratorPtr iterator = rfbGetClientIterator(screen);
    rfbClientPtr other;
    while ((other = rfbClientIteratorNext(iterator))) {
        struct input_client_state *state = other->clientData;
        if (state) buttons |= state->buttons;
    }
    rfbReleaseClientIterator(iterator);
    return buttons;
}

void uinput_cleanup()
{
    if (kmsvnc->input) {
        if (kmsvnc->input->uinput_fd > 0){
            INP_IOCTL_MAY(kmsvnc->input->uinput_fd, UI_DEV_DESTROY);
            close(kmsvnc->input->uinput_fd);
            kmsvnc->input->uinput_fd = 0;
        }
        if (kmsvnc->input->keystate){
            free(kmsvnc->input->keystate);
            kmsvnc->input->keystate = NULL;
        }
        free(kmsvnc->input);
        kmsvnc->input = NULL;
    }
}

static void wake_system_up();
int uinput_init()
{
    struct kmsvnc_input_data *inp = malloc(sizeof(struct kmsvnc_input_data));
    if (!inp) KMSVNC_FATAL("memory allocation error at %s:%d\n", __FILE__, __LINE__);
    memset(inp, 0, sizeof(struct kmsvnc_input_data));
    kmsvnc->input = inp;

    inp->uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (inp->uinput_fd <= 0)
    {
        KMSVNC_FATAL("Failed to open uinput\n");
    }
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_EVBIT, EV_KEY);
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_EVBIT, EV_SYN);
    for (int i = 0; i < UINPUT_MAX_KEY; i++)
    {
        INP_IOCTL_MUST(inp->uinput_fd, UI_SET_KEYBIT, i);
    }

    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_EVBIT, EV_ABS);
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_ABSBIT, ABS_X);
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_ABSBIT, ABS_Y);

    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_KEYBIT, BTN_LEFT);
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_KEYBIT, BTN_MIDDLE);
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_KEYBIT, BTN_RIGHT);
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_EVBIT, EV_REL);
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_RELBIT, REL_WHEEL);
    INP_IOCTL_MUST(inp->uinput_fd, UI_SET_RELBIT, REL_HWHEEL);

    struct uinput_abs_setup abs;
    memset(&abs, 0, sizeof(abs));
    abs.absinfo.maximum = UINPUT_ABS_MAX;
    abs.absinfo.minimum = 0;
    abs.code = ABS_X;
    INP_IOCTL_MUST(inp->uinput_fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_Y;
    INP_IOCTL_MUST(inp->uinput_fd, UI_ABS_SETUP, &abs);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x0011;
    usetup.id.product = 0x4514;
    strcpy(usetup.name, "kmsvnc");

    INP_IOCTL_MUST(inp->uinput_fd, UI_DEV_SETUP, &usetup);
    INP_IOCTL_MUST(inp->uinput_fd, UI_DEV_CREATE);

    inp->keystate = malloc(UINPUT_MAX_KEY);
    if (!inp->keystate) KMSVNC_FATAL("memory allocation error at %s:%d\n", __FILE__, __LINE__);
    memset(inp->keystate, 0, UINPUT_MAX_KEY);

    if (kmsvnc->input_wakeup) {
        printf("waiting for 1 second for userspace to detect the input devive...\n");
        sleep(1);
        wake_system_up();
        printf("waiting for 1 second for mouse input to be processed...\n");
        sleep(1);
    }
    return 0;
}

void rfb_key_hook(rfbBool down, rfbKeySym keysym, rfbClientPtr cl)
{
    struct key_iter_search search = {
        .keysym = keysym,
        .keycode = XKB_KEYCODE_INVALID,
        .level = 0,
    };
    xkb_keymap_key_for_each(kmsvnc->keymap->map, key_iter, &search);
    if (search.keycode == XKB_KEYCODE_INVALID)
    {
        fprintf(stderr, "Keysym %04x not found in our keymap\n", keysym);
        return;
    }
    // printf("key %s, keysym %04x, keycode %u\n", down ? "down" : "up", keysym, search.keycode);
    if (search.keycode >= UINPUT_MAX_KEY)
    {
        fprintf(stderr, "Keycode %d >= %d\n", search.keycode, UINPUT_MAX_KEY);
        return;
    }
    struct input_client_state *state = cl->clientData;
    if (!state) return;
    /* Ввод выводит захват из простоя: изменение снимется без задержки. */
    capture_wakeup(CAPTURE_WAKE_ACTIVITY);
    pthread_mutex_lock(&input_lock);
    state->keys[search.keycode] = !!down;
    if (!down && client_key_held(cl->screen, search.keycode)) {
        pthread_mutex_unlock(&input_lock);
        return;
    }
    /* Повторы клавиш приходят от клиента, включая удержание стрелок. */
    if (down || kmsvnc->input->keystate[search.keycode])
    {
        /* Сенсорная клавиатура может прислать символ без отдельного Shift. */
        int shift = search.level == 1 &&
            !kmsvnc->input->keystate[KEY_LEFTSHIFT + 8] &&
            !kmsvnc->input->keystate[KEY_RIGHTSHIFT + 8];
        struct input_event modifier[] = {
            { .type = EV_KEY, .code = KEY_LEFTSHIFT, .value = 1 },
            { .type = EV_SYN, .code = SYN_REPORT },
        };
        if (shift) KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, modifier, sizeof(modifier));
        struct input_event ies[] = {
            {
                .type = EV_KEY,
                .code = search.keycode - 8, // magic
                .value = down ? (kmsvnc->input->keystate[search.keycode] ? 2 : 1) : 0,
                .time.tv_sec = 0,
                .time.tv_usec = 0,
            },
            {
                .type = EV_SYN,
                .code = SYN_REPORT,
                .value = 0,
            },
        };
        for (int i = 0; i < KMSVNC_ARRAY_ELEMENTS(ies); i++)
        {
            KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, &ies[i], sizeof(ies[0]));
        }

        if (shift) {
            modifier[0].value = 0;
            KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, modifier, sizeof(modifier));
        }
        kmsvnc->input->keystate[search.keycode] = !!down;
    }
    pthread_mutex_unlock(&input_lock);
}

void rfb_ptr_hook(int mask, int screen_x, int screen_y, rfbClientPtr cl)
{
    struct input_client_state *state = cl->clientData;
    if (!state) return;
    capture_wakeup(CAPTURE_WAKE_ACTIVITY);
    pthread_mutex_lock(&input_lock);
    state->buttons = mask & 7;
    int buttons = client_buttons(cl->screen);
    // printf("pointer to %d, %d\n", screen_x, screen_y);
    float global_x = (float)(screen_x + kmsvnc->input_offx);
    float global_y = (float)(screen_y + kmsvnc->input_offy);
    int touch_x = round(global_x / (kmsvnc->input_width ?: kmsvnc->drm->mfb->width) * UINPUT_ABS_MAX);
    int touch_y = round(global_y / (kmsvnc->input_height ?: kmsvnc->drm->mfb->height) * UINPUT_ABS_MAX);
    struct input_event ies1[] = {
        {
            .type = EV_ABS,
            .code = ABS_X,
            .value = touch_x,
        },
        {
            .type = EV_ABS,
            .code = ABS_Y,
            .value = touch_y,
        },
        {
            .type = EV_KEY,
            .code = BTN_LEFT,
            .value = !!(buttons & 0b1)},
        {
            .type = EV_KEY,
            .code = BTN_MIDDLE,
            .value = !!(buttons & 0b10)},
        {
            .type = EV_KEY,
            .code = BTN_RIGHT,
            .value = !!(buttons & 0b100)},
        {
            .type = EV_SYN,
            .code = SYN_REPORT,
            .value = 0,
        },
    };
    for (int i = 0; i < KMSVNC_ARRAY_ELEMENTS(ies1); i++)
    {
        KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, &ies1[i], sizeof(ies1[0]));
    }
    if (mask & 0b11000)
    {
        struct input_event ies2[] = {
            {
                .type = EV_REL,
                .code = REL_WHEEL,
                .value = mask & 0b1000 ? 1 : -1,
            },
            {
                .type = EV_SYN,
                .code = SYN_REPORT,
                .value = 0,
            },
        };
        for (int i = 0; i < KMSVNC_ARRAY_ELEMENTS(ies2); i++)
        {
            KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, &ies2[i], sizeof(ies2[0]));
        }
    }
    if (mask & 0b1100000) {
        struct input_event horizontal[] = {
            { .type = EV_REL, .code = REL_HWHEEL,
              .value = mask & 0b100000 ? -1 : 1 },
            { .type = EV_SYN, .code = SYN_REPORT },
        };
        KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, horizontal, sizeof(horizontal));
    }

    pthread_mutex_unlock(&input_lock);
}

/*
 * Пробуждение экрана нажатием левого Shift. Прежнее перемещение абсолютной
 * мыши ставило курсор в (0, 0) — горячий угол KWin, и каждый запуск службы
 * (установка, смена пароля, перезапуск) открывал на экране обзор окон.
 * Одиночное нажатие Shift будит экран и ничего не делает в сеансе.
 */
static void wake_system_up()
{
    struct input_event ies1[] = {
        { .type = EV_KEY, .code = KEY_LEFTSHIFT, .value = 1 },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
        { .type = EV_KEY, .code = KEY_LEFTSHIFT, .value = 0 },
        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
    };
    for (int i = 0; i < KMSVNC_ARRAY_ELEMENTS(ies1); i++)
    {
        KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, &ies1[i], sizeof(ies1[0]));
    }
}

static void input_client_gone(rfbClientPtr cl)
{
    pthread_mutex_lock(&input_lock);
    struct input_client_state *state = cl->clientData;
    cl->clientData = NULL;
    if (!state) { pthread_mutex_unlock(&input_lock); return; }
    if (!kmsvnc->input) { free(state); pthread_mutex_unlock(&input_lock); return; }
    for (int key = 8; key < UINPUT_MAX_KEY; key++) {
        if (!state->keys[key] || client_key_held(cl->screen, key)) continue;
        struct input_event release = { .type = EV_KEY, .code = key - 8, .value = 0 };
        KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, &release, sizeof(release));
        kmsvnc->input->keystate[key] = 0;
    }
    int buttons = client_buttons(cl->screen);
    struct input_event release[] = {
        { .type = EV_KEY, .code = BTN_LEFT, .value = !!(buttons & 1) },
        { .type = EV_KEY, .code = BTN_MIDDLE, .value = !!(buttons & 2) },
        { .type = EV_KEY, .code = BTN_RIGHT, .value = !!(buttons & 4) },
        { .type = EV_SYN, .code = SYN_REPORT },
    };
    KMSVNC_WRITE_MAY(kmsvnc->input->uinput_fd, release, sizeof(release));
    free(state);
    pthread_mutex_unlock(&input_lock);
}

enum rfbNewClientAction input_new_client(rfbClientPtr cl)
{
    cl->clientData = calloc(1, sizeof(struct input_client_state));
    if (!cl->clientData) return RFB_CLIENT_REFUSE;
    cl->clientGoneHook = input_client_gone;
    return RFB_CLIENT_ACCEPT;
}
