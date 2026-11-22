#include "firmware_version.h"

#include <ctype.h>
#include <stddef.h>

// Release-candidate marker following major.minor.patch, as in "v2.0.0-rc1"
#define RC_SUFFIX "-rc"
#define RC_SUFFIX_LEN (sizeof(RC_SUFFIX) - 1)

// Read a run of decimal digits at *s into *out, advancing *s past them.
// Returns false (and leaves *out untouched) if *s is not at a digit.
static bool read_number(const char **s, int *out)
{
    if (!isdigit((unsigned char)**s)) return false;
    int v = 0;
    while (isdigit((unsigned char)**s)) {
        v = v * 10 + (**s - '0');
        (*s)++;
    }
    *out = v;
    return true;
}

void firmware_version_parse(const char *s, fw_version_t *out)
{
    *out = (fw_version_t){0};
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;

    // All three parts or nothing: an untagged build's version is a bare
    // commit hash, whose leading digits ("8d0e5f1") are not a major version.
    int parts[3];
    for (int i = 0; i < 3; i++) {
        if (!read_number(&s, &parts[i])) return;
        if (i < 2) {
            if (*s != '.') return;
            s++;
        }
    }
    out->major = parts[0];
    out->minor = parts[1];
    out->patch = parts[2];

    // A describe suffix starts "-<digits>-g<hash>", so a '-' followed by a
    // digit is never a pre-release; "-rc" must be followed by its number,
    // the end, or the describe suffix of an rc build ("-rc1-3-gabc1234").
    for (size_t i = 0; i < RC_SUFFIX_LEN; i++) {
        if (tolower((unsigned char)s[i]) != RC_SUFFIX[i]) return;
    }
    s += RC_SUFFIX_LEN;
    int rc = 0;
    read_number(&s, &rc);
    if (*s != '\0' && *s != '-') return;
    out->prerelease = true;
    out->rc = rc;
}

bool firmware_version_is_prerelease(const char *s)
{
    fw_version_t v;
    firmware_version_parse(s, &v);
    return v.prerelease;
}

static int version_cmp(const fw_version_t *a, const fw_version_t *b)
{
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    // A release sorts after all of its release candidates.
    if (a->prerelease != b->prerelease) return a->prerelease ? -1 : 1;
    if (a->rc != b->rc) return a->rc < b->rc ? -1 : 1;
    return 0;
}

int firmware_version_cmp(const char *a, const char *b)
{
    fw_version_t va, vb;
    firmware_version_parse(a, &va);
    firmware_version_parse(b, &vb);
    return version_cmp(&va, &vb);
}

int firmware_version_select_latest(const char *installed,
                                   const char *const tags[], int count)
{
    fw_version_t inst;
    firmware_version_parse(installed, &inst);

    int best = -1;
    fw_version_t best_v = {0};
    for (int i = 0; i < count; i++) {
        fw_version_t v;
        firmware_version_parse(tags[i], &v);
        // An rc qualifies only for an rc install, and only if it is not older
        // than what is installed (so the installed rc itself can be "latest").
        if (v.prerelease && !(inst.prerelease && version_cmp(&v, &inst) >= 0)) {
            continue;
        }
        if (best < 0 || version_cmp(&v, &best_v) > 0) {
            best = i;
            best_v = v;
        }
    }
    return best;
}
