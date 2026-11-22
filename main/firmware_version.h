#ifndef FIRMWARE_VERSION_H
#define FIRMWARE_VERSION_H

#include <stdbool.h>

// ======================================================
// Firmware version tags: parsing, ordering and picking the release to offer
//
// Understands release tags (v1.22.1), release candidates (v2.0.0-rc1) and
// `git describe` builds of either (v1.22.1-12-gabc1234-dirty,
// v2.0.0-rc1-3-gabc1234). A describe suffix is ignored, so a dev build
// compares equal to the tag it was built from. Ordering:
//   v1.22.1 < v2.0.0-rc1 < v2.0.0-rc2 < v2.0.0
// Pure C with no ESP-IDF dependencies, so it is host-tested.
// ======================================================

typedef struct {
    int major;
    int minor;
    int patch;
    bool prerelease;   // Tag carries an "-rc<N>" suffix
    int rc;            // N of "-rc<N>" (0 if absent or no number); 0 when !prerelease
} fw_version_t;

// Parse a version string. Tolerates a leading 'v' and a trailing describe
// suffix. Anything without a full major.minor.patch is 0.0.0 and not a
// pre-release (e.g. an untagged build's bare hash, even "8d0e5f1").
void firmware_version_parse(const char *s, fw_version_t *out);

// True if `s` is a release candidate (e.g. v2.0.0-rc1, v2.0.0-rc1-3-gabc1234).
bool firmware_version_is_prerelease(const char *s);

// Returns <0 if a<b, 0 if equal, >0 if a>b.
int firmware_version_cmp(const char *a, const char *b);

// Pick which of `tags` (as read from the release feed) to offer as the
// latest release for `installed`. Returns its index, or -1 if none qualifies.
//
// Release candidates are opt-in: only stable releases are offered, unless the
// installed firmware is itself a release candidate, in which case a newer
// release candidate qualifies too. The highest qualifying version wins,
// whatever its position in the feed.
//
// If no stable release is in `tags` (the feed holds only release candidates,
// e.g. after a burst of rcs pushed the last stable release out of the
// window), a stable install gets -1: nothing is offered, rather than an rc
// it never opted into. The rcs stay installable by explicit choice.
int firmware_version_select_latest(const char *installed,
                                   const char *const tags[], int count);

#endif // FIRMWARE_VERSION_H
