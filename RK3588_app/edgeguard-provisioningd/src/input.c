#include "edgeguard_provisioning/input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

struct _EgpInput {
    int fd;
    guint fd_source;
    guint hold_source;
    guint key_code;
    guint hold_ms;
    gboolean pressed;
    EgpInputActivated activated;
    EgpInputLost lost;
    gpointer user_data;
};

static gboolean input_fail(EgpError *error, const char *message)
{
    egp_error_set(error, EGP_ERROR_INTERNAL_ERROR, TRUE,
                  EGP_PERSISTENT_CHANGE_NONE, "%s", message);
    return FALSE;
}

static int find_adc_keys(EgpError *error)
{
    int directory_fd = open("/dev/input", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        input_fail(error, "adc-keys input directory is unavailable");
        return -1;
    }
    int scan_fd = dup(directory_fd);
    DIR *directory = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!directory) {
        if (scan_fd >= 0)
            close(scan_fd);
        close(directory_fd);
        input_fail(error, "adc-keys discovery could not start");
        return -1;
    }

    int found = -1;
    guint matches = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!g_str_has_prefix(entry->d_name, "event"))
            continue;
        const char *suffix = entry->d_name + strlen("event");
        if (!*suffix || strspn(suffix, "0123456789") != strlen(suffix))
            continue;
        int fd = openat(directory_fd, entry->d_name,
                        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            continue;
        struct stat st;
        char name[256] = {0};
        if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
            ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            !strcmp(name, "adc-keys")) {
            ++matches;
            if (found >= 0)
                close(found);
            found = fd;
        } else {
            close(fd);
        }
    }
    closedir(directory);
    close(directory_fd);
    if (matches != 1) {
        if (found >= 0)
            close(found);
        input_fail(error, matches ? "adc-keys input source is ambiguous" :
                                  "adc-keys input source was not found");
        return -1;
    }
    return found;
}

static gboolean hold_elapsed(gpointer user_data)
{
    EgpInput *input = user_data;
    input->hold_source = 0;
    if (input->pressed && input->activated)
        input->activated(input->user_data);
    return G_SOURCE_REMOVE;
}

static void input_lost(EgpInput *input)
{
    if (input->hold_source) {
        g_source_remove(input->hold_source);
        input->hold_source = 0;
    }
    input->pressed = FALSE;
    input->fd_source = 0;
    if (input->fd >= 0) {
        close(input->fd);
        input->fd = -1;
    }
    if (input->lost)
        input->lost(input->user_data);
}

static gboolean input_ready(gint fd, GIOCondition condition, gpointer user_data)
{
    EgpInput *input = user_data;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        input->fd_source = 0;
        input_lost(input);
        return G_SOURCE_REMOVE;
    }
    for (;;) {
        struct input_event event;
        ssize_t count = read(fd, &event, sizeof(event));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (count != sizeof(event)) {
            input->fd_source = 0;
            input_lost(input);
            return G_SOURCE_REMOVE;
        }
        if (event.type != EV_KEY || (guint)event.code != input->key_code)
            continue;
        if (event.value == 1 && !input->pressed) {
            input->pressed = TRUE;
            input->hold_source = g_timeout_add(input->hold_ms, hold_elapsed, input);
        } else if (event.value == 0) {
            input->pressed = FALSE;
            if (input->hold_source) {
                g_source_remove(input->hold_source);
                input->hold_source = 0;
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

EgpInput *egp_input_new(guint key_code, guint hold_ms,
                        EgpInputActivated activated, EgpInputLost lost,
                        gpointer user_data, EgpError *error)
{
    if ((!key_code && hold_ms) || (key_code && !hold_ms) ||
        key_code > KEY_MAX || (hold_ms && (hold_ms < 500 || hold_ms > 10000))) {
        input_fail(error, "Physical-presence key mapping is invalid");
        return NULL;
    }
    EgpInput *input = g_new0(EgpInput, 1);
    input->fd = -1;
    input->key_code = key_code;
    input->hold_ms = hold_ms;
    input->activated = activated;
    input->lost = lost;
    input->user_data = user_data;
    if (!key_code) {
        egp_error_clear(error);
        return input;
    }
    input->fd = find_adc_keys(error);
    if (input->fd < 0) {
        g_free(input);
        return NULL;
    }
    input->fd_source = g_unix_fd_add(input->fd,
                                     G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                     input_ready, input);
    egp_error_clear(error);
    return input;
}

void egp_input_free(EgpInput *input)
{
    if (!input)
        return;
    if (input->hold_source)
        g_source_remove(input->hold_source);
    if (input->fd_source)
        g_source_remove(input->fd_source);
    if (input->fd >= 0)
        close(input->fd);
    memset(input, 0, sizeof(*input));
    g_free(input);
}
