#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MISC_PATH "/dev/disk/by-partlabel/misc"

#define AB_OFFSET 2048
#define AB_SIZE 32
#define AB_MAX_PRIORITY 15
#define AB_MAX_TRIES 7
#define AB_MAJOR 1
#define AB_MINOR 0

#define LOCK_DIR "/run/lock"
#define LOCK_PATH "/run/lock/edgeguard-rk-abctl.lock"

#define CMDLINE_PATH "/proc/cmdline"
#define MOUNTINFO_PATH "/proc/self/mountinfo"

#define PATHBUF 4096

struct __attribute__((packed)) slot_data {
    uint8_t priority;
    uint8_t tries_remaining;
    uint8_t successful_boot;
    uint8_t flags;
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

_Static_assert(sizeof(struct ab_data) == AB_SIZE,
               "AvbABData must be exactly 32 bytes");


static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;

    while (n--) {
        crc ^= *p++;

        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^
                  ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }

    return ~crc;
}


static uint32_t load_be32(const uint8_t p[4])
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
            (uint32_t)p[3];
}


static void store_be32(uint8_t p[4], uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}


static int is_bootable(const struct slot_data *s)
{
    return s->priority > 0 &&
           (s->successful_boot != 0 ||
            s->tries_remaining > 0);
}


static int validate(const struct ab_data *d)
{
    static const uint8_t magic[4] = {
        0x00, 'A', 'B', '0'
    };

    uint32_t stored_crc;
    uint32_t calculated_crc;

    if (memcmp(d->magic, magic, sizeof(magic)) != 0) {
        fprintf(stderr,
                "edgeguard-rk-abctl: invalid A/B magic\n");
        return -1;
    }

    if (d->version_major != AB_MAJOR ||
        d->version_minor != AB_MINOR) {

        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "unsupported A/B version %u.%u "
                "(expected %u.%u)\n",
                d->version_major,
                d->version_minor,
                AB_MAJOR,
                AB_MINOR);

        return -1;
    }

    stored_crc = load_be32(d->crc_be);

    calculated_crc =
        crc32_ieee((const uint8_t *)d, 28);

    if (stored_crc != calculated_crc) {
        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "CRC mismatch: disk=%08x calculated=%08x\n",
                stored_crc,
                calculated_crc);

        return -1;
    }

    for (int i = 0; i < 2; i++) {

        if (d->slots[i].priority > AB_MAX_PRIORITY ||
            d->slots[i].tries_remaining > AB_MAX_TRIES ||
            d->slots[i].successful_boot > 1) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "slot %c contains invalid fields\n",
                    'a' + i);

            return -1;
        }
    }

    if (d->last_boot > 1) {
        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "invalid last_boot=%u\n",
                d->last_boot);

        return -1;
    }

    return 0;
}


static int read_exact(int fd, struct ab_data *d)
{
    ssize_t n;

    n = pread(fd,
              d,
              sizeof(*d),
              AB_OFFSET);

    if (n < 0) {
        perror("edgeguard-rk-abctl: pread misc");
        return -1;
    }

    if (n != (ssize_t)sizeof(*d)) {
        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "short metadata read: %zd\n",
                n);

        return -1;
    }

    return 0;
}


static int open_and_read(int rw,
                         int *fd_out,
                         struct ab_data *d)
{
    int flags;
    int fd;

    flags = rw ? O_RDWR : O_RDONLY;

    fd = open(MISC_PATH,
              flags | O_CLOEXEC);

    if (fd < 0) {
        perror("edgeguard-rk-abctl: open misc");
        return -1;
    }

    if (read_exact(fd, d) != 0) {
        close(fd);
        return -1;
    }

    if (validate(d) != 0) {
        close(fd);
        return -1;
    }

    if (fd_out)
        *fd_out = fd;
    else
        close(fd);

    return 0;
}


/*
 * This is the single Linux-side writer lock.
 *
 * Every Native A/B metadata mutation must pass
 * through this lock.
 */
static int acquire_writer_lock(void)
{
    struct flock lk;
    int fd;

    if (mkdir(LOCK_DIR, 0755) < 0 &&
        errno != EEXIST) {

        perror("edgeguard-rk-abctl: mkdir /run/lock");
        return -1;
    }

    fd = open(LOCK_PATH,
              O_RDWR |
              O_CREAT |
              O_CLOEXEC,
              0600);

    if (fd < 0) {
        perror("edgeguard-rk-abctl: open writer lock");
        return -1;
    }

    memset(&lk, 0, sizeof(lk));

    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;

    if (fcntl(fd, F_SETLKW, &lk) < 0) {

        perror("edgeguard-rk-abctl: "
               "fcntl writer lock");

        close(fd);
        return -1;
    }

    return fd;
}


