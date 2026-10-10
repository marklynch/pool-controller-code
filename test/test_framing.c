#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "log_capture.h"

// Include framing module directly for testing
#include "../main/framing.h"
#include "../main/framing.c"

// Helper to trim trailing whitespace/newlines
static void trim_trailing(char *s) {
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '\t' || s[l-1] == '\r' || s[l-1] == '\n')) {
        s[--l] = '\0';
    }
}

// Helper to convert single hex char to nibble
static int hex_char_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Helper to parse hex string into byte array
static int parse_hex_to_bytes(const char *hex, uint8_t *dest) {
    int len = 0;
    const char *p = hex;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        int hi = hex_char_to_val(*p++);
        if (hi < 0 || !*p) break;
        int lo = hex_char_to_val(*p++);
        if (lo < 0) break;
        dest[len++] = (hi << 4) | lo;
    }
    return len;
}

// Device name mapping matching python scripts
static const char* get_test_device_name(uint16_t addr) {
    if (addr == 0x0050) return "Touch Screen";
    if (addr == 0x0062) return "Connect 8/10";
    if (addr == 0x0074) return "ICI Gas Heater";
    if (addr == 0x00A0) return "Viron XT Pump";
    if (addr == 0xFFFF) return "Broadcast";
    if (addr == 0x006F) return "Internal Channels";
    return "Unknown";
}

// Resync type name — must match the resync category keys surfaced elsewhere
// (pool_state resyncs_* fields and the /status JSON).
static const char* resync_name(framing_result_t r) {
    switch (r) {
        case FRAMING_NO_START_BYTE:       return "no_start";
        case FRAMING_BAD_HEADER_CHECKSUM: return "header_checksum";
        case FRAMING_BAD_FRAME_TYPE:      return "bad_frame_type";
        case FRAMING_BAD_LENGTH:          return "bad_length";
        case FRAMING_BAD_END_BYTE:        return "bad_end";
        case FRAMING_BAD_DATA_CHECKSUM:   return "data_checksum";
        default:                          return "?";
    }
}

static char actual_out[8192];
static int actual_len = 0;

// Drain every frame/resync currently available in `fb`, appending the ordered
// "Discard:/Resync:/Decoded:" lines to actual_out. *log_pos tracks how far the
// log-capture buffer has been consumed so a multi-byte "Discarding ..." log
// line (which is logged but not reflected in `result`) is surfaced exactly
// once, even when draining is spread across several chunks. Stops when the
// parser returns NEED_MORE_DATA.
static void drain_and_record(framing_buffer_t *fb, size_t *log_pos) {
    uint8_t frame[BUS_MESSAGE_MAX_SIZE];
    int frame_len;
    framing_result_t result;
    for (;;) {
        result = framing_process_next(fb, frame, &frame_len);

        // A single call can silently discard bytes before resyncing onto a
        // later start byte (logged but not reflected in `result`) - surface
        // that log line so the golden file can assert on it. This happens
        // even on a call that ultimately returns NEED_MORE_DATA, since the
        // discard happens before that check.
        char *discard = strstr(log_capture_buf + *log_pos, "Discarding ");
        if (discard) {
            char *line_end = strchr(discard, '\n');
            int line_len = line_end ? (int)(line_end - discard) : (int)strlen(discard);
            // Trim trailing whitespace to match what trim_trailing() does
            // to the golden file's copy of this line when read back.
            while (line_len > 0 && (discard[line_len - 1] == ' ' || discard[line_len - 1] == '\t')) {
                line_len--;
            }
            int n = snprintf(actual_out + actual_len, sizeof(actual_out) - actual_len,
                             "Discard: %.*s\n", line_len, discard);
            if (n > 0) actual_len += n;
        }
        *log_pos = log_capture_len;

        if (result == FRAMING_NEED_MORE_DATA) {
            break;
        }
        if (result != FRAMING_FRAME_READY) {
            int n = snprintf(actual_out + actual_len, sizeof(actual_out) - actual_len,
                             "Resync:  %s\n", resync_name(result));
            if (n > 0) actual_len += n;
            continue;
        }
        uint16_t src = (frame[1] << 8) | frame[2];
        uint16_t dst = (frame[3] << 8) | frame[4];
        uint8_t cmd = frame[7];

        char payload_str[1024] = "";
        int payload_len = frame_len - 12;
        for (int i = 0; i < payload_len; i++) {
            sprintf(payload_str + i * 2, "%02X", frame[10 + i]);
        }

        int n = snprintf(actual_out + actual_len, sizeof(actual_out) - actual_len,
                         "Decoded: Src=%04X (%s), Dst=%04X (%s), Cmd=0x%02X, Payload=%s\n",
                         src, get_test_device_name(src),
                         dst, get_test_device_name(dst),
                         cmd, payload_str);
        if (n > 0) actual_len += n;
    }
}

