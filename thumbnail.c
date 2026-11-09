/*
 * Уменьшенный кадр для миниатюр dno-tool.
 *
 * Протокол:
 *   1. Клиент добавляет KMSVNC_THUMBNAIL_ENCODING в SetEncodings.
 *   2. Сервер сразу отвечает FramebufferUpdate с одним прямоугольником этой
 *      псевдокодировки без данных: x — версия, y — наибольший делитель,
 *      w и h — полный размер экрана.
 *   3. Клиент присылает сообщение KMSVNC_THUMBNAIL_MESSAGE:
 *      u8 тип, u8 флаги, u16 ширина, u16 высота (0×0 — полный размер).
 *   4. Если размер кадра для клиента изменился, следующий FramebufferUpdate
 *      содержит DesktopSize (-223) с новым размером, затем кадр этого размера.
 *   5. (версия 2) Флаг KMSVNC_THUMBNAIL_FLAG_CAPTURE с текущим размером перед
 *      FramebufferUpdateRequest: «сними экран и ответь снимком». Сервер ставит
 *      клиента на удержание (cl->onHold: поток отправки libvncserver ждёт),
 *      снимает экран и отпускает клиента. Ответ на запрос поэтому всегда
 *      содержит снимок, сделанный после запроса. Если экран не изменился,
 *      отмечается пиксель (0,0): ответ приходит сразу, клиент не ждёт таймаута.
 *      Сервер версии 1 байт флагов не читает: для него это запрос того же
 *      размера, то есть ничего.
 *   6. (версия 3) Псевдокодировка KMSVNC_LIVE_ENCODING вместо
 *      KMSVNC_THUMBNAIL_ENCODING: «живой» клиент (окно VNC на экране 4K).
 *      Подтверждение и сообщение размера те же, в подтверждении — эта
 *      псевдокодировка. Клиент получает уменьшенный кадр, но снимается с
 *      полной частотой, как обычный, без удержания. Мышь масштабированного
 *      клиента libvncserver пересчитывает в полный экран сама.
 *
 * Уменьшение выполняет libvncserver (scaled screen на клиента, усреднение по
 * блоку). Его усреднение верно только при целом делителе: при дробном
 * отношении правая и нижняя части экрана обрезаются. Поэтому сервер выбирает
 * целый делитель, при котором кадр не меньше запрошенного размера.
 * Набор возможных размеров конечен (делители до THUMBNAIL_MAX_DIVISOR), и
 * буферы libvncserver, которые она не освобождает до остановки сервера,
 * в сумме не превышают двух третей полного кадра.
 *
 * Модуль обращается к полям rfbClientRec (sendMutex, extensions), поэтому
 * собирается только вместе со встроенной libvncserver: у системной под тем
 * же soname раскладка в МОС 12 и МОС 13 разная (CMakeLists.txt).
 *
 * Частота съёмки: клиенты полного кадра (включая «живых») — по их открытым
 * запросам, не чаще --fps (kmsvnc.c). Если подключены только клиенты миниатюр, экран снимается
 * по их запросам (флаг снимка, не чаще THUMBNAIL_MIN_CAPTURE_GAP_MS), а также
 * сразу после подключения клиента и смены размера. Клиент миниатюр, который
 * флаг не присылает (dno-tool до 0.15.12), получает съёмку раз в
 * THUMBNAIL_CAPTURE_INTERVAL_MS. Съёмка на полной частоте занимает почти ядро
 * процессора, а миниатюра запрашивает кадр раз в несколько секунд.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "thumbnail.h"

/* Экспортируется libvncserver, но не объявлена в rfb.h. */
extern void rfbScalingSetup(rfbClientPtr cl, int width, int height);

#define THUMBNAIL_MIN_SIDE 16
#define THUMBNAIL_MAX_DIVISOR 64
#define THUMBNAIL_CAPTURE_INTERVAL_MS 1000
#define THUMBNAIL_MIN_CAPTURE_GAP_MS 100

static int thumbnail_encodings[] = { KMSVNC_THUMBNAIL_ENCODING, KMSVNC_LIVE_ENCODING, 0 };
static rfbProtocolExtension thumbnail_extension;

/*
 * Клиент миниатюр: прежний обработчик отключения (ввод kmsvnc) и снимок по
 * запросу. Поля снимка меняются под registry_lock.
 */
