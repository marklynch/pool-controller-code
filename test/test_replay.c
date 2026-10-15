/**
 * Log replay harness for message_decoder.
 *
 * Reads a captured log file. For each line of the form
 *
 *     I (HH:MM:SS.mmm) MSG_DECODER: RX MSG: <hex bytes>
 *
 * the bytes are fed into decode_message(). The subsequent consecutive
 * MSG_DECODER log lines in the file are the "expected" output; the captured
 * output of the decoder (stripped of timestamps) must match line-for-line.
 *
 * Usage:
 *     ./run_replay [--bless] [path ...]
 *
 *   --bless    Rewrite each file's MSG_DECODER lines using the current
 *              decoder's output, preserving the RX MSG timestamp on each
 *              line. Use to update goldens after intentional log changes.
 *   path ...   Sample files. If omitted, scans `samples/` next to the
 *              binary's working directory.
 */

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../main/message_decoder.h"
#include "../main/pool_state.h"
#include "log_capture.h"

/* ====================================================================== *
 * Mocks for hardware / framework dependencies                              *
 * ====================================================================== */

uint32_t xTaskGetTickCount(void) { return 1000; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t s) { (void)s; }

void mqtt_publish_mode(const pool_state_t *s) { (void)s; }
void mqtt_publish_heater(const pool_state_t *s, int i) { (void)s; (void)i; }
void mqtt_publish_gas_heater(const pool_state_t *s, int i) { (void)s; (void)i; }
void mqtt_publish_chlorinator(const pool_state_t *s) { (void)s; }
void mqtt_publish_light(const pool_state_t *s, uint8_t z) { (void)s; (void)z; }
void mqtt_publish_channel(const pool_state_t *s, uint8_t c) { (void)s; (void)c; }
void mqtt_publish_valve(const pool_state_t *s, uint8_t v) { (void)s; (void)v; }
void mqtt_publish_heater_setpoints(const pool_state_t *s, int i) { (void)s; (void)i; }
void mqtt_publish_temperature_reading(const pool_state_t *s, int d, uint8_t i) { (void)s; (void)d; (void)i; }
void mqtt_publish_favourite(const pool_state_t *s) { (void)s; }
void mqtt_publish_pump(const pool_state_t *s) { (void)s; }
void mqtt_publish_service_mode(const pool_state_t *s) { (void)s; }

void register_requester_notify(void) {}
void mqtt_publish_heater_telemetry(const pool_state_t *state, int index, bool force)
{
    (void)state; (void)index; (void)force;
}

static pool_state_t              g_state;
static message_decoder_context_t g_ctx;
static int                       g_dummy_mutex;

static void init_ctx(void)
{
    memset(&g_state, 0, sizeof g_state);
    g_ctx.pool_state  = &g_state;
    g_ctx.state_mutex = (SemaphoreHandle_t)&g_dummy_mutex;
    g_ctx.enable_mqtt = false;
}

/* ====================================================================== *
 * String list                                                              *
 * ====================================================================== */

typedef struct {
    char **items;
    int    count;
    int    cap;
} strlist_t;

static void sl_init(strlist_t *l) { l->items = NULL; l->count = 0; l->cap = 0; }

static void sl_push(strlist_t *l, const char *s)
{
    if (l->count >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, (size_t)l->cap * sizeof *l->items);
    }
    l->items[l->count++] = strdup(s);
}

static void sl_free(strlist_t *l)
{
    for (int i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

/* ====================================================================== *
 * Line helpers                                                             *
 * ====================================================================== */

static void rtrim(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '\t' ||
                     s[l-1] == '\r' || s[l-1] == '\n')) {
        s[--l] = '\0';
    }
}

/* Match "<L> (<anything>) MSG_DECODER: <body>" and emit canonical
 * "<L> MSG_DECODER: <body>" into `out`. The bracketed timestamp can be either
 * the ESP_LOGI human form "HH:MM:SS.mmm" or a raw tick count. Returns true
 * on match. */
static bool normalize_decoder_line(const char *line, char *out, size_t out_size)
{
    if (line[0] == '\0' || line[1] != ' ' || line[2] != '(') return false;
    char level = line[0];
    if (level != 'I' && level != 'W' && level != 'E' && level != 'D' && level != 'V')
        return false;
    const char *close = strchr(line + 3, ')');
    if (!close || close[1] != ' ') return false;
    const char *rest = close + 2;
    if (strncmp(rest, "MSG_DECODER:", 12) != 0) return false;
    snprintf(out, out_size, "%c %s", level, rest);
    rtrim(out);
    return true;
}

/* Extract "HH:MM:SS.mmm" from "X (HH:MM:SS.mmm) ...". Returns true on match. */
static bool extract_timestamp(const char *line, char *out, size_t out_size)
{
    const char *p = strchr(line, '(');
    if (!p) return false;
    const char *q = strchr(p, ')');
    if (!q || q <= p + 1) return false;
    size_t len = (size_t)(q - p - 1);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p + 1, len);
    out[len] = '\0';
    return true;
}

