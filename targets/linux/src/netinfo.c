/*
 * Parsers for the kernel's network status files. Kept apart from net.c so
 * the host tests can feed them text.
 *
 *   /proc/net/route       Iface Destination Gateway Flags ... (hex, IPv4)
 *   /proc/net/ipv6_route  dest dest_len src src_len next_hop metric refcnt use flags iface
 *   /proc/net/wireless    two header lines, then "wlan0: 0000   70.  -40.  -256 ..."
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define RTF_UP_FLAG 0x0001

static const char *next_line(const char *p)
{
    const char *eol = strchr(p, '\n');
    return eol ? eol + 1 : p + strlen(p);
}

static bool ipv4_default(const char *text)
{
    const char *p = next_line(text); /* header */
    while (*p) {
        char iface[32], dest[16], gw[16];
        unsigned flags = 0;
        if (sscanf(p, "%31s %15s %15s %x", iface, dest, gw, &flags) == 4) {
            if (strcmp(dest, "00000000") == 0 && (flags & RTF_UP_FLAG) && strcmp(iface, "lo") != 0) {
                return true;
            }
        }
        p = next_line(p);
    }
    return false;
}

static bool ipv6_default(const char *text)
{
    const char *p = text;
    while (*p) {
        char dest[40], dlen[4], src[40], slen[4], hop[40], metric[12], ref[12], use[12], flags[12], iface[32];
        if (sscanf(p, "%39s %3s %39s %3s %39s %11s %11s %11s %11s %31s", dest, dlen, src, slen, hop, metric, ref,
                   use, flags, iface) == 10) {
            unsigned long f = strtoul(flags, NULL, 16);
            bool zero_hop = strspn(hop, "0") == strlen(hop);
            if (strspn(dest, "0") == strlen(dest) && strcmp(dlen, "00") == 0 && (f & RTF_UP_FLAG) && !zero_hop &&
                strcmp(iface, "lo") != 0) {
                return true;
            }
        }
        p = next_line(p);
    }
    return false;
}

bool np_net_has_default_route(const char *route, const char *ipv6_route)
{
    return (route && ipv4_default(route)) || (ipv6_route && ipv6_default(ipv6_route));
}

int np_net_wireless_dbm(const char *wireless)
{
    if (!wireless) {
        return 0;
    }
    const char *p = next_line(next_line(wireless));
    while (*p) {
        const char *colon = strchr(p, ':');
        const char *eol = strchr(p, '\n');
        if (colon && (!eol || colon < eol)) {
            unsigned status = 0;
            double link = 0, level = 0;
            /* strtod reads "70." and "-40." with their trailing dots. */
            if (sscanf(colon + 1, " %x %lf %lf", &status, &link, &level) == 3) {
                int dbm = (int)level;
                if (dbm < 0 && dbm > -120) {
                    return dbm;
                }
            }
        }
        p = next_line(p);
    }
    return 0;
}
