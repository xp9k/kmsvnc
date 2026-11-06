#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <json-c/json.h>

#include "screen_layout.h"

#define MAX_RESPONSE (1024 * 1024)
#define MAX_COORDINATE 16384

/* В команды попадают только проверенные имена сеанса, пользователя и сокета. */
static int safe_name(const char *value)
{
    if (!value || !*value) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
        if (!isalnum(*p) && !strchr("_.:-", *p)) return 0;
    return 1;
}

static int shell_quote(const char *value, char *output, size_t capacity)
{
    size_t used = 0;
    if (capacity < 3) return 0;
    output[used++] = '\'';
    for (; *value; value++) {
        const char *part = *value == '\'' ? "'\\''" : NULL;
        size_t length = part ? 4 : 1;
        if (used + length + 2 > capacity) return 0;
        if (part) memcpy(output + used, part, length);
        else output[used] = *value;
        used += length;
    }
    output[used++] = '\'';
    output[used] = '\0';
    return 1;
}

static char *command_output(const char *command)
{
    FILE *pipe = popen(command, "r");
    if (!pipe) return NULL;
    char *text = malloc(MAX_RESPONSE + 1);
    if (!text) { pclose(pipe); return NULL; }
    size_t size = fread(text, 1, MAX_RESPONSE, pipe);
    int failed = ferror(pipe) || !feof(pipe);
    text[size] = '\0';
    if (pclose(pipe) != 0) failed = 1;
    if (failed) { free(text); return NULL; }
    return text;
}

static const char *property(const char *text, const char *key, char *value, size_t capacity)
{
    size_t length = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t size = end ? (size_t)(end - line) : strlen(line);
        if (size > length && !strncmp(line, key, length) && line[length] == '=') {
            size_t count = size - length - 1;
            if (count >= capacity) return NULL;
            memcpy(value, line + length + 1, count);
            value[count] = '\0';
            return value;
        }
        line = end ? end + 1 : NULL;
    }
    return NULL;
}

static int wayland_socket(unsigned uid, char *name, size_t capacity)
{
    char directory[64], path[256];
    snprintf(directory, sizeof(directory), "/run/user/%u", uid);
    DIR *dir = opendir(directory);
    if (!dir) return 0;
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "wayland-", 8) || !safe_name(entry->d_name)) continue;
        if (strlen(entry->d_name) >= capacity) continue;
        int length = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) continue;
        struct stat info;
        if (stat(path, &info) || !S_ISSOCK(info.st_mode)) continue;
        if (!found || strcmp(entry->d_name, name) < 0) {
            strcpy(name, entry->d_name);
            found = 1;
        }
    }
    closedir(dir);
    return found;
}

static int json_integer(struct json_object *object, const char *key, int minimum, int maximum, int *value)
{
    struct json_object *field;
    if (!json_object_object_get_ex(object, key, &field) || !json_object_is_type(field, json_type_int)) return 0;
    int64_t number = json_object_get_int64(field);
    if (number < minimum || number > maximum) return 0;
    *value = (int)number;
    return 1;
}

static int parse_layout(const char *text, struct kmsvnc_output_layout *outputs, int capacity)
{
    struct json_object *root = json_tokener_parse(text), *array;
    if (!root) return 0;
    int count = 0;
    if (!json_object_object_get_ex(root, "outputs", &array) || !json_object_is_type(array, json_type_array)) goto done;
    for (size_t n = 0; n < json_object_array_length(array); n++) {
        struct json_object *item = json_object_array_get_idx(array, n), *field, *position, *size;
        if (!json_object_object_get_ex(item, "connected", &field) || !json_object_get_boolean(field)) continue;
        if (!json_object_object_get_ex(item, "enabled", &field) || !json_object_get_boolean(field)) continue;
        if (count >= capacity) { count = 0; goto done; }
        if (!json_object_object_get_ex(item, "name", &field) || !json_object_is_type(field, json_type_string)) { count = 0; goto done; }
        const char *name = json_object_get_string(field);
        if (!safe_name(name) || strlen(name) >= sizeof(outputs[count].name)) { count = 0; goto done; }
        if (!json_object_object_get_ex(item, "pos", &position) || !json_object_object_get_ex(item, "size", &size)) { count = 0; goto done; }
        struct kmsvnc_output_layout *out = &outputs[count];
        if (!json_integer(position, "x", -MAX_COORDINATE, MAX_COORDINATE, &out->x) ||
            !json_integer(position, "y", -MAX_COORDINATE, MAX_COORDINATE, &out->y) ||
            !json_integer(size, "width", 1, MAX_COORDINATE, &out->width) ||
            !json_integer(size, "height", 1, MAX_COORDINATE, &out->height)) { count = 0; goto done; }
        strcpy(out->name, name);
        count++;
    }
    if (count) {
        int min_x = outputs[0].x, min_y = outputs[0].y;
        for (int n = 1; n < count; n++) {
            if (outputs[n].x < min_x) min_x = outputs[n].x;
            if (outputs[n].y < min_y) min_y = outputs[n].y;
        }
        for (int n = 0; n < count; n++) {
            outputs[n].x -= min_x;
            outputs[n].y -= min_y;
            if (outputs[n].x > MAX_COORDINATE - outputs[n].width ||
                outputs[n].y > MAX_COORDINATE - outputs[n].height) { count = 0; goto done; }
        }
    }
done:
    json_object_put(root);
    return count;
}