struct thumbnail_client {
    rfbClientPtr cl;
    ClientGoneHookPtr previous_gone_hook;
    int on_demand;        /* присылал флаг снимка: периодическая съёмка не нужна */
    int live;             /* «живой» клиент: полный клиент с уменьшенным кадром */
    int capture_wanted;   /* ждёт снимка, ещё не начатого */
    int capture_taken;    /* ждёт снимка, который сейчас делается */
    struct thumbnail_client *next;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct thumbnail_client *registry = NULL;
static int thumbnail_client_count = 0;
static int thumbnail_on_demand_count = 0;
static int thumbnail_live_count = 0;
/* Снять экран без ожидания (новый уменьшенный буфер заполнен из старого снимка). */
static int thumbnail_capture_requested = 0;
/* Есть клиент с флагом снимка, которому снимок ещё не начат. */
static int thumbnail_demand_pending = 0;

/* Удержание клиента: поток отправки libvncserver не отвечает на запросы. */
static void thumbnail_hold(rfbClientPtr cl, rfbBool hold)
{
    __atomic_store_n(&cl->onHold, hold, __ATOMIC_SEQ_CST);
}

/* Вызывается под registry_lock. */
static struct thumbnail_client *thumbnail_find(rfbClientPtr cl)
{
    struct thumbnail_client *state = registry;
    while (state && state->cl != cl) {
        state = state->next;
    }
    return state;
}

/*
 * Повторяет вычисление размера блока в libvncserver (ScaleX(scaled, full, 1)).
 * Из-за округления double для некоторых экранов блок получается на единицу
 * больше делителя, такой делитель не используется.
 */
static int thumbnail_box_matches(int full, int divisor)
{
    int scaled = full / divisor;
    if (scaled < THUMBNAIL_MIN_SIDE) {
        return 0;
    }
    return (int)(((double)1 / (double)scaled) * (double)full) == divisor;
}

/* Наибольший целый делитель, при котором кадр вписывается не меньше запроса. */
static int thumbnail_divisor(int full_width, int full_height, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return 1;
    }
    int divisor = full_width / width;
    if (full_height / height > divisor) {
        divisor = full_height / height;
    }
    if (divisor > THUMBNAIL_MAX_DIVISOR) {
        divisor = THUMBNAIL_MAX_DIVISOR;
    }
    while (divisor > 1 && !(thumbnail_box_matches(full_width, divisor) &&
                            thumbnail_box_matches(full_height, divisor))) {
        divisor--;
    }
    return divisor < 1 ? 1 : divisor;
}

/*
 * Отключение клиента миниатюр: вызывается один раз, в потоке клиента, когда
 * поток отправки уже завершён. libvncserver не освобождает запись расширения
 * клиента (rfbDisableExtension освобождает только данные), поэтому запись
 * отцепляется и освобождается здесь.
 */
static void thumbnail_client_gone(rfbClientPtr cl)
{
    rfbExtensionData **link = &cl->extensions;
    while (*link && (*link)->extension != &thumbnail_extension) {
        link = &(*link)->next;
    }
    if (*link) {
        rfbExtensionData *entry = *link;
        *link = entry->next;
        free(entry);
    }

    ClientGoneHookPtr hook = NULL;
    pthread_mutex_lock(&registry_lock);
    struct thumbnail_client **state_link = &registry;
    while (*state_link && (*state_link)->cl != cl) {
        state_link = &(*state_link)->next;
    }
    struct thumbnail_client *state = *state_link;
    if (state) {
        *state_link = state->next;
        hook = state->previous_gone_hook;
        __atomic_sub_fetch(&thumbnail_client_count, 1, __ATOMIC_SEQ_CST);
        if (state->live) {
            __atomic_sub_fetch(&thumbnail_live_count, 1, __ATOMIC_SEQ_CST);
        }
        if (state->on_demand) {
            __atomic_sub_fetch(&thumbnail_on_demand_count, 1, __ATOMIC_SEQ_CST);
        }
    }
    pthread_mutex_unlock(&registry_lock);
    free(state);
    if (hook) {
        hook(cl);
    }
}

