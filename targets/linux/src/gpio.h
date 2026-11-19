/* GPIO character device helpers (Linux only), see gpio.c. Offsets are line numbers on the chip, BCM GPIO numbers on a Raspberry Pi. */
#ifndef NP_GPIO_H
#define NP_GPIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Open `path`, or when it is empty the chip behind the Raspberry Pi header. `found` names the chip used. Returns an fd or -1. */
int gpio_open_chip(const char *path, char *found, size_t found_cap);
/* Request output lines; bit i of initial_bits is the starting level of offsets[i]. Returns a line fd or -1. */
int gpio_request_outputs(int chip, const unsigned *offsets, int count, uint64_t initial_bits, const char *consumer);
int gpio_request_input(int chip, unsigned offset, bool pull_up, const char *consumer);
/* Inputs with pull-up and falling edge events, for buttons to GND. */
int gpio_request_buttons(int chip, const unsigned *offsets, int count, uint32_t debounce_ms, const char *consumer);
/* Bits and masks are indexed like the offsets array of the request. */
int gpio_set(int line_fd, uint64_t mask, uint64_t bits);
int gpio_get(int line_fd, uint64_t mask, uint64_t *bits);
/* Read one edge event. Returns 1 for a falling edge, 0 for another event, -1 on error. */
int gpio_read_falling(int line_fd, unsigned *offset, uint64_t *timestamp_ns);

#endif
