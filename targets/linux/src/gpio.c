/*
 * GPIO through the kernel's character device (/dev/gpiochipN), uAPI v2
 * (Linux 5.10 and newer). No libgpiod: its API changed between 1.x and 2.x
 * and Raspberry Pi OS releases ship different ones.
 *
 * Some cross toolchains ship older kernel headers than the kernel they run
 * on (the Buildroot Bootlin toolchain has 5.4 headers), so the v2 structures
 * are declared here when <linux/gpio.h> does not have them. They are part of
 * the kernel's stable user space ABI (include/uapi/linux/gpio.h).
 */
#include "gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/gpio.h>

#ifndef GPIO_MAX_NAME_SIZE
#define GPIO_MAX_NAME_SIZE 32 /* headers before 5.10 spell it out */
#endif

#ifndef GPIO_V2_GET_LINE_IOCTL
#define GPIO_V2_LINES_MAX 64
#define GPIO_V2_LINE_NUM_ATTRS_MAX 10
#define GPIO_V2_LINE_FLAG_ACTIVE_LOW (1ULL << 1)
#define GPIO_V2_LINE_FLAG_INPUT (1ULL << 2)
#define GPIO_V2_LINE_FLAG_OUTPUT (1ULL << 3)
#define GPIO_V2_LINE_FLAG_EDGE_RISING (1ULL << 4)
#define GPIO_V2_LINE_FLAG_EDGE_FALLING (1ULL << 5)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_UP (1ULL << 8)
#define GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN (1ULL << 9)
#define GPIO_V2_LINE_FLAG_BIAS_DISABLED (1ULL << 10)
#define GPIO_V2_LINE_ATTR_ID_FLAGS 1
#define GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES 2
#define GPIO_V2_LINE_ATTR_ID_DEBOUNCE 3
#define GPIO_V2_LINE_EVENT_RISING_EDGE 1
#define GPIO_V2_LINE_EVENT_FALLING_EDGE 2

struct gpio_v2_line_values {
    __u64 bits __attribute__((aligned(8)));
    __u64 mask __attribute__((aligned(8)));
};

struct gpio_v2_line_attribute {
    __u32 id;
    __u32 padding;
    union {
        __u64 flags __attribute__((aligned(8)));
        __u64 values __attribute__((aligned(8)));
        __u32 debounce_period_us;
    };
};

struct gpio_v2_line_config_attribute {
    struct gpio_v2_line_attribute attr;
    __u64 mask __attribute__((aligned(8)));
};

struct gpio_v2_line_config {
    __u64 flags __attribute__((aligned(8)));
    __u32 num_attrs;
    __u32 padding[5];
    struct gpio_v2_line_config_attribute attrs[GPIO_V2_LINE_NUM_ATTRS_MAX];
};

struct gpio_v2_line_request {
    __u32 offsets[GPIO_V2_LINES_MAX];
    char consumer[GPIO_MAX_NAME_SIZE];
    struct gpio_v2_line_config config;
    __u32 num_lines;
    __u32 event_buffer_size;
    __u32 padding[5];
    __s32 fd;
};

struct gpio_v2_line_event {
    __u64 timestamp_ns __attribute__((aligned(8)));
    __u32 id;
    __u32 offset;
    __u32 seqno;
    __u32 line_seqno;
    __u32 padding[6];
};

#define GPIO_V2_GET_LINE_IOCTL _IOWR(0xB4, 0x07, struct gpio_v2_line_request)
#define GPIO_V2_LINE_GET_VALUES_IOCTL _IOWR(0xB4, 0x0E, struct gpio_v2_line_values)
#define GPIO_V2_LINE_SET_VALUES_IOCTL _IOWR(0xB4, 0x0F, struct gpio_v2_line_values)
#endif

/* Labels of the chip that drives the 40 pin header on Raspberry Pi boards. */
static const char *HEADER_CHIPS[] = { "pinctrl-bcm2835", "pinctrl-bcm2711", "pinctrl-bcm2712", "pinctrl-rp1" };