static int write_and_verify(int fd,
                            struct ab_data *d)
{
    struct ab_data verify;
    uint32_t crc;
    ssize_t n;

    /*
     * CRC covers first 28 bytes.
     */
    crc = crc32_ieee(
        (const uint8_t *)d,
        28);

    store_be32(d->crc_be, crc);

    n = pwrite(fd,
               d,
               sizeof(*d),
               AB_OFFSET);

    if (n < 0) {
        perror("edgeguard-rk-abctl: pwrite misc");
        return -1;
    }

    if (n != (ssize_t)sizeof(*d)) {
        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "short metadata write: %zd\n",
                n);

        return -1;
    }

    if (fsync(fd) < 0) {
        perror("edgeguard-rk-abctl: fsync misc");
        return -1;
    }

    /*
     * Mandatory read-back.
     */
    if (read_exact(fd, &verify) != 0)
        return -1;

    if (validate(&verify) != 0)
        return -1;

    if (memcmp(&verify,
               d,
               sizeof(verify)) != 0) {

        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "post-write read-back mismatch\n");

        return -1;
    }

    return 0;
}


/*
 * Parse the two authoritative kernel command-line
 * inputs used by EdgeGuard:
 *
 *   android_slotsufix=_a|_b
 *   root=PARTUUID=...
 *
 * Duplicate entries are rejected.
 */
static int read_cmdline_identity(
    char *suffix,
    size_t suffix_size,
    char *partuuid,
    size_t partuuid_size)
{
    static const char suffix_key[] =
        "android_slotsufix=";

    static const char root_key[] =
        "root=PARTUUID=";

    char buf[8192];
    char *save = NULL;
    char *tok;

    size_t n;

    int suffix_seen = 0;
    int root_seen = 0;

    FILE *f;

    f = fopen(CMDLINE_PATH, "r");

    if (!f) {
        perror("edgeguard-rk-abctl: "
               "fopen /proc/cmdline");
        return -1;
    }

    n = fread(buf,
              1,
              sizeof(buf) - 1,
              f);

    if (ferror(f)) {
        perror("edgeguard-rk-abctl: "
               "fread /proc/cmdline");

        fclose(f);
        return -1;
    }

    fclose(f);

    if (n == sizeof(buf) - 1) {
        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "/proc/cmdline too long\n");

        return -1;
    }

    buf[n] = '\0';

    for (tok = strtok_r(buf,
                        " \t\r\n",
                        &save);
         tok;
         tok = strtok_r(NULL,
                        " \t\r\n",
                        &save)) {

        if (strncmp(tok,
                    suffix_key,
                    sizeof(suffix_key) - 1) == 0) {

            const char *value =
                tok + sizeof(suffix_key) - 1;

            suffix_seen++;

            if (suffix_seen != 1 ||
                *value == '\0' ||
                strlen(value) >= suffix_size) {

                fprintf(stderr,
                        "edgeguard-rk-abctl: "
                        "invalid/duplicate "
                        "android_slotsufix\n");

                return -1;
            }

            strcpy(suffix, value);
        }

        else if (strncmp(tok,
                         root_key,
                         sizeof(root_key) - 1) == 0) {

            const char *value =
                tok + sizeof(root_key) - 1;

            root_seen++;

            if (root_seen != 1 ||
                *value == '\0' ||
                strlen(value) >= partuuid_size) {

                fprintf(stderr,
                        "edgeguard-rk-abctl: "
                        "invalid/duplicate "
                        "root=PARTUUID\n");

                return -1;
            }

            strcpy(partuuid, value);
        }
    }

    if (suffix_seen != 1) {
        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "android_slotsufix missing\n");

        return -1;
    }

    if (root_seen != 1) {
        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "root=PARTUUID missing\n");

        return -1;
    }

    return 0;
}


static int resolve_path(const char *path,
                        char out[PATHBUF])
{
    if (!realpath(path, out)) {

        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "cannot resolve %s: %s\n",
                path,
                strerror(errno));

        return -1;
    }

    return 0;
}


/*
 * Resolve the actual block device mounted as "/".
 *
 * mountinfo field 3 is major:minor.
 * /sys/dev/block/<major:minor> resolves to the
 * actual kernel block device.
 */
