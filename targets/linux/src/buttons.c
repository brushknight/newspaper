/*
 * Buttons. On the Raspberry Pi: GPIO lines with pull-up and falling edge
 * events (each button connects its line to GND, as on the Inky Impression).
 * The kernel debounces where it can; presses closer together than the
 * debounce time are dropped here as well.
 *
 * In no-hardware mode the keyboard stands in: type n (Next), b (Back) or
 * h (Home) and Enter on the program's standard input.
 *
 * buttons_wait is the program's idle: it blocks in poll() until a press,
 * a stop request or the timeout. Where the ESP32 deep-sleeps, this sleeps.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#ifdef NP_HARDWARE
#include "gpio.h"
#endif

#define QUEUE 16

static np_button_t queue[QUEUE];
static int queue_len;
static int line_fd = -1;  /* button events (hardware) */
static int stdin_fd = -1; /* key presses (no-hardware mode) */
#ifdef NP_HARDWARE
static int home_index = -1;
static unsigned offsets[3];
static np_button_t ids[3];
static int count;
static uint64_t last_ns[3];
static uint64_t debounce_ns;
#endif

static void push(np_button_t b)
{
    if (queue_len < QUEUE) {
        queue[queue_len++] = b;
    }
}

int buttons_init(const np_linux_config_t *cfg)
{
    queue_len = 0;
    if (cfg->no_hardware) {
        stdin_fd = STDIN_FILENO;
        np_log(NP_LOG_INFO, "buttons: type n (Next), b (Back) or h (Home) and Enter");
        return 0;
    }
#ifdef NP_HARDWARE
    const int pins[3] = { cfg->button_next, cfg->button_back, cfg->button_home };
    const np_button_t names[3] = { NP_BTN_NEXT, NP_BTN_BACK, NP_BTN_HOME };
    count = 0;
    for (int i = 0; i < 3; i++) {
        if (pins[i] >= 0) {
            if (names[i] == NP_BTN_HOME) {
                home_index = count;
            }
            offsets[count] = (unsigned)pins[i];
            ids[count] = names[i];
            count++;
        }
    }
    debounce_ns = (uint64_t)cfg->debounce_ms * 1000000u;
    if (count == 0) {
        return 0;
    }
    char chip_name[96];
    int chip = gpio_open_chip(cfg->gpiochip, chip_name, sizeof(chip_name));
    if (chip < 0) {
        np_log(NP_LOG_ERROR, "buttons: cannot open the GPIO chip: %s", strerror(errno));
        return -1;
    }
    line_fd = gpio_request_buttons(chip, offsets, count, cfg->debounce_ms, "newspaper-buttons");
    int err = errno;
    close(chip); /* the line request stays valid on its own */
    if (line_fd < 0) {
        np_log(NP_LOG_ERROR, "buttons: cannot claim the button lines: %s", strerror(err));
        return -1;
    }
    np_log(NP_LOG_INFO, "buttons: Next GPIO %d, Back GPIO %d, Home GPIO %d", cfg->button_next, cfg->button_back,
           cfg->button_home);
    return 0;
#else
    np_log(NP_LOG_ERROR, "buttons: built without hardware support; use --no-hardware");
    return -1;
#endif
}

static void read_events(void)
{
#ifdef NP_HARDWARE
    for (;;) {
        struct pollfd p = { line_fd, POLLIN, 0 };
        if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) {
            return;
        }
        unsigned offset;
        uint64_t ts;
        int rc = gpio_read_falling(line_fd, &offset, &ts);
        if (rc < 0) {
            return;
        }
        if (rc == 0) {
            continue;
        }
        for (int i = 0; i < count; i++) {
            if (offsets[i] != offset) {
                continue;
            }
            if (last_ns[i] && ts - last_ns[i] < debounce_ns) {
                break; /* bounce */
            }
            last_ns[i] = ts;
            push(ids[i]);
            break;
        }
    }
#endif
}

static void read_keys(void)
{
    for (;;) {
        struct pollfd p = { stdin_fd, POLLIN, 0 };
        if (poll(&p, 1, 0) <= 0) {
            return;
        }
        char buf[64];
        ssize_t n = read(stdin_fd, buf, sizeof(buf));
        if (n <= 0) {
            stdin_fd = -1; /* end of input (for example /dev/null under a service manager) */
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            switch (buf[i]) {
            case 'n': case 'N': push(NP_BTN_NEXT); break;
            case 'b': case 'B': push(NP_BTN_BACK); break;
            case 'h': case 'H': push(NP_BTN_HOME); break;
            default: break;
            }
        }
    }
}

static void collect(void)
{
    if (line_fd >= 0) {
        read_events();
    }
    if (stdin_fd >= 0) {
        read_keys();
    }
}

np_button_t buttons_poll(void *ctx)
{
    collect();
    if (queue_len == 0) {
        return NP_BTN_NONE;
    }
    np_button_t b = queue[0];
    memmove(queue, queue + 1, (size_t)(queue_len - 1) * sizeof(queue[0]));
    queue_len--;
    return b;
}

void buttons_wait(uint32_t ms)
{
    collect();
    if (queue_len > 0 || np_stop_requested()) {
        return;
    }
    struct pollfd p[3];
    int n = 0;
    p[n++] = (struct pollfd){ np_stop_fd(), POLLIN, 0 };
    if (line_fd >= 0) {
        p[n++] = (struct pollfd){ line_fd, POLLIN, 0 };
    }
    if (stdin_fd >= 0) {
        p[n++] = (struct pollfd){ stdin_fd, POLLIN, 0 };
    }
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t left = ms;
    for (;;) {
        int rc = poll(p, (nfds_t)n, left > 60000 ? 60000 : (int)left);
        if (rc > 0 || np_stop_requested()) {
            collect();
            if (queue_len > 0 || np_stop_requested() || p[0].revents) {
                return;
            }
        } else if (rc < 0 && errno != EINTR) {
            return;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t spent = (uint64_t)(now.tv_sec - start.tv_sec) * 1000u + (uint64_t)((now.tv_nsec - start.tv_nsec) / 1000000L);
        if (spent >= ms) {
            return;
        }
        left = ms - spent;
        /* Keyboard input may have ended (it is always the last entry): stop watching it. */
        if (stdin_fd < 0 && n > 1 && p[n - 1].fd == STDIN_FILENO && line_fd != STDIN_FILENO) {
            n--;
        }
    }
}

bool buttons_home_held(void)
{
#ifdef NP_HARDWARE
    if (line_fd < 0 || home_index < 0) {
        return false;
    }
    uint64_t bits = ~0ULL;
    if (gpio_get(line_fd, 1ULL << home_index, &bits) != 0) {
        return false;
    }
    return ((bits >> home_index) & 1) == 0; /* pressed pulls the line to GND */
#else
    return false;
#endif
}
