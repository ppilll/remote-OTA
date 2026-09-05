#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MISC_PATH "/dev/disk/by-partlabel/misc"
#define AB_OFFSET 2048
#define AB_SIZE 32
#define AB_MAX_PRIORITY 15
#define AB_MAX_TRIES 7
#define AB_MAJOR 1

struct __attribute__((packed)) slot_data {
    uint8_t priority;
    uint8_t tries_remaining;
    uint8_t successful_boot;
    uint8_t flags;        /* Rockchip: bit0 = is_update */
};

struct __attribute__((packed)) ab_data {
    uint8_t magic[4];
    uint8_t version_major;
    uint8_t version_minor;
    uint8_t reserved1[2];
    struct slot_data slots[2];
    uint8_t last_boot;
    uint8_t reserved2[11];
    uint8_t crc_be[4];
};

_Static_assert(sizeof(struct ab_data) == 32,
               "AvbABData must be exactly 32 bytes");

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;

    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^
                  ((crc & 1u) ? 0xedb88320u : 0u);
    }

    return ~crc;
}

static uint32_t load_be32(const uint8_t p[4])
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
            (uint32_t)p[3];
}

static void store_be32(uint8_t p[4], uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static int is_bootable(const struct slot_data *s)
{
    return s->priority > 0 &&
           (s->successful_boot || s->tries_remaining > 0);
}

static int validate(const struct ab_data *d)
{
    static const uint8_t magic[4] = {0, 'A', 'B', '0'};

    if (memcmp(d->magic, magic, 4) != 0) {
        fprintf(stderr, "invalid A/B magic\n");
        return -1;
    }

    if (d->version_major != AB_MAJOR) {
        fprintf(stderr, "unsupported A/B major version: %u\n",
                d->version_major);
        return -1;
    }

    uint32_t disk_crc = load_be32(d->crc_be);
    uint32_t calc_crc = crc32_ieee((const uint8_t *)d, 28);

    if (disk_crc != calc_crc) {
        fprintf(stderr,
                "CRC mismatch: disk=%08x calculated=%08x\n",
                disk_crc, calc_crc);
        return -1;
    }

    for (int i = 0; i < 2; i++) {
        if (d->slots[i].priority > AB_MAX_PRIORITY ||
            d->slots[i].tries_remaining > AB_MAX_TRIES ||
            d->slots[i].successful_boot > 1) {
            fprintf(stderr, "slot %c contains invalid fields\n",
                    'a' + i);
            return -1;
        }
    }

    if (d->last_boot > 1) {
        fprintf(stderr, "invalid last_boot=%u\n", d->last_boot);
        return -1;
    }

    return 0;
}

static int read_data(int rw, int *fd_out, struct ab_data *d)
{
    int flags = rw ? O_RDWR : O_RDONLY;
    int fd = open(MISC_PATH, flags | O_CLOEXEC);

    if (fd < 0) {
        perror("open misc");
        return -1;
    }

    ssize_t n = pread(fd, d, sizeof(*d), AB_OFFSET);

    if (n != sizeof(*d)) {
        if (n < 0)
            perror("pread misc");
        else
            fprintf(stderr, "short metadata read: %zd\n", n);
        close(fd);
        return -1;
    }

    if (validate(d)) {
        close(fd);
        return -1;
    }

    if (fd_out)
        *fd_out = fd;
    else
        close(fd);

    return 0;
}

static int write_data(int fd, struct ab_data *d)
{
    uint32_t crc = crc32_ieee((const uint8_t *)d, 28);
    store_be32(d->crc_be, crc);

    ssize_t n = pwrite(fd, d, sizeof(*d), AB_OFFSET);

    if (n != sizeof(*d)) {
        if (n < 0)
            perror("pwrite misc");
        else
            fprintf(stderr, "short metadata write: %zd\n", n);
        return -1;
    }

    if (fsync(fd)) {
        perror("fsync misc");
        return -1;
    }

    struct ab_data verify;

    n = pread(fd, &verify, sizeof(verify), AB_OFFSET);

    if (n != sizeof(verify)) {
        fprintf(stderr, "metadata reread failed\n");
        return -1;
    }

    if (validate(&verify) ||
        memcmp(&verify, d, sizeof(verify)) != 0) {
        fprintf(stderr, "post-write verification failed\n");
        return -1;
    }

    return 0;
}

static int current_slot(void)
{
    FILE *f = fopen("/proc/cmdline", "r");
    char buf[8192];

    if (!f)
        return -1;

    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* Current Rockchip BSP spelling */
    if (strstr(buf, "android_slotsufix=_a"))
        return 0;
    if (strstr(buf, "android_slotsufix=_b"))
        return 1;

    /* Optional compatibility */
    if (strstr(buf, "androidboot.slot_suffix=_a"))
        return 0;
    if (strstr(buf, "androidboot.slot_suffix=_b"))
        return 1;

    return -1;
}

static int primary_slot(const struct ab_data *d)
{
    int a = is_bootable(&d->slots[0]);
    int b = is_bootable(&d->slots[1]);

    if (!a && !b)
        return -1;
    if (a && !b)
        return 0;
    if (!a && b)
        return 1;

    /*
     * Match Rockchip selector:
     * B only wins if its priority is strictly greater.
     * Tie -> A.
     */
    return d->slots[1].priority > d->slots[0].priority ? 1 : 0;
}

static int parse_slot(const char *s)
{
    if (!strcmp(s, "a") || !strcmp(s, "_a"))
        return 0;
    if (!strcmp(s, "b") || !strcmp(s, "_b"))
        return 1;
    if (!strcmp(s, "current"))
        return current_slot();

    return -1;
}

static void print_status(const struct ab_data *d)
{
    int cur = current_slot();
    int pri = primary_slot(d);

    printf("format=Rockchip-AvbABData "
           "offset=%d size=%d version=%u.%u crc=ok\n",
           AB_OFFSET, AB_SIZE,
           d->version_major, d->version_minor);

    printf("current=%s primary=%s last_boot=%c\n",
           cur < 0 ? "unknown" : (cur ? "b" : "a"),
           pri < 0 ? "none" : (pri ? "b" : "a"),
           d->last_boot ? 'b' : 'a');

    for (int i = 0; i < 2; i++) {
        const struct slot_data *s = &d->slots[i];

        printf("slot=%c priority=%u tries=%u "
               "successful=%u is_update=%u "
               "bootable=%s flags=0x%02x\n",
               'a' + i,
               s->priority,
               s->tries_remaining,
               s->successful_boot,
               s->flags & 1u,
               is_bootable(s) ? "yes" : "no",
               s->flags);
    }
}

static int mutate(const char *op, int slot)
{
    struct ab_data d;
    int fd = -1;
    int other = 1 - slot;

    if (read_data(1, &fd, &d))
        return 1;

    struct slot_data *s = &d.slots[slot];

    if (!strcmp(op, "set-primary")) {
        /*
         * Mirror Rockchip avb_ab_mark_slot_active().
         */
        s->priority = AB_MAX_PRIORITY;
        s->tries_remaining = AB_MAX_TRIES;
        s->successful_boot = 0;

        if (d.slots[other].priority == AB_MAX_PRIORITY)
            d.slots[other].priority =
                AB_MAX_PRIORITY - 1;

    } else if (!strcmp(op, "mark-good")) {
        if (!is_bootable(s)) {
            fprintf(stderr,
                    "refusing to mark unbootable slot %c good\n",
                    'a' + slot);
            close(fd);
            return 1;
        }

        /*
         * Rockchip successful-boot mode.
         */
        s->priority = AB_MAX_PRIORITY;
        s->tries_remaining = 0;
        s->successful_boot = 1;
        s->flags &= (uint8_t)~1u;   /* is_update = 0 */
        d.last_boot = slot;

    } else {
        close(fd);
        return 1;
    }

    if (write_data(fd, &d)) {
        close(fd);
        return 1;
    }

    close(fd);
    print_status(&d);
    return 0;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage:\n"
        "  %s status\n"
        "  %s get-current\n"
        "  %s get-primary\n"
        "  %s set-primary a|b\n"
        "  %s mark-good a|b|current\n",
        p, p, p, p, p);
}