static int root_mount_device(
    char out[PATHBUF])
{
    char line[8192];

    char major_minor[64];
    char root[PATHBUF];
    char mountpoint[PATHBUF];

    char sys_path[PATHBUF];
    char sys_resolved[PATHBUF];

    char dev_path[PATHBUF];

    const char *base;

    FILE *f;

    f = fopen(MOUNTINFO_PATH, "r");

    if (!f) {
        perror("edgeguard-rk-abctl: "
               "fopen mountinfo");

        return -1;
    }

    while (fgets(line,
                 sizeof(line),
                 f)) {

        if (sscanf(line,
                   "%*u %*u "
                   "%63s "
                   "%4095s "
                   "%4095s",
                   major_minor,
                   root,
                   mountpoint) != 3) {

            continue;
        }

        if (strcmp(mountpoint, "/") != 0)
            continue;

        fclose(f);

        if (snprintf(sys_path,
                     sizeof(sys_path),
                     "/sys/dev/block/%s",
                     major_minor) >=
            (int)sizeof(sys_path)) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "root mount sysfs path too long\n");

            return -1;
        }

        if (resolve_path(sys_path,
                         sys_resolved) != 0) {

            return -1;
        }

        base = strrchr(sys_resolved, '/');

        if (!base || !base[1]) {
            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "invalid root mount sysfs target %s\n",
                    sys_resolved);

            return -1;
        }

        base++;

        if (snprintf(dev_path,
                     sizeof(dev_path),
                     "/dev/%s",
                     base) >=
            (int)sizeof(dev_path)) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "root mount device path too long\n");

            return -1;
        }

        return resolve_path(dev_path, out);
    }

    fclose(f);

    fprintf(stderr,
            "edgeguard-rk-abctl: "
            "cannot determine root mount block device\n");

    return -1;
}


/*
 * Frozen EdgeGuard current-slot identity rule:
 *
 * android_slotsufix
 *       +
 * root=PARTUUID
 *       +
 * /dev/disk/by-partuuid resolution
 *       +
 * actual root mount block device
 *
 * All four identities must agree.
 */
static int current_slot_verified(void)
{
    char suffix[16] = {0};
    char partuuid[128] = {0};

    char partuuid_path[PATHBUF];
    char expected_path[PATHBUF];

    char rootdev[PATHBUF];
    char expected_dev[PATHBUF];
    char mountdev[PATHBUF];

    int slot;

    if (read_cmdline_identity(
            suffix,
            sizeof(suffix),
            partuuid,
            sizeof(partuuid)) != 0) {

        return -1;
    }

    /*
     * Preserve the vendor spelling exactly:
     *
     * android_slotsufix
     */
    if (strcmp(suffix, "_a") == 0) {
        slot = 0;
    }
    else if (strcmp(suffix, "_b") == 0) {
        slot = 1;
    }
    else {
        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "invalid android_slotsufix '%s'\n",
                suffix);

        return -1;
    }

    if (snprintf(partuuid_path,
                 sizeof(partuuid_path),
                 "/dev/disk/by-partuuid/%s",
                 partuuid) >=
        (int)sizeof(partuuid_path)) {

        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "PARTUUID path too long\n");

        return -1;
    }

    if (snprintf(expected_path,
                 sizeof(expected_path),
                 "/dev/disk/by-partlabel/system_%c",
                 'a' + slot) >=
        (int)sizeof(expected_path)) {

        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "expected system path too long\n");

        return -1;
    }

    if (resolve_path(partuuid_path,
                     rootdev) != 0) {

        return -1;
    }

    if (resolve_path(expected_path,
                     expected_dev) != 0) {

        return -1;
    }

    if (root_mount_device(mountdev) != 0)
        return -1;

    if (strcmp(rootdev,
               expected_dev) != 0) {

        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "root PARTUUID mismatch: "
                "suffix=%s "
                "rootdev=%s "
                "expected=%s\n",
                suffix,
                rootdev,
                expected_dev);

        return -1;
    }

    if (strcmp(mountdev,
               expected_dev) != 0) {

        fprintf(stderr,
                "edgeguard-rk-abctl: "
                "root mount mismatch: "
                "suffix=%s "
                "mountdev=%s "
                "expected=%s\n",
                suffix,
                mountdev,
                expected_dev);

        return -1;
    }

    return slot;
}


static int primary_slot(
    const struct ab_data *d)
{
    int a;
    int b;

    a = is_bootable(&d->slots[0]);
    b = is_bootable(&d->slots[1]);

    if (!a && !b)
        return -1;

    if (a && !b)
        return 0;

    if (!a && b)
        return 1;

    /*
     * Match the already-verified Rockchip selector:
     *
     * B wins only when strictly higher priority.
     * Tie => A.
     */
    return d->slots[1].priority >
           d->slots[0].priority
           ? 1 : 0;
}


