# kmsvnc

VNC-сервер для GNU/Linux, написанный на C. Захватывает экран через DRM/KMS
и передаёт ввод через uinput. Работает в X11 и Wayland, включая экран входа.

Форк [isjerryxiao/kmsvnc](https://github.com/isjerryxiao/kmsvnc).
Лицензия GPLv3 — [LICENSE](LICENSE).

## Сборка

Нужны CMake, GCC, make, pkg-config и заголовки libdrm, LibVNCServer,
xkbcommon, libva, libva-drm, EGL, GLESv2, GBM и json-c.

```bash
cmake -S . -B _build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build _build
sudo cmake --install _build
```

## Запуск

Для захвата DRM и эмуляции ввода нужны права root.

```bash
sudo kmsvnc -w -p 5900 -d /dev/dri/card0 --password-file /etc/kmsvnc/passwd
```

Файл пароля должен быть доступен только root. VNC-аутентификация использует
первые восемь символов. Полный список параметров — `kmsvnc --help`.

## Мониторы

NVIDIA захватывается через EGL DMA-BUF, включая AB30/BLOCK_LINEAR.
Сервер передаёт актуальные кадры и схему мониторов ExtendedDesktopSize.
В KDE Wayland все мониторы объединяются по реальной геометрии KWin.
Модуль `screen_layout.c` получает её через loginctl, runuser и kscreen-doctor.

При неизвестной схеме нескольких мониторов следует выбрать один выход
через `--source-plane` или `--source-crtc`. Для такого режима
`--input-width`, `--input-height`, `--input-offx`, `--input-offy` задают
соответствие выбранного изображения всему рабочему столу.

Изменение разрешения или расположения мониторов требует перезапуска сервера.
При DPMS сохраняется последний кадр каждого монитора.

Для Intel и AMD требуется рабочая настройка VAAPI. На Intel может понадобиться
отключение CCS через `INTEL_DEBUG=noccs` в окружении службы.

## Ввод

Поддерживаются абсолютная мышь, три кнопки, вертикальная и горизонтальная
прокрутка, повторы клавиш и отпускание нажатий при отключении клиента.

Для русской клавиатуры можно задать `XKB_DEFAULT_LAYOUT=us,ru`.
Активную раскладку выбирает удалённый графический сеанс.
Обмена текстовым буфером между компьютерами нет.