/* Читаем схему активного локального сеанса KDE через штатные утилиты системы. */
int screen_layout_read(struct kmsvnc_output_layout *outputs, int capacity)
{
    char *sessions = command_output("timeout 4 loginctl list-sessions --no-legend 2>/dev/null");
    if (!sessions) return 0;
    int count = 0;
    char *next = NULL;
    for (char *line = strtok_r(sessions, "\n", &next); line; line = strtok_r(NULL, "\n", &next)) {
        char session[64], user[64], seat[64], command[2048], active[16], type[32], remote[16];
        unsigned uid;
        if (sscanf(line, "%63s %u %63s %63s", session, &uid, user, seat) != 4) continue;
        if (!strcmp(seat, "-") || !safe_name(session)) continue;
        snprintf(command, sizeof(command), "timeout 4 loginctl show-session '%s' -p Active -p Type -p Remote -p Display 2>/dev/null", session);
        char *properties = command_output(command);
        if (!properties) continue;
        int eligible = property(properties, "Active", active, sizeof(active)) && !strcmp(active, "yes") &&
            property(properties, "Type", type, sizeof(type)) && (!strcmp(type, "wayland") || !strcmp(type, "x11"));
        if (property(properties, "Remote", remote, sizeof(remote)) && !strcmp(remote, "yes")) eligible = 0;
        struct passwd *account = eligible ? getpwuid(uid) : NULL;
        if (!account || !safe_name(account->pw_name) || (geteuid() != 0 && geteuid() != uid)) { free(properties); continue; }
        char display[64], environment[1024];
        if (!strcmp(type, "wayland")) {
            if (!wayland_socket(uid, display, sizeof(display))) { free(properties); continue; }
            snprintf(environment, sizeof(environment), "QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY='%s'", display);
        } else {
            if (!property(properties, "Display", display, sizeof(display)) || !*display) strcpy(display, ":0");
            char authority[512], quoted[768];
            int length = snprintf(authority, sizeof(authority), "%s/.Xauthority", account->pw_dir);
            if (!safe_name(display) || length < 0 || (size_t)length >= sizeof(authority) || !shell_quote(authority, quoted, sizeof(quoted))) { free(properties); continue; }
            snprintf(environment, sizeof(environment), "QT_QPA_PLATFORM=xcb DISPLAY='%s' XAUTHORITY=%s", display, quoted);
        }
        if (geteuid() == 0) snprintf(command, sizeof(command),
            "timeout 6 runuser -u '%s' -- env XDG_RUNTIME_DIR=/run/user/%u DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus %s kscreen-doctor -j 2>/dev/null",
            account->pw_name, uid, uid, environment);
        else snprintf(command, sizeof(command),
            "timeout 6 env XDG_RUNTIME_DIR=/run/user/%u DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus %s kscreen-doctor -j 2>/dev/null",
            uid, uid, environment);
        free(properties);
        char *json = command_output(command);
        if (!json) continue;
        count = parse_layout(json, outputs, capacity);
        free(json);
        if (count) break;
    }
    free(sessions);
    return count;
}