// Run one hex/golden file pair. Each non-blank, non-comment line of the hex
// file is one case: its bytes are fed to the parser and the ordered stream of
// resync events ("Resync:  <type>") and decoded frames ("Decoded: ...") is
// compared against the matching "--- Case N ---" block in the golden file.
static int run_file(const char *hex_file_path, const char *gold_file_path,
                    bool bless, int *passed, int *failed) {
    FILE *f_hex = fopen(hex_file_path, "r");
    if (!f_hex) {
        fprintf(stderr, "ERROR: Cannot open %s\n", hex_file_path);
        return 1;
    }

    // Compare mode reads the golden; bless mode rewrites it from the current
    // parser output via a temp file + rename (mirrors run_replay --bless).
    char gold_tmp_path[4096];
    FILE *f_gold = NULL;
    if (bless) {
        snprintf(gold_tmp_path, sizeof(gold_tmp_path), "%s.bless.tmp", gold_file_path);
        f_gold = fopen(gold_tmp_path, "w");
    } else {
        f_gold = fopen(gold_file_path, "r");
    }
    if (!f_gold) {
        fprintf(stderr, "ERROR: Cannot open %s\n", bless ? gold_tmp_path : gold_file_path);
        fclose(f_hex);
        return 1;
    }

    char hex_line[4096];
    char gold_line[1024];
    int case_num = 1;
    int file_line = 0;   // 1-based source line in the hex file, for failure locating
    framing_buffer_t fb;

    printf("Checking %s ...\n", hex_file_path);

    while (fgets(hex_line, sizeof(hex_line), f_hex)) {
        file_line++;
        trim_trailing(hex_line);
        if (hex_line[0] == '\0' || hex_line[0] == '#') {
            continue;  // skip blank lines and comments
        }

        // Compare mode: read this case's expected block from the golden.
        char expected_out[4096] = "";
        if (!bless) {
            char expected_hdr[128];
            sprintf(expected_hdr, "--- Case %d ---", case_num);

            if (!fgets(gold_line, sizeof(gold_line), f_gold)) {
                fprintf(stderr, "ERROR: Unexpected EOF in %s when looking for Case %d header\n", gold_file_path, case_num);
                (*failed)++;
                break;
            }
            trim_trailing(gold_line);
            if (strcmp(gold_line, expected_hdr) != 0) {
                fprintf(stderr, "ERROR: Expected golden header '%s', got '%s'\n", expected_hdr, gold_line);
                (*failed)++;
                break;
            }

            // Read all expected outputs until a blank line or EOF
            int expected_len = 0;
            while (fgets(gold_line, sizeof(gold_line), f_gold)) {
                trim_trailing(gold_line);
                if (gold_line[0] == '\0') {
                    break; // Blank line signals end of case
                }
                int n = snprintf(expected_out + expected_len, sizeof(expected_out) - expected_len, "%s\n", gold_line);
                if (n > 0) {
                    expected_len += n;
                }
            }
        }

        // Prepare parser state and capture buffers
        framing_init(&fb);
        actual_len = 0;
        actual_out[0] = '\0';
        log_capture_reset();
        size_t log_pos = log_capture_len;

        // Feed the case into the parser. A '|' in the line splits it into
        // separate framing_add_bytes() calls, draining after each - this
        // mirrors the bridge's "append per UART read, then drain" loop and
        // exercises reassembly/resync across read boundaries. A line with no
        // '|' is a single chunk, identical to the original whole-line behaviour.
        bool add_failed = false;
        char *saveptr = NULL;
        for (char *seg = strtok_r(hex_line, "|", &saveptr);
             seg != NULL;
             seg = strtok_r(NULL, "|", &saveptr)) {
            uint8_t tmp_buf[1024];
            int parsed_bytes = parse_hex_to_bytes(seg, tmp_buf);
            if (parsed_bytes <= 0) {
                continue;  // empty/whitespace-only chunk
            }
            if (!framing_add_bytes(&fb, tmp_buf, parsed_bytes)) {
                fprintf(stderr, "ERROR: Failed to add bytes to framing buffer (too long)\n");
                add_failed = true;
                break;
            }
            // Drain everything available so far, recording resync events and
            // decoded frames in the order they occur.
            drain_and_record(&fb, &log_pos);
        }
        if (add_failed) {
            (*failed)++;
            case_num++;
            continue;
        }

        if (bless) {
            // Rewrite this case from the current parser output.
            fprintf(f_gold, "--- Case %d ---\n%s\n", case_num, actual_out);
            (*passed)++;
        } else if (strcmp(actual_out, expected_out) == 0) {
            (*passed)++;
        } else {
            (*failed)++;
            printf("FAIL: %s:%d (Case %d)\n", hex_file_path, file_line, case_num);
            printf("  Input hex: %s\n", hex_line);
            printf("  Expected:\n%s", expected_out);
            printf("  Actual:\n%s", actual_out);
            printf("  Warnings/Resync Logs:\n");
            if (log_capture_len == 0) {
                printf("    (None)\n");
            } else {
                printf("%s", log_capture_buf);
            }
            printf("==================================================\n\n");
        }

        case_num++;
    }

    fclose(f_hex);
    fclose(f_gold);

    if (bless) {
        if (rename(gold_tmp_path, gold_file_path) != 0) {
            perror("rename");
            (*failed)++;
        } else {
            printf("  blessed: %s (%d case%s)\n",
                   gold_file_path, case_num - 1, (case_num - 1) == 1 ? "" : "s");
        }
    }
    return case_num - 1;
}

