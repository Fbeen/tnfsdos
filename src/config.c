/*
 * CONFIG.C  —  TNFSDRV.CFG INI-file parser.
 *
 * Parses [section] / key=value pairs.  Section names and keys are
 * case-insensitive.  Fallback order: [profile] > [default] > built-in.
 *
 * Called once from main() before going TSR; standard file I/O is safe here.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

/* ------------------------------------------------------------------ */
/*  String helpers                                                      */
/* ------------------------------------------------------------------ */

static void str_to_upper(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s -= 32;
}

static void str_trim(char *s)
{
    int len, i, j;
    len = (int)strlen(s);
    while (len > 0 && (unsigned char)s[len-1] <= ' ') s[--len] = '\0';
    for (i = 0; s[i] && (unsigned char)s[i] <= ' '; i++) {}
    if (i > 0) {
        for (j = 0; j + i <= len; j++) s[j] = s[j + i];
    }
}

static int str_eq_ci(const char *a, const char *b)
{
    char ac, bc;
    for (;;) {
        ac = *a++; bc = *b++;
        if (ac >= 'a' && ac <= 'z') ac -= 32;
        if (bc >= 'a' && bc <= 'z') bc -= 32;
        if (ac != bc) return 0;
        if (!ac) return 1;
    }
}

/* ------------------------------------------------------------------ */
/*  Key dispatch                                                        */
/* ------------------------------------------------------------------ */

static void apply_key(TnfsDrvConfig *cfg, const char *key, const char *value)
{
    if (str_eq_ci(key, "servername")) {
        strncpy(cfg->servername, value, sizeof(cfg->servername) - 1);
        cfg->servername[sizeof(cfg->servername) - 1] = '\0';
    } else if (str_eq_ci(key, "serverroot")) {
        strncpy(cfg->serverroot, value, sizeof(cfg->serverroot) - 1);
        cfg->serverroot[sizeof(cfg->serverroot) - 1] = '\0';
    } else if (str_eq_ci(key, "protocol")) {
        strncpy(cfg->protocol, value, sizeof(cfg->protocol) - 1);
        cfg->protocol[sizeof(cfg->protocol) - 1] = '\0';
        str_to_upper(cfg->protocol);
    } else if (str_eq_ci(key, "port")) {
        int v = atoi(value);
        if (v > 0) cfg->port = (unsigned int)v;
    } else if (str_eq_ci(key, "driveletter")) {
        char c = value[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c <= 'Z') cfg->driveletter = c;
    } else if (str_eq_ci(key, "localip")) {
        strncpy(cfg->localip, value, sizeof(cfg->localip) - 1);
        cfg->localip[sizeof(cfg->localip) - 1] = '\0';
    } else if (str_eq_ci(key, "netmask")) {
        strncpy(cfg->netmask, value, sizeof(cfg->netmask) - 1);
        cfg->netmask[sizeof(cfg->netmask) - 1] = '\0';
    } else if (str_eq_ci(key, "gateway")) {
        strncpy(cfg->gateway, value, sizeof(cfg->gateway) - 1);
        cfg->gateway[sizeof(cfg->gateway) - 1] = '\0';
    } else if (str_eq_ci(key, "packetint")) {
        /* Accept decimal or 0x hex: strtol handles both. */
        unsigned long v = strtoul(value, (char **)0, 0);
        if (v > 0 && v <= 0xFF) cfg->packetint = (unsigned int)v;
    }
}

/* ------------------------------------------------------------------ */
/*  Section scanner — rewinds and applies all keys in [section].       */
/* ------------------------------------------------------------------ */

static int apply_section(FILE *f, const char *section, TnfsDrvConfig *cfg)
{
    char line[128];
    char cur_sec[32];
    int  in_sec = 0, found = 0;
    char *eq, *key, *val, *close;
    int  len;

    rewind(f);
    cur_sec[0] = '\0';

    while (fgets(line, sizeof(line), f)) {
        len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';

        if (!line[0] || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[') {
            close = strchr(line + 1, ']');
            if (!close) continue;
            *close = '\0';
            strncpy(cur_sec, line + 1, sizeof(cur_sec) - 1);
            cur_sec[sizeof(cur_sec) - 1] = '\0';
            str_trim(cur_sec);
            in_sec = str_eq_ci(cur_sec, section);
            if (in_sec) found = 1;
            continue;
        }

        if (!in_sec) continue;

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line; val = eq + 1;
        str_trim(key); str_trim(val);
        if (!key[0]) continue;

        apply_key(cfg, key, val);
    }

    return found;
}

/* ------------------------------------------------------------------ */
/*  Public interface                                                    */
/* ------------------------------------------------------------------ */

void config_set_defaults(TnfsDrvConfig *cfg)
{
    strncpy(cfg->profile,    "default",  sizeof(cfg->profile)    - 1);
    cfg->profile[sizeof(cfg->profile) - 1] = '\0';
    cfg->servername[0] = '\0';
    cfg->serverroot[0] = '/';
    cfg->serverroot[1] = '\0';
    strncpy(cfg->protocol,   "UDP",      sizeof(cfg->protocol)   - 1);
    cfg->protocol[sizeof(cfg->protocol) - 1] = '\0';
    cfg->port        = 16384;
    cfg->driveletter = (char)TNFSDRV_DRIVE_LETTER;
    cfg->localip[0]  = '\0';
    cfg->netmask[0]  = '\0';
    cfg->gateway[0]  = '\0';
    cfg->packetint   = 0x60;
}

int config_load(const char *filename, const char *profile, TnfsDrvConfig *cfg)
{
    FILE *f;

    config_set_defaults(cfg);
    strncpy(cfg->profile, profile, sizeof(cfg->profile) - 1);
    cfg->profile[sizeof(cfg->profile) - 1] = '\0';

    f = fopen(filename, "r");
    if (!f) return 0;

    apply_section(f, "default", cfg);
    if (!str_eq_ci(profile, "default"))
        apply_section(f, profile, cfg);

    fclose(f);
    return 1;
}