static int parse_slot(const char *s)
{
    if (!strcmp(s, "a") ||
        !strcmp(s, "_a")) {

        return 0;
    }

    if (!strcmp(s, "b") ||
        !strcmp(s, "_b")) {

        return 1;
    }

    return -1;
}


/*
 * Native 3-state model.
 */
static const char *native_state(
    const struct slot_data *s)
{
    /*
     * confirmed-good
     */
    if (s->priority > 0 &&
        s->tries_remaining == 0 &&
        s->successful_boot == 1 &&
        is_bootable(s)) {

        return "confirmed-good";
    }

    /*
     * pending
     */
    if (s->priority > 0 &&
        s->tries_remaining > 0 &&
        s->successful_boot == 0 &&
        is_bootable(s)) {

        return "pending";
    }

    /*
     * canonical bad
     */
    if (s->priority == 0 &&
        s->tries_remaining == 0 &&
        s->successful_boot == 0 &&
        !is_bootable(s)) {

        return "bad";
    }

    return NULL;
}


static void print_status(
    const struct ab_data *d,
    int current)
{
    int primary;

    primary = primary_slot(d);

    printf("format=Rockchip-AvbABData "
           "offset=%d "
           "size=%d "
           "version=%u.%u "
           "crc=ok\n",
           AB_OFFSET,
           AB_SIZE,
           d->version_major,
           d->version_minor);

    printf("current=%s "
           "primary=%s "
           "last_boot=%c\n",
           current < 0
               ? "unknown"
               : (current ? "b" : "a"),

           primary < 0
               ? "none"
               : (primary ? "b" : "a"),

           d->last_boot
               ? 'b'
               : 'a');

    for (int i = 0; i < 2; i++) {

        const struct slot_data *s;
        const char *state;

        s = &d->slots[i];
        state = native_state(s);

        printf("slot=%c "
               "priority=%u "
               "tries=%u "
               "successful=%u "
               "is_update=%u "
               "bootable=%s "
               "flags=0x%02x "
               "native=%s\n",

               'a' + i,

               s->priority,
               s->tries_remaining,
               s->successful_boot,

               s->flags & 1u,

               is_bootable(s)
                   ? "yes"
                   : "no",

               s->flags,

               state
                   ? state
                   : "inconsistent");
    }
}


enum mutation {
    MUT_SET_PRIMARY,
    MUT_MARK_BAD,
    MUT_MARK_GOOD
};


/*
 * The only metadata mutation path.
 */
static int mutate(
    enum mutation op,
    int slot)
{
    struct ab_data d;

    struct slot_data *s;

    int other;
    int current;

    int lockfd = -1;
    int fd = -1;

    int rc = 1;

    other = 1 - slot;

    /*
     * Lock first, then read/validate/mutate/write.
     */
    lockfd = acquire_writer_lock();

    if (lockfd < 0)
        return 1;

    /*
     * FAIL CLOSED:
     *
     * no Linux-side metadata mutation is allowed
     * while current slot identity is inconsistent.
     */
    current = current_slot_verified();

    if (current < 0)
        goto out;

    if (open_and_read(1,
                      &fd,
                      &d) != 0) {

        goto out;
    }

    s = &d.slots[slot];

    switch (op) {

    case MUT_SET_PRIMARY:

        /*
         * Native pending state:
         *
         * priority=15
         * tries=7
         * successful=0
         */
        s->priority =
            AB_MAX_PRIORITY;

        s->tries_remaining =
            AB_MAX_TRIES;

        s->successful_boot = 0;

        /*
         * Match the verified Rockchip behavior.
         */
        if (d.slots[other].priority ==
            AB_MAX_PRIORITY) {

            d.slots[other].priority =
                AB_MAX_PRIORITY - 1;
        }

        break;


    case MUT_MARK_BAD:

        /*
         * Never intentionally leave the board
         * with zero bootable slots.
         */
        if (!is_bootable(
                &d.slots[other])) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "refusing mark-bad: "
                    "peer slot %c is not bootable\n",
                    'a' + other);

            goto out;
        }

        /*
         * Canonical bad is idempotent.
         */
        if (s->priority == 0 &&
            s->tries_remaining == 0 &&
            s->successful_boot == 0) {

            print_status(&d, current);

            rc = 0;
            goto out;
        }

        s->priority = 0;
        s->tries_remaining = 0;
        s->successful_boot = 0;

        /*
         * Preserve flags and all unrelated bytes.
         */
        break;


    case MUT_MARK_GOOD:

        /*
         * Good is only legal for rigorously
         * verified current slot.
         */
        if (slot != current) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "refusing mark-good: "
                    "slot %c is not current "
                    "(current=%c)\n",
                    'a' + slot,
                    'a' + current);

            goto out;
        }

        if (!is_bootable(s)) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "refusing to mark unbootable "
                    "slot %c good\n",
                    'a' + slot);

            goto out;
        }

        /*
         * confirmed-good
         */
        s->priority =
            AB_MAX_PRIORITY;

        s->tries_remaining = 0;
        s->successful_boot = 1;

        /*
         * Rockchip successful-boot behavior.
         */
        s->flags &=
            (uint8_t)~1u;

        d.last_boot =
            (uint8_t)slot;

        break;
    }

    if (write_and_verify(fd,
                         &d) != 0) {

        goto out;
    }

    print_status(&d, current);

    rc = 0;

