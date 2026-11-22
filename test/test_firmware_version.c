/**
 * Unit tests for firmware version parsing, ordering and the choice of
 * release to offer (main/firmware_version.c).
 *
 * To run:
 *   cd test && gcc -I. -I.. -o run_firmware_version test_firmware_version.c ../main/firmware_version.c && ./run_firmware_version
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "../main/firmware_version.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  ✓ PASS: %s\n", message); \
            tests_passed++; \
        } else { \
            printf("  ✗ FAIL: %s\n", message); \
            tests_failed++; \
        } \
    } while(0)

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int sign(int v)
{
    return (v > 0) - (v < 0);
}

static void expect_cmp(const char *a, const char *b, int expected)
{
    char msg[128];
    const char *op = expected < 0 ? "<" : expected > 0 ? ">" : "==";
    snprintf(msg, sizeof(msg), "%s %s %s", a, op, b);
    TEST_ASSERT(sign(firmware_version_cmp(a, b)) == expected, msg);
    // Ordering must be antisymmetric.
    snprintf(msg, sizeof(msg), "%s %s %s (reversed)", b,
             expected < 0 ? ">" : expected > 0 ? "<" : "==", a);
    TEST_ASSERT(sign(firmware_version_cmp(b, a)) == -expected, msg);
}

// Returns the tag chosen for `installed`, or NULL if none.
static const char *select_tag(const char *installed, const char *const tags[], int count)
{
    int idx = firmware_version_select_latest(installed, tags, count);
    return idx >= 0 ? tags[idx] : NULL;
}

static void expect_select(const char *installed, const char *const tags[], int count,
                          const char *expected, const char *what)
{
    const char *got = select_tag(installed, tags, count);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: installed %s -> %s (got %s)", what, installed,
             expected ? expected : "none", got ? got : "none");
    TEST_ASSERT((expected == NULL && got == NULL) ||
                (expected && got && strcmp(expected, got) == 0), msg);
}

static void test_parse(void)
{
    printf("\nTest: parse\n");
    fw_version_t v;

    firmware_version_parse("v1.22.1", &v);
    TEST_ASSERT(v.major == 1 && v.minor == 22 && v.patch == 1 && !v.prerelease,
                "v1.22.1 is a 1.22.1 release");

    firmware_version_parse("v2.0.0-rc1", &v);
    TEST_ASSERT(v.major == 2 && v.minor == 0 && v.patch == 0 && v.prerelease && v.rc == 1,
                "v2.0.0-rc1 is 2.0.0 rc 1");

    firmware_version_parse("v2.0.0-rc12", &v);
    TEST_ASSERT(v.prerelease && v.rc == 12, "v2.0.0-rc12 is rc 12");

    firmware_version_parse("v1.22.1-12-gabc1234", &v);
    TEST_ASSERT(v.major == 1 && v.minor == 22 && v.patch == 1 && !v.prerelease,
                "describe suffix is not a pre-release");

    firmware_version_parse("v1.22.1-12-gabc1234-dirty", &v);
    TEST_ASSERT(v.patch == 1 && !v.prerelease, "dirty describe suffix is not a pre-release");

    firmware_version_parse("v1.22.1-dirty", &v);
    TEST_ASSERT(v.patch == 1 && !v.prerelease, "dirty build on a release tag is not a pre-release");

    firmware_version_parse("v2.0.0-rc1-3-gabc1234", &v);
    TEST_ASSERT(v.major == 2 && v.prerelease && v.rc == 1, "describe build of an rc keeps rc 1");

    firmware_version_parse("v2.0.0-rc1-3-gabc1234-dirty", &v);
    TEST_ASSERT(v.prerelease && v.rc == 1, "dirty describe build of an rc keeps rc 1");

    firmware_version_parse("1.2.3", &v);
    TEST_ASSERT(v.major == 1 && v.minor == 2 && v.patch == 3, "leading v is optional");

    firmware_version_parse("abc1234", &v);
    TEST_ASSERT(v.major == 0 && v.minor == 0 && v.patch == 0 && !v.prerelease,
                "untagged bare hash is 0.0.0");

    firmware_version_parse("8d0e5f1", &v);
    TEST_ASSERT(v.major == 0 && v.minor == 0 && v.patch == 0 && !v.prerelease,
                "untagged hash starting with digits is 0.0.0, not 8.0.0");

    firmware_version_parse("1234567-dirty", &v);
    TEST_ASSERT(v.major == 0 && !v.prerelease, "all-digit dirty hash is 0.0.0");

    firmware_version_parse("v1.2", &v);
    TEST_ASSERT(v.major == 0 && v.minor == 0 && v.patch == 0, "partial version is 0.0.0");

    firmware_version_parse(NULL, &v);
    TEST_ASSERT(v.major == 0 && !v.prerelease, "NULL is 0.0.0");

    TEST_ASSERT(firmware_version_is_prerelease("v2.0.0-rc2"), "is_prerelease: v2.0.0-rc2");
    TEST_ASSERT(!firmware_version_is_prerelease("v2.0.0"), "is_prerelease: v2.0.0 is not");
    TEST_ASSERT(!firmware_version_is_prerelease("v1.22.1-12-gabc1234"),
                "is_prerelease: describe build is not");
}

static void test_compare(void)
{
    printf("\nTest: compare\n");
    // The ordering from issue #166.
    expect_cmp("v1.22.1", "v2.0.0-rc1", -1);
    expect_cmp("v2.0.0-rc1", "v2.0.0-rc2", -1);
    expect_cmp("v2.0.0-rc2", "v2.0.0", -1);
    expect_cmp("v2.0.0-rc1", "v2.0.0", -1);
    expect_cmp("v2.0.0-rc9", "v2.0.0-rc10", -1);

    // Plain release ordering is unchanged.
    expect_cmp("v1.22.0", "v1.22.1", -1);
    expect_cmp("v1.9.0", "v1.22.0", -1);
    expect_cmp("v1.22.1", "v1.22.1", 0);

    // Describe suffixes compare equal to their tag.
    expect_cmp("v1.22.1-12-gabc1234", "v1.22.1", 0);
    expect_cmp("v1.22.1-12-gabc1234-dirty", "v1.22.1", 0);
    expect_cmp("v2.0.0-rc1-3-gabc1234", "v2.0.0-rc1", 0);
    expect_cmp("v2.0.0-rc1-3-gabc1234", "v2.0.0-rc2", -1);
    expect_cmp("v2.0.0-rc1-3-gabc1234", "v2.0.0", -1);
}

static void test_select(void)
{
    printf("\nTest: select latest\n");

    const char *const rc_newest[] = { "v2.0.0-rc1", "v1.22.1", "v1.22.0" };
    expect_select("v1.22.1", rc_newest, ARRAY_LEN(rc_newest), "v1.22.1",
                  "rc newest in feed, stable install");
    TEST_ASSERT(firmware_version_cmp("v1.22.1",
                                     select_tag("v1.22.1", rc_newest, ARRAY_LEN(rc_newest))) == 0,
                "rc newest in feed: no update for the current stable install");
    expect_select("v1.22.0", rc_newest, ARRAY_LEN(rc_newest), "v1.22.1",
                  "rc newest in feed, older stable install");

    const char *const final_after_rc[] = { "v2.0.0", "v2.0.0-rc2", "v2.0.0-rc1", "v1.22.1" };
    expect_select("v1.22.1", final_after_rc, ARRAY_LEN(final_after_rc), "v2.0.0",
                  "final after rc, stable install");
    expect_select("v2.0.0-rc2", final_after_rc, ARRAY_LEN(final_after_rc), "v2.0.0",
                  "final after rc, rc install is offered the final");
    expect_select("v2.0.0-rc1-3-gabc1234", final_after_rc, ARRAY_LEN(final_after_rc), "v2.0.0",
                  "final after rc, rc describe build is offered the final");

    const char *const two_rcs[] = { "v2.0.0-rc2", "v2.0.0-rc1", "v1.22.1" };
    expect_select("v2.0.0-rc1", two_rcs, ARRAY_LEN(two_rcs), "v2.0.0-rc2",
                  "rc install is offered the next rc");
    expect_select("v2.0.0-rc2", two_rcs, ARRAY_LEN(two_rcs), "v2.0.0-rc2",
                  "rc install on the newest rc: latest is itself");
    expect_select("v1.22.1", two_rcs, ARRAY_LEN(two_rcs), "v1.22.1",
                  "two rcs, stable install skips both");

    const char *const describe[] = { "v1.22.1", "v1.22.0" };
    expect_select("v1.22.1-12-gabc1234", describe, ARRAY_LEN(describe), "v1.22.1",
                  "describe build");
    TEST_ASSERT(firmware_version_cmp("v1.22.1-12-gabc1234-dirty",
                                     select_tag("v1.22.1-12-gabc1234-dirty", describe,
                                                ARRAY_LEN(describe))) == 0,
                "dirty describe build: no update");

    const char *const only_rcs[] = { "v2.0.0-rc3", "v2.0.0-rc2", "v2.0.0-rc1" };
    expect_select("v1.22.1", only_rcs, ARRAY_LEN(only_rcs), NULL,
                  "feed of only rcs, stable install is offered nothing");
    expect_select("v2.0.0-rc1", only_rcs, ARRAY_LEN(only_rcs), "v2.0.0-rc3",
                  "feed of only rcs, rc install is offered the newest rc");

    // Highest version wins, not feed position.
    const char *const out_of_order[] = { "v1.21.4", "v1.22.1", "v1.22.0" };
    expect_select("v1.0.0", out_of_order, ARRAY_LEN(out_of_order), "v1.22.1",
                  "highest stable wins regardless of feed order");

    expect_select("v1.22.1", NULL, 0, NULL, "empty feed");
    expect_select("8d0e5f1", describe, ARRAY_LEN(describe), "v1.22.1",
                  "untagged build with a digit-led hash is offered the release");
    TEST_ASSERT(firmware_version_cmp("8d0e5f1", "v1.22.1") < 0,
                "untagged build with a digit-led hash is older than any release");
}

int main(void)
{
    printf("========================================\n");
    printf("  Firmware version tests\n");
    printf("========================================\n");

    test_parse();
    test_compare();
    test_select();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");
    return tests_failed > 0 ? 1 : 0;
}
