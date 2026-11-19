/*
 * newspaper-wifi: turns newspaper-wifi.txt (boot partition of the image)
 * into a wpa_supplicant.conf. Run by /etc/init.d/S41wifi at every boot.
 *
 *   newspaper-wifi INPUT OUTPUT
 *
 * Exit status: 0 written, 2 no SSID set (file missing or untouched),
 * 1 the file has a mistake (the reason goes to stderr).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: newspaper-wifi INPUT OUTPUT\n");
        return 1;
    }
    static char text[8192];
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "newspaper-wifi: %s: %s\n", argv[1], strerror(errno));
        return errno == ENOENT ? 2 : 1;
    }
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;

    np_wifi_file_t w;
    char err[160];
    int rc = np_wifi_file_parse(text, &w, err, sizeof(err));
    if (rc == 1) {
        fprintf(stderr, "newspaper-wifi: no ssid in %s\n", argv[1]);
        return 2;
    }
    if (rc != 0) {
        fprintf(stderr, "newspaper-wifi: %s: %s\n", argv[1], err);
        return 1;
    }
    static char conf[2048];
    if (np_wifi_file_to_wpa(&w, conf, sizeof(conf), err, sizeof(err)) != 0) {
        fprintf(stderr, "newspaper-wifi: %s\n", err);
        return 1;
    }
    int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "newspaper-wifi: %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    size_t len = strlen(conf);
    bool ok = write(fd, conf, len) == (ssize_t)len;
    ok = close(fd) == 0 && ok;
    if (!ok) {
        fprintf(stderr, "newspaper-wifi: cannot write %s\n", argv[2]);
        return 1;
    }
    printf("newspaper-wifi: network \"%s\"%s%s%s\n", w.ssid, w.password[0] ? "" : " (open)",
           w.country[0] ? ", country " : "", w.country);
    return 0;
}