static rfbBool thumbnail_enable(rfbClientPtr cl, void **data, int encoding)
{
    (void)data; /* состояние в реестре; запись расширения освобождает thumbnail_client_gone */
    if (encoding != KMSVNC_THUMBNAIL_ENCODING && encoding != KMSVNC_LIVE_ENCODING) {
        return FALSE;
    }
    int live = encoding == KMSVNC_LIVE_ENCODING;
    int first = 0, known = 0;
    pthread_mutex_lock(&registry_lock);
    if (thumbnail_find(cl)) {
        known = 1;
    } else {
        struct thumbnail_client *state = calloc(1, sizeof(*state));
        if (state) {
            state->cl = cl;
            state->live = live;
            state->previous_gone_hook = cl->clientGoneHook;
            cl->clientGoneHook = thumbnail_client_gone;
            state->next = registry;
            registry = state;
            __atomic_add_fetch(&thumbnail_client_count, 1, __ATOMIC_SEQ_CST);
            if (live) {
                __atomic_add_fetch(&thumbnail_live_count, 1, __ATOMIC_SEQ_CST);
            }
            first = 1;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    if (!first) {
        /* Повторный SetEncodings: подтверждение уже отправлено. */
        return known;
    }
    char message[sz_rfbFramebufferUpdateMsg + sz_rfbFramebufferUpdateRectHeader];
    rfbFramebufferUpdateMsg update;
    rfbFramebufferUpdateRectHeader rect;
    update.type = rfbFramebufferUpdate;
    update.pad = 0;
    update.nRects = Swap16IfLE(1);
    rect.r.x = Swap16IfLE(KMSVNC_THUMBNAIL_VERSION);
    rect.r.y = Swap16IfLE(THUMBNAIL_MAX_DIVISOR);
    rect.r.w = Swap16IfLE(cl->screen->width);
    rect.r.h = Swap16IfLE(cl->screen->height);
    rect.encoding = Swap32IfLE(encoding);
    memcpy(message, &update, sz_rfbFramebufferUpdateMsg);
    memcpy(message + sz_rfbFramebufferUpdateMsg, &rect, sz_rfbFramebufferUpdateRectHeader);

    /* Обновления кадра отправляет другой поток, он тоже берёт sendMutex. */
    LOCK(cl->sendMutex);
    int written = rfbWriteExact(cl, message, sizeof(message));
    UNLOCK(cl->sendMutex);
    if (written < 0) {
        rfbLogPerror("thumbnail: write");
        rfbCloseClient(cl);
    }
    return TRUE;
}

static rfbBool thumbnail_message(rfbClientPtr cl, void *data, const rfbClientToServerMsg *message)
{
    (void)data;
    if (message->type != KMSVNC_THUMBNAIL_MESSAGE) {
        return FALSE;
    }
    uint8_t body[5];
    int got = rfbReadExact(cl, (char *)body, sizeof(body));
    if (got <= 0) {
        if (got < 0) {
            rfbLogPerror("thumbnail: read");
        }
        rfbCloseClient(cl);
        return TRUE;
    }
    pthread_mutex_lock(&registry_lock);
    struct thumbnail_client *known = thumbnail_find(cl);
    int live = known && known->live;
    pthread_mutex_unlock(&registry_lock);
    /* «Живой» клиент снимается как полный: флаг снимка ему не нужен. */
    if (!live && (body[0] & KMSVNC_THUMBNAIL_FLAG_CAPTURE)) {
        /*
         * Удержание ставится здесь, в потоке ввода клиента, до разбора
         * следующего за флагом FramebufferUpdateRequest: поток отправки не
         * ответит на запрос, пока снимок не готов (thumbnail_capture_finished).
         */
        pthread_mutex_lock(&registry_lock);
        struct thumbnail_client *state = thumbnail_find(cl);
        if (state) {
            if (!state->on_demand) {
                state->on_demand = 1;
                __atomic_add_fetch(&thumbnail_on_demand_count, 1, __ATOMIC_SEQ_CST);
            }
            state->capture_wanted = 1;
            thumbnail_hold(cl, TRUE);
            __atomic_store_n(&thumbnail_demand_pending, 1, __ATOMIC_SEQ_CST);
        }
        pthread_mutex_unlock(&registry_lock);
    }
    int width = (body[1] << 8) | body[2];
    int height = (body[3] << 8) | body[4];
    int divisor = thumbnail_divisor(cl->screen->width, cl->screen->height, width, height);
    int scaled_width = cl->screen->width / divisor;
    int scaled_height = cl->screen->height / divisor;
    if (cl->scaledScreen && cl->scaledScreen->width == scaled_width &&
        cl->scaledScreen->height == scaled_height) {
        /* Размер не изменился: кадр клиента остаётся действительным. */
        return TRUE;
    }
    /*
     * Поток отправки кодирует прямоугольники из cl->scaledScreen под
     * sendMutex. Смена экрана посреди кодирования дала бы чтение за
     * пределами меньшего буфера. rfbScalingSetup ставит newFBSizePending:
     * ближайшее обновление отправит DesktopSize, затем кадр нового размера.
     */
    LOCK(cl->sendMutex);
    rfbScalingSetup(cl, scaled_width, scaled_height);
    UNLOCK(cl->sendMutex);
    /*
     * rfbScalingSetup поток отправки не будит: без изменения экрана новый
     * размер не уходил бы клиенту, а мышь клиента, ещё не знающего о смене,
     * пересчитывалась бы из уменьшенного кадра в полный (за край экрана).
     * newFBSizePending входит в FB_UPDATE_PENDING, достаточно сигнала.
     */
    LOCK(cl->updateMutex);
    TSIGNAL(cl->updateCond);
    UNLOCK(cl->updateMutex);
    __atomic_store_n(&thumbnail_capture_requested, 1, __ATOMIC_SEQ_CST);
    return TRUE;
}

static long long thumbnail_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int thumbnail_client_passive(rfbClientPtr cl)
{
    pthread_mutex_lock(&registry_lock);
    struct thumbnail_client *state = thumbnail_find(cl);
    int passive = state && !state->live;
    pthread_mutex_unlock(&registry_lock);
    return passive;
}

int thumbnail_capture_due(rfbScreenInfoPtr server, int live)
{
    static int last_clients = 0;
    static long long last_capture = 0;

    int clients = 0;
    rfbClientIteratorPtr iterator = rfbGetClientIterator(server);
    while (rfbClientIteratorNext(iterator)) {
        clients++;
    }
    rfbReleaseClientIterator(iterator);

    int previous_clients = last_clients;
    last_clients = clients;
    if (clients == 0) {
        return 0;
    }
    long long now = thumbnail_now_ms();
    int thumbnails = __atomic_load_n(&thumbnail_client_count, __ATOMIC_SEQ_CST) -
        __atomic_load_n(&thumbnail_live_count, __ATOMIC_SEQ_CST);
    int on_demand = __atomic_load_n(&thumbnail_on_demand_count, __ATOMIC_SEQ_CST);
    int demand = __atomic_load_n(&thumbnail_demand_pending, __ATOMIC_SEQ_CST);
    int requested = __atomic_exchange_n(&thumbnail_capture_requested, 0, __ATOMIC_SEQ_CST);
    long long elapsed = now - last_capture;
    /*
     * Клиенты полного кадра снимаются, пока у них открыт запрос (live,
     * считает kmsvnc.c): пока клиент принимает прошлый кадр, снимать для
     * него незачем. Периодическая съёмка нужна, только пока есть клиент
     * миниатюр без флага снимка.
     */
    int periodic = on_demand < thumbnails && elapsed >= THUMBNAIL_CAPTURE_INTERVAL_MS;
    int on_request = demand && elapsed >= THUMBNAIL_MIN_CAPTURE_GAP_MS;
    if (!(live || requested || clients > previous_clients || periodic ||
          on_request)) {
        return 0;
    }
    last_capture = now;
    /*
     * Снимок начинается после этой точки: он отвечает только на запросы,
     * пришедшие до неё. Запрос, пришедший позже, ждёт следующего снимка.
     */
    pthread_mutex_lock(&registry_lock);
    for (struct thumbnail_client *state = registry; state; state = state->next) {
        if (state->capture_wanted) {
            state->capture_wanted = 0;
            state->capture_taken = 1;
        }
    }
    __atomic_store_n(&thumbnail_demand_pending, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&registry_lock);
    return 1;
}

void thumbnail_capture_finished(rfbScreenInfoPtr server, int changed)
{
    int taken = 0;
    pthread_mutex_lock(&registry_lock);
    for (struct thumbnail_client *state = registry; state; state = state->next) {
        taken |= state->capture_taken;
    }
    pthread_mutex_unlock(&registry_lock);
    if (!taken) {
        return;
    }
    /*
     * Без изменений открытый запрос остался бы без ответа до следующего
     * изменения экрана. Отмеченный пиксель (0,0) даёт ответ сразу; его
     * содержимое и так совпадает с кадром клиента. Отметка вне registry_lock:
     * rfbMarkRectAsModified берёт блокировки libvncserver.
     */
    if (!changed) {
        rfbMarkRectAsModified(server, 0, 0, 1, 1);
    }
    pthread_mutex_lock(&registry_lock);
    for (struct thumbnail_client *state = registry; state; state = state->next) {
        if (state->capture_taken) {
            state->capture_taken = 0;
            /* Новый флаг пришёл во время снимка: удержание до следующего. */
            if (!state->capture_wanted) {
                thumbnail_hold(state->cl, FALSE);
            }
        }
    }
    pthread_mutex_unlock(&registry_lock);
}

static rfbProtocolExtension thumbnail_extension = {
    .newClient = NULL,
    .init = NULL,
    .pseudoEncodings = thumbnail_encodings,
    .enablePseudoEncoding = thumbnail_enable,
    .handleMessage = thumbnail_message,
    .close = NULL,
    .usage = NULL,
    .processArgument = NULL,
    .next = NULL,
};

void thumbnail_register(void)
{
    rfbRegisterProtocolExtension(&thumbnail_extension);
}