int main(int argc, char **argv)
{
    struct ab_data d;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "get-current")) {
        int s = current_slot();

        if (s < 0) {
            fprintf(stderr, "current slot unknown\n");
            return 1;
        }

        puts(s ? "b" : "a");
        return 0;
    }

    if (!strcmp(argv[1], "status") ||
        !strcmp(argv[1], "get-primary")) {

        if (read_data(0, NULL, &d))
            return 1;

        if (!strcmp(argv[1], "status")) {
            print_status(&d);
            return 0;
        }

        int s = primary_slot(&d);

        if (s < 0) {
            fprintf(stderr, "no bootable slot\n");
            return 1;
        }

        puts(s ? "b" : "a");
        return 0;
    }

    if ((!strcmp(argv[1], "set-primary") ||
         !strcmp(argv[1], "mark-good")) &&
        argc == 3) {

        int s = parse_slot(argv[2]);

        if (s < 0) {
            fprintf(stderr, "invalid slot\n");
            return 2;
        }

        if (!strcmp(argv[1], "mark-good")) {
            int cur = current_slot();

            if (cur < 0 || cur != s) {
                fprintf(stderr,
                        "refusing mark-good: slot %c "
                        "is not the currently booted slot\n",
                        'a' + s);
                return 1;
            }
        }

        return mutate(argv[1], s);
    }

    usage(argv[0]);
    return 2;
}