int gpio_open_chip(const char *path, char *found, size_t found_cap)
{
    if (path && *path) {
        snprintf(found, found_cap, "%s", path);
        return open(path, O_RDWR | O_CLOEXEC);
    }
    int fallback = -1;
    for (int i = 0; i < 16; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/gpiochip%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        struct gpiochip_info info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) {
            for (size_t k = 0; k < sizeof(HEADER_CHIPS) / sizeof(HEADER_CHIPS[0]); k++) {
                if (strcmp(info.label, HEADER_CHIPS[k]) == 0 && info.lines >= 28) {
                    if (fallback >= 0) {
                        close(fallback);
                    }
                    snprintf(found, found_cap, "%s (%s)", p, info.label);
                    return fd;
                }
            }
        }
        if (i == 0 && fallback < 0) {
            fallback = fd;
            snprintf(found, found_cap, "%s (label not recognised)", p);
        } else {
            close(fd);
        }
    }
    if (fallback < 0) {
        errno = ENOENT;
    }
    return fallback;
}

static int request(int chip, const unsigned *offsets, int count, uint64_t flags, uint64_t initial_bits,
                   uint32_t debounce_us, const char *consumer)
{
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof(req));
    for (int i = 0; i < count; i++) {
        req.offsets[i] = offsets[i];
    }
    req.num_lines = (__u32)count;
    snprintf(req.consumer, sizeof(req.consumer), "%s", consumer);
    req.config.flags = flags;
    int a = 0;
    if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
        req.config.attrs[a].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[a].attr.values = initial_bits;
        req.config.attrs[a].mask = (count >= 64) ? ~0ULL : ((1ULL << count) - 1);
        a++;
    }
    if (debounce_us > 0) {
        req.config.attrs[a].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
        req.config.attrs[a].attr.debounce_period_us = debounce_us;
        req.config.attrs[a].mask = (count >= 64) ? ~0ULL : ((1ULL << count) - 1);
        a++;
    }
    req.config.num_attrs = (__u32)a;
    if (flags & (GPIO_V2_LINE_FLAG_EDGE_FALLING | GPIO_V2_LINE_FLAG_EDGE_RISING)) {
        req.event_buffer_size = 16;
    }
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        return -1;
    }
    return req.fd;
}

int gpio_request_outputs(int chip, const unsigned *offsets, int count, uint64_t initial_bits, const char *consumer)
{
    return request(chip, offsets, count, GPIO_V2_LINE_FLAG_OUTPUT, initial_bits, 0, consumer);
}

int gpio_request_input(int chip, unsigned offset, bool pull_up, const char *consumer)
{
    uint64_t flags = GPIO_V2_LINE_FLAG_INPUT | (pull_up ? GPIO_V2_LINE_FLAG_BIAS_PULL_UP : 0);
    return request(chip, &offset, 1, flags, 0, 0, consumer);
}

int gpio_request_buttons(int chip, const unsigned *offsets, int count, uint32_t debounce_ms, const char *consumer)
{
    uint64_t flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP | GPIO_V2_LINE_FLAG_EDGE_FALLING;
    int fd = request(chip, offsets, count, flags, 0, debounce_ms * 1000u, consumer);
    if (fd < 0 && debounce_ms > 0) {
        fd = request(chip, offsets, count, flags, 0, 0, consumer); /* buttons.c also debounces in software */
    }
    return fd;
}

int gpio_set(int line_fd, uint64_t mask, uint64_t bits)
{
    struct gpio_v2_line_values v = { .bits = bits, .mask = mask };
    return ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

int gpio_get(int line_fd, uint64_t mask, uint64_t *bits)
{
    struct gpio_v2_line_values v = { .bits = 0, .mask = mask };
    if (ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) {
        return -1;
    }
    *bits = v.bits;
    return 0;
}

int gpio_read_falling(int line_fd, unsigned *offset, uint64_t *timestamp_ns)
{
    struct gpio_v2_line_event ev;
    ssize_t n = read(line_fd, &ev, sizeof(ev));
    if (n != (ssize_t)sizeof(ev)) {
        return -1;
    }
    *offset = ev.offset;
    *timestamp_ns = ev.timestamp_ns;
    return ev.id == GPIO_V2_LINE_EVENT_FALLING_EDGE ? 1 : 0;
}