// Direct assertion for the overflow guard, which framing_process_next never
// reports (the bridge layer raises it when framing_add_bytes rejects a push).
static bool run_overflow_test(int *passed, int *failed) {
    framing_buffer_t fb;
    uint8_t big[BUS_MESSAGE_MAX_SIZE + 1];
    memset(big, 0xFF, sizeof(big));

    framing_init(&fb);
    bool reject_oversized = !framing_add_bytes(&fb, big, BUS_MESSAGE_MAX_SIZE + 1);

    framing_init(&fb);
    bool accept_exact = framing_add_bytes(&fb, big, BUS_MESSAGE_MAX_SIZE);
    bool reject_one_more = !framing_add_bytes(&fb, big, 1);

    bool ok = reject_oversized && accept_exact && reject_one_more;
    printf("Checking framing_add_bytes overflow guard ...\n");
    printf("  oversized push rejected=%d, exact-fill accepted=%d, +1 rejected=%d -> %s\n",
           reject_oversized, accept_exact, reject_one_more, ok ? "PASS" : "FAIL");
    if (ok) (*passed)++; else (*failed)++;
    return ok;
}

// Feed `n` bytes through a fresh parser, split into chunks at the given sorted
// byte offsets (draining after each chunk), and write only the ordered
// "Decoded:" lines to `out`. num_splits==0 means feed everything in one shot.
// Used by the chunk-invariance property test below.
static void decoded_lines_chunked(const uint8_t *bytes, int n,
                                  const int *splits, int num_splits,
                                  char *out, size_t out_size) {
    framing_buffer_t fb;
    framing_init(&fb);
    out[0] = '\0';
    int out_len = 0;
    log_capture_reset();  // discard logs accumulate but are irrelevant here

    int pos = 0;
    for (int s = 0; s <= num_splits; s++) {
        int end = (s < num_splits) ? splits[s] : n;
        if (end > pos) {
            framing_add_bytes(&fb, &bytes[pos], end - pos);
            pos = end;
        }
        uint8_t frame[BUS_MESSAGE_MAX_SIZE];
        int frame_len;
        framing_result_t result;
        while ((result = framing_process_next(&fb, frame, &frame_len)) != FRAMING_NEED_MORE_DATA) {
            if (result != FRAMING_FRAME_READY) {
                continue;  // resync/discard accounting is deliberately ignored
            }
            uint16_t src = (frame[1] << 8) | frame[2];
            uint16_t dst = (frame[3] << 8) | frame[4];
            uint8_t cmd = frame[7];
            char payload_str[1024] = "";
            int payload_len = frame_len - 12;
            for (int i = 0; i < payload_len; i++) {
                sprintf(payload_str + i * 2, "%02X", frame[10 + i]);
            }
            int w = snprintf(out + out_len, out_size - out_len,
                             "Decoded: Src=%04X, Dst=%04X, Cmd=0x%02X, Payload=%s\n",
                             src, dst, cmd, payload_str);
            if (w > 0) out_len += w;
        }
    }
}

