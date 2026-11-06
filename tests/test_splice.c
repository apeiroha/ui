/* Regression tests for ui_SpliceTo (io_uring SPLICE).
 *
 * Three goroutines form a relay: feeder -> middle pipe -> splice relay
 * -> out pipe -> drainer.  The middle goroutine moves 12KB from the
 * first pipe to the second with pure splice; every other goroutine
 * does plain ui_Write/ui_Read, so the whole chain exercises the
 * io_uring park/resume path (not just the sync fallback).
 *
 * Also covers the fallback: calling ui_SpliceTo outside ui_Init (no
 * vCPU) must behave like a synchronous splice(2).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAYLOAD_SIZE 12288
#define PAYLOAD_BYTE 0x5A

static int     g_pass, g_fail;
static unsigned char g_payload[PAYLOAD_SIZE];

static void
check(const char *name, bool ok, const char *detail)
{
    printf("  TEST: %s ... %s%s%s\n", name, ok ? "PASS" : "FAIL",
           detail && *detail ? " — " : "", detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
}

/* ── fallback: no runtime ── */

static void
test_fallback_outside_runtime(void)
{
    int in_p[2], out_p[2];
    if (pipe(in_p) != 0 || pipe(out_p) != 0) {
        check("ui_SpliceTo sync fallback (no runtime)", false, "pipe failed");
        return;
    }
    if (write(in_p[1], g_payload, PAYLOAD_SIZE) != PAYLOAD_SIZE) {
        check("ui_SpliceTo sync fallback (no runtime)", false, "write failed");
        close(in_p[0]); close(in_p[1]); close(out_p[0]); close(out_p[1]);
        return;
    }
    close(in_p[1]);
    ssize_t moved = ui_SpliceTo(out_p[1], in_p[0], PAYLOAD_SIZE, 0);
    close(in_p[0]); close(out_p[1]);
    unsigned char buf[PAYLOAD_SIZE];
    ssize_t got = 0;
    while (got < PAYLOAD_SIZE) {
        ssize_t n = read(out_p[0], buf + got, PAYLOAD_SIZE - got);
        if (n <= 0) break;
        got += n;
    }
    close(out_p[0]);
    bool ok = moved == PAYLOAD_SIZE && got == PAYLOAD_SIZE &&
              memcmp(buf, g_payload, PAYLOAD_SIZE) == 0;
    char detail[128];
    snprintf(detail, sizeof detail, "moved=%zd got=%zd", moved, got);
    check("ui_SpliceTo sync fallback (no runtime)", ok, detail);
}

/* ── 3-goro relay through io_uring ── */

static int in_p[2], out_p[2];
static ssize_t g_relay_moved;

static void
relay_feeder(void)
{
    ssize_t off = 0;
    while (off < PAYLOAD_SIZE) {
        ssize_t n = ui_Write(in_p[1], g_payload + off, PAYLOAD_SIZE - off);
        if (n <= 0) break;
        off += n;
    }
    ui_Close(in_p[1]);
}

static void
relay_splice(void)
{
    ssize_t off = 0;
    while (off < PAYLOAD_SIZE) {
        ssize_t n = ui_SpliceTo(out_p[1], in_p[0], PAYLOAD_SIZE - off, 0);
        if (n <= 0) break;
        off += n;
    }
    g_relay_moved = off;
    ui_Close(out_p[1]);
}

static unsigned char g_drained[PAYLOAD_SIZE];
static ssize_t g_drained_n;

static void
relay_drainer(void)
{
    ssize_t off = 0;
    while (off < PAYLOAD_SIZE) {
        ssize_t n = ui_Read(out_p[0], g_drained + off, PAYLOAD_SIZE - off);
        if (n <= 0) break;
        off += n;
    }
    g_drained_n = off;
}

static void
test_relay_3_goro(void)
{
    if (pipe(in_p) != 0 || pipe(out_p) != 0) {
        check("3-goro splice relay (12KB, io_uring park)", false, "pipe failed");
        return;
    }
    g_relay_moved = 0;
    g_drained_n = 0;
    memset(g_drained, 0, sizeof g_drained);

    ui_Go(relay_feeder);
    ui_Go(relay_splice);
    ui_Go(relay_drainer);
    ui_Run();

    close(in_p[0]);
    close(out_p[0]);

    bool ok = g_relay_moved == PAYLOAD_SIZE && g_drained_n == PAYLOAD_SIZE &&
              memcmp(g_drained, g_payload, PAYLOAD_SIZE) == 0;
    char detail[128];
    snprintf(detail, sizeof detail, "relayed=%zd drained=%zd",
             g_relay_moved, g_drained_n);
    check("3-goro splice relay (12KB, io_uring park)", ok, detail);
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== UI: ui_SpliceTo regression (sync fallback + 3-goro relay)\n\n");

    memset(g_payload, PAYLOAD_BYTE, sizeof g_payload);

    test_fallback_outside_runtime();

    if (ui_Init() != 0) return 1;
    test_relay_3_goro();
    ui_Fini();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