out:

    if (fd >= 0)
        close(fd);

    close(lockfd);

    return rc;
}


static void usage(const char *prog)
{
    fprintf(stderr,
            "usage:\n"
            "  %s status\n"
            "  %s get-current\n"
            "  %s get-primary\n"
            "  %s get-native-state a|b\n"
            "  %s set-primary a|b\n"
            "  %s mark-bad a|b\n"
            "  %s mark-good a|b|current\n",
            prog,
            prog,
            prog,
            prog,
            prog,
            prog,
            prog);
}


int main(int argc, char **argv)
{
    struct ab_data d;

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }


    /*
     * Rigorous verified current slot.
     */
    if (!strcmp(argv[1], "get-current") &&
        argc == 2) {

        int slot;

        slot = current_slot_verified();

        if (slot < 0)
            return 1;

        puts(slot ? "b" : "a");
        return 0;
    }


    /*
     * Diagnostic status.
     *
     * Metadata is still printed when current
     * identity is inconsistent, but command
     * returns non-zero.
     */
    if (!strcmp(argv[1], "status") &&
        argc == 2) {

        int current;

        if (open_and_read(0,
                          NULL,
                          &d) != 0) {

            return 1;
        }

        current =
            current_slot_verified();

        print_status(&d, current);

        return current < 0
            ? 1
            : 0;
    }


    if (!strcmp(argv[1], "get-primary") &&
        argc == 2) {

        int slot;

        if (open_and_read(0,
                          NULL,
                          &d) != 0) {

            return 1;
        }

        slot = primary_slot(&d);

        if (slot < 0) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "no bootable primary slot\n");

            return 1;
        }

        puts(slot ? "b" : "a");
        return 0;
    }


    /*
     * Native 3-state query for the RAUC adapter.
     *
     * stdout is exactly one of:
     *
     *   confirmed-good
     *   pending
     *   bad
     */
    if (!strcmp(argv[1],
                "get-native-state") &&
        argc == 3) {

        int slot;
        const char *state;

        slot = parse_slot(argv[2]);

        if (slot < 0) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "invalid slot\n");

            return 2;
        }

        if (open_and_read(0,
                          NULL,
                          &d) != 0) {

            return 1;
        }

        state =
            native_state(&d.slots[slot]);

        if (!state) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "inconsistent native state "
                    "for slot %c\n",
                    'a' + slot);

            return 1;
        }

        puts(state);
        return 0;
    }


    if (!strcmp(argv[1],
                "set-primary") &&
        argc == 3) {

        int slot;

        slot = parse_slot(argv[2]);

        if (slot < 0) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "invalid slot\n");

            return 2;
        }

        return mutate(
            MUT_SET_PRIMARY,
            slot);
    }


    if (!strcmp(argv[1],
                "mark-bad") &&
        argc == 3) {

        int slot;

        slot = parse_slot(argv[2]);

        if (slot < 0) {

            fprintf(stderr,
                    "edgeguard-rk-abctl: "
                    "invalid slot\n");

            return 2;
        }

        return mutate(
            MUT_MARK_BAD,
            slot);
    }


    if (!strcmp(argv[1],
                "mark-good") &&
        argc == 3) {

        int slot;

        if (!strcmp(argv[2],
                    "current")) {

            slot =
                current_slot_verified();

            if (slot < 0)
                return 1;
        }
        else {

            slot =
                parse_slot(argv[2]);

            if (slot < 0) {

                fprintf(stderr,
                        "edgeguard-rk-abctl: "
                        "invalid slot\n");

                return 2;
            }
        }

        return mutate(
            MUT_MARK_GOOD,
            slot);
    }


    usage(argv[0]);
    return 2;
}