/* Parse hex bytes following "RX MSG: " in a normalized decoder line. */
static int parse_rx_msg_hex(const char *normalized, uint8_t *out, int out_max)
{
    const char *p = strstr(normalized, "RX MSG: ");
    if (!p) return -1;
    p += strlen("RX MSG: ");
    int n = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) break;
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) break;
        if (n >= out_max) return -1;
        out[n++] = (uint8_t)v;
        p += 2;
    }
    return n;
}

/* ====================================================================== *
 * File I/O                                                                 *
 * ====================================================================== */

static char **read_lines(const char *path, int *out_count)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    int    cap = 256, n = 0;
    char **lines = malloc((size_t)cap * sizeof *lines);
    char   buf[8192];
    while (fgets(buf, sizeof buf, f)) {
        rtrim(buf);
        if (n >= cap) {
            cap *= 2;
            lines = realloc(lines, (size_t)cap * sizeof *lines);
        }
        lines[n++] = strdup(buf);
    }
    fclose(f);
    *out_count = n;
    return lines;
}

static void free_lines(char **lines, int n)
{
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* ====================================================================== *
 * Blocks                                                                   *
 * ====================================================================== */

typedef struct {
    int       rx_line_idx;     /* index of "RX MSG:" line in file */
    int       last_msg_idx;    /* index of last consecutive MSG_DECODER line */
    char      timestamp[32];   /* "HH:MM:SS.mmm" from the RX MSG line */
    uint8_t   frame[512];
    int       frame_len;
    strlist_t expected;        /* normalized expected lines (incl. the RX MSG line) */
    strlist_t actual;          /* normalized actual lines from this run */
} block_t;

static block_t *find_blocks(char **lines, int n_lines, int *out_n)
{
    int       cap = 0, n = 0;
    block_t  *blocks = NULL;
    char      norm[8192];

    int i = 0;
    while (i < n_lines) {
        if (!normalize_decoder_line(lines[i], norm, sizeof norm)) { i++; continue; }
        if (strstr(norm, "RX MSG: ") == NULL) { i++; continue; }

        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            blocks = realloc(blocks, (size_t)cap * sizeof *blocks);
        }
        block_t *b = &blocks[n++];
        memset(b, 0, sizeof *b);
        sl_init(&b->expected);
        sl_init(&b->actual);
        b->rx_line_idx  = i;
        b->last_msg_idx = i;
        extract_timestamp(lines[i], b->timestamp, sizeof b->timestamp);
        b->frame_len = parse_rx_msg_hex(norm, b->frame, sizeof b->frame);
        sl_push(&b->expected, norm);

        /* Collect subsequent consecutive MSG_DECODER lines that are not
         * themselves the start of a new RX MSG block. */
        int j = i + 1;
        while (j < n_lines) {
            char norm2[8192];
            if (!normalize_decoder_line(lines[j], norm2, sizeof norm2)) break;
            if (strstr(norm2, "RX MSG: ") != NULL) break;
            sl_push(&b->expected, norm2);
            b->last_msg_idx = j;
            j++;
        }
        i = j;
    }
    *out_n = n;
    return blocks;
}

static void free_blocks(block_t *blocks, int n)
{
    for (int i = 0; i < n; i++) {
        sl_free(&blocks[i].expected);
        sl_free(&blocks[i].actual);
    }
    free(blocks);
}

/* ====================================================================== *
 * Replay                                                                   *
 * ====================================================================== */

static void replay_one(block_t *b)
{
    log_capture_reset();
    log_capture_enabled = true;
    (void)decode_message(b->frame, b->frame_len, &g_ctx);
    log_capture_enabled = false;

    /* Split capture buffer into trimmed lines. */
    char *copy = strdup(log_capture_buf);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save);
         line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", line);
        rtrim(buf);
        sl_push(&b->actual, buf);
    }
    free(copy);
}

static bool blocks_match(const block_t *b)
{
    if (b->expected.count != b->actual.count) return false;
    for (int i = 0; i < b->expected.count; i++) {
        if (strcmp(b->expected.items[i], b->actual.items[i]) != 0) return false;
    }
    return true;
}

static void print_mismatch(const char *path, int frame_no, const block_t *b)
{
    printf("FAIL: %s frame %d (line %d)\n", path, frame_no, b->rx_line_idx + 1);
    printf("  Expected (%d line%s):\n",
           b->expected.count, b->expected.count == 1 ? "" : "s");
    for (int i = 0; i < b->expected.count; i++) {
        printf("    %s\n", b->expected.items[i]);
    }
    printf("  Actual (%d line%s):\n",
           b->actual.count, b->actual.count == 1 ? "" : "s");
    for (int i = 0; i < b->actual.count; i++) {
        printf("    %s\n", b->actual.items[i]);
    }
    printf("\n");
}