// Property: the set and order of recovered frames is invariant to how the byte
// stream is chunked across framing_add_bytes() calls. The parser is
// deterministic on buffer contents, and the only chunk-sensitive path (the
// no-start mass-discard) only ever drops non-0x02 bytes, so it can never lose a
// recoverable frame. We therefore decode each case one-shot, then re-decode it
// split at every interior offset AND fully fragmented (one byte per chunk), and
// assert the "Decoded:" lines match. Resync/discard counts legitimately vary
// with chunking and are NOT compared. Reuses the existing capture/synthetic
// inputs, so it needs no goldens of its own. (Lines here must not use '|'.)
static void run_chunk_invariance(const char *hex_file_path, int *passed, int *failed) {
    FILE *f = fopen(hex_file_path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open %s\n", hex_file_path);
        (*failed)++;
        return;
    }
    printf("Checking chunk invariance: %s ...\n", hex_file_path);

    char line[4096];
    int file_line = 0, case_num = 0, local_fail = 0;
    while (fgets(line, sizeof(line), f)) {
        file_line++;
        trim_trailing(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        uint8_t bytes[1024];
        int n = parse_hex_to_bytes(line, bytes);
        if (n <= 0) {
            continue;
        }
        case_num++;

        char oneshot[8192];
        decoded_lines_chunked(bytes, n, NULL, 0, oneshot, sizeof(oneshot));

        bool case_failed = false;

        // Every single interior split point (two chunks).
        for (int split = 1; split < n && !case_failed; split++) {
            char chunked[8192];
            decoded_lines_chunked(bytes, n, &split, 1, chunked, sizeof(chunked));
            if (strcmp(oneshot, chunked) != 0) {
                local_fail++;
                case_failed = true;
                printf("FAIL: %s:%d (Case %d) split@%d\n", hex_file_path, file_line, case_num, split);
                printf("  One-shot:\n%s  Chunked:\n%s", oneshot, chunked);
                printf("==================================================\n");
            }
        }

        // Maximal fragmentation: one byte per chunk.
        if (!case_failed && n > 1) {
            int splits[1024];
            for (int i = 0; i < n - 1; i++) splits[i] = i + 1;
            char fragmented[8192];
            decoded_lines_chunked(bytes, n, splits, n - 1, fragmented, sizeof(fragmented));
            if (strcmp(oneshot, fragmented) != 0) {
                local_fail++;
                printf("FAIL: %s:%d (Case %d) fully fragmented\n", hex_file_path, file_line, case_num);
                printf("  One-shot:\n%s  Fragmented:\n%s", oneshot, fragmented);
                printf("==================================================\n");
            }
        }
    }
    fclose(f);

    if (local_fail == 0) {
        printf("  %d case(s) invariant across all single splits + full fragmentation -> PASS\n", case_num);
        (*passed)++;
    } else {
        (*failed)++;
    }
}

int main(int argc, char **argv) {
    bool bless = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bless") == 0) {
            bless = true;
        } else {
            fprintf(stderr, "Usage: %s [--bless]\n", argv[0]);
            return 2;
        }
    }

    // Enable log capture so ESP_LOG resync/warnings go to buffer instead of stdout
    log_capture_enabled = true;

    int passed = 0;
    int failed = 0;

    printf(bless ? "Blessing framing goldens...\n\n" : "Running standalone framing tests...\n\n");

    int real_cases = run_file("frames/observed_error_frames.txt",
                              "frames/observed_error_frames_output.txt", bless, &passed, &failed);
    int synth_cases = run_file("frames/synthetic_errors.txt",
                               "frames/synthetic_errors_output.txt", bless, &passed, &failed);
    int chunk_cases = run_file("frames/chunked_reassembly.txt",
                               "frames/chunked_reassembly_output.txt", bless, &passed, &failed);
    run_overflow_test(&passed, &failed);

    // Property test: feeding the same bytes split at any boundary must recover
    // the same frames. Runs over the existing whole-line captures only (no '|').
    if (!bless) {
        run_chunk_invariance("frames/observed_error_frames.txt", &passed, &failed);
        run_chunk_invariance("frames/synthetic_errors.txt", &passed, &failed);
    }

    int total_checks = real_cases + synth_cases + chunk_cases + 1;
    printf("\nFraming test summary: %d golden checks (%d real captures, %d synthetic modes, %d chunked, 1 overflow)"
           " + 2 chunk-invariance sweeps, %d passed, %d failed\n",
           total_checks, real_cases, synth_cases, chunk_cases, passed, failed);

    return (failed == 0) ? 0 : 1;
}
