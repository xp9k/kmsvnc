#pragma once

#include <rfb/rfb.h>

/*
 * Миниатюры dno-tool: клиент объявляет псевдокодировку и получает в ответ
 * подтверждение. Только после него клиент присылает желаемый размер, и сервер
 * отдаёт этому клиенту кадр, уменьшенный в целое число раз. Остальные клиенты
 * (просмотр, управление) продолжают получать полный кадр.
 */
#define KMSVNC_THUMBNAIL_ENCODING 0x444E4F54 /* "DNOT" */
#define KMSVNC_THUMBNAIL_MESSAGE 0xC4
#define KMSVNC_THUMBNAIL_VERSION 1

/* Регистрирует расширение (до rfbInitServer). */
void thumbnail_register(void);
/* Нужно ли снимать экран на этом такте (редкая съёмка при одних миниатюрах). */
int thumbnail_capture_due(rfbScreenInfoPtr server);