/* ====================================================================== *
 * Bless: rewrite file using current decoder output                         *
 * ====================================================================== */

static int bless_file(const char *path, char **lines, int n_lines,
                      block_t *blocks, int n_blocks)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.bless.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) { perror(tmp); return 1; }

    int bi = 0;
    for (int j = 0; j < n_lines; j++) {
        if (bi < n_blocks && j == blocks[bi].rx_line_idx) {
            const block_t *b = &blocks[bi];
            for (int k = 0; k < b->actual.count; k++) {
                const char *al = b->actual.items[k];
                /* al is "<L> MSG_DECODER: <body>" — split into level + rest. */
                if (strlen(al) < 3) { fprintf(out, "%s\n", al); continue; }
                char level = al[0];
                const char *rest = al + 2;
                fprintf(out, "%c (%s) %s\n", level, b->timestamp, rest);
            }
            j = b->last_msg_idx;   /* for-loop will increment past it */
            bi++;
            continue;
        }
        fprintf(out, "%s\n", lines[j]);
    }
    fclose(out);
    if (rename(tmp, path) != 0) { perror("rename"); return 1; }
    return 0;
}

/* ====================================================================== *
 * Per-file driver                                                          *
 * ====================================================================== */

typedef struct {
    int files;
    int frames;
    int passed;
    int failed;
} stats_t;

static int process_file(const char *path, bool bless, stats_t *st)
{
    int    n_lines = 0;
    char **lines   = read_lines(path, &n_lines);
    if (!lines) {
        fprintf(stderr, "ERROR: cannot read %s\n", path);
        return 1;
    }

    int       n_blocks = 0;
    block_t  *blocks   = find_blocks(lines, n_lines, &n_blocks);

    /* State is shared across all frames within a file — matches how the
     * decoder runs in the live device, where each frame may depend on
     * state accumulated from earlier ones. */
    init_ctx();

    st->files++;
    if (n_blocks == 0) {
        printf("  %s: no RX MSG lines\n", path);
        free_blocks(blocks, n_blocks);
        free_lines(lines, n_lines);
        return 0;
    }

    int file_fail = 0;
    int file_pass = 0;
    for (int i = 0; i < n_blocks; i++) {
        block_t *b = &blocks[i];
        st->frames++;

        if (b->frame_len <= 0) {
            printf("FAIL: %s frame %d (line %d): could not parse RX MSG hex\n",
                   path, i + 1, b->rx_line_idx + 1);
            st->failed++;
            file_fail = 1;
            continue;
        }

        replay_one(b);

        if (bless) {
            st->passed++;
            file_pass++;
            continue;
        }

        if (blocks_match(b)) {
            st->passed++;
            file_pass++;
        } else {
            st->failed++;
            file_fail = 1;
            print_mismatch(path, i + 1, b);
        }
    }

    if (bless) {
        if (bless_file(path, lines, n_lines, blocks, n_blocks) == 0) {
            printf("  blessed: %s (%d frame%s)\n",
                   path, n_blocks, n_blocks == 1 ? "" : "s");
        } else {
            file_fail = 1;
        }
    } else {
        printf("  %s: %d/%d frame%s passed\n",
               path, file_pass, n_blocks, n_blocks == 1 ? "" : "s");
    }

    free_blocks(blocks, n_blocks);
    free_lines(lines, n_lines);
    return file_fail;
}

/* ====================================================================== *
 * Path collection                                                          *
 * ====================================================================== */

static int collect_default_paths(strlist_t *out)
{
    const char *dir = "samples";
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        size_t l = strlen(name);
        if (l < 4 || strcmp(name + l - 4, ".txt") != 0) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        sl_push(out, path);
    }
    closedir(d);
    return out->count;
}

/* ====================================================================== *
 * main                                                                     *
 * ====================================================================== */

int main(int argc, char **argv)
{
    bool      bless = false;
    strlist_t paths;
    sl_init(&paths);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bless") == 0) {
            bless = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--bless] [path ...]\n", argv[0]);
            return 0;
        } else {
            sl_push(&paths, argv[i]);
        }
    }

    if (paths.count == 0) {
        if (collect_default_paths(&paths) == 0) {
            printf("No sample files found in ./samples/. Nothing to do.\n");
            return 0;
        }
    }

    stats_t st = {0};
    int     overall_fail = 0;
    for (int i = 0; i < paths.count; i++) {
        if (process_file(paths.items[i], bless, &st) != 0) overall_fail = 1;
    }

    printf("\n");
    printf("Replay summary: %d file%s, %d frame%s, %d passed, %d failed\n",
           st.files,  st.files  == 1 ? "" : "s",
           st.frames, st.frames == 1 ? "" : "s",
           st.passed, st.failed);

    sl_free(&paths);
    return overall_fail ? 1 : 0;
}
