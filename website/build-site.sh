#!/usr/bin/env bash
#
# Assemble the poolcontrol.io site: the home page at the root, and the web
# installer under install/ — its page, one manifest and firmware binary per
# offered release, and a versions.json index it reads to build its version
# picker.
#
#   ./website/build-site.sh --out site                   # latest 5 releases
#   ./website/build-site.sh --out site --limit 2
#   ./website/build-site.sh --out site --bin build/x.bin --version dev
#
# Used by both .github/workflows/pages.yml and website/preview.sh, so what
# you preview locally is exactly what gets deployed.
set -euo pipefail

REPO="${REPO:-marklynch/pool-controller-code}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIMIT=5
OUT=""
LOCAL_BIN=""
LOCAL_VERSION=""

# ESP Web Tools is vendored into the site rather than loaded from a CDN. The
# page holds a live Web Serial port and writes a full flash image, so whatever
# JavaScript it loads decides which bytes reach the device — that code must be
# pinned and verified, not resolved at page load from a third-party origin.
#
# EWT_INTEGRITY is the npm registry's published dist.integrity for this exact
# version. Published npm versions are immutable, so a mismatch means the
# tarball was tampered with in transit or the registry was compromised; either
# way the build fails rather than shipping it.
#
# To upgrade: bump EWT_VERSION, then take the new hash from
#   curl -s https://registry.npmjs.org/esp-web-tools/<version> | jq -r .dist.integrity
# and re-test flashing before merging.
EWT_VERSION="10.4.0"
EWT_INTEGRITY="sha512-3pwkeFFm5Fj7UQo8SJNYK5RXrtNCpq6X9QoI6bMT4GBZWgrJqjn0YvM9ihG74BtMoSFYXfmDtkehuxe50PTMPQ=="

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --limit) LIMIT="$2"; shift 2 ;;
    --bin) LOCAL_BIN="$2"; shift 2 ;;
    --version) LOCAL_VERSION="$2"; shift 2 ;;
    -h|--help) sed -n '2,13p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

[ -n "$OUT" ] || { echo "--out is required" >&2; exit 1; }

rm -rf "$OUT"
# Everything installer-related lives under install/; the home page and shared
# stylesheet sit at the root.
INSTALL="$OUT/install"
mkdir -p "$INSTALL"

# Collected as "tag<TAB>published" lines, newest first.
SELECTED=""

if [ -n "$LOCAL_BIN" ]; then
  [ -f "$LOCAL_BIN" ] || { echo "no such file: $LOCAL_BIN" >&2; exit 1; }
  VERSION="${LOCAL_VERSION:-local}"
  cp "$LOCAL_BIN" "$INSTALL/pool-controller-full-${VERSION}.bin"
  SELECTED="${VERSION}	"
else
  command -v gh >/dev/null || {
    echo "gh CLI not found — install it, or pass --bin <path> for a local build." >&2
    exit 1
  }

  # Over-fetch: drafts, prereleases and any release predating the merged
  # full-flash asset are skipped, and we still want LIMIT usable ones.
  CANDIDATES=$(gh release list --repo "$REPO" --limit $((LIMIT * 3)) \
    --json tagName,publishedAt,isDraft,isPrerelease \
    -q '.[] | select(.isDraft == false and .isPrerelease == false)
        | "\(.tagName)\t\(.publishedAt)"')

  count=0
  while IFS=$'\t' read -r tag published; do
    [ -n "$tag" ] || continue
    [ "$count" -lt "$LIMIT" ] || break
    binary="pool-controller-full-${tag}.bin"
    if gh release download "$tag" --repo "$REPO" --pattern "$binary" --dir "$INSTALL" 2>/dev/null; then
      SELECTED="${SELECTED}${tag}	${published}"$'\n'
      count=$((count + 1))
      echo "  + ${tag} ($(du -h "$INSTALL/$binary" | cut -f1))"
    else
      echo "  - ${tag} skipped (no ${binary})" >&2
    fi
  done <<< "$CANDIDATES"

  [ "$count" -gt 0 ] || { echo "no releases with a full-flash binary found" >&2; exit 1; }
fi

# One manifest per version. ESP Web Tools reads the manifest at click time, so
# the page just swaps the button's manifest attribute when the picker changes.
while IFS=$'\t' read -r tag published; do
  [ -n "$tag" ] || continue
  sed -e "s|__VERSION__|${tag}|g" \
      -e "s|__BINARY__|pool-controller-full-${tag}.bin|g" \
      "$SRC/install/manifest.template.json" > "$INSTALL/manifest-${tag}.json"
  if grep -q '__VERSION__\|__BINARY__' "$INSTALL/manifest-${tag}.json"; then
    echo "manifest-${tag}.json still contains unsubstituted placeholders" >&2
    exit 1
  fi
  test -s "$INSTALL/pool-controller-full-${tag}.bin"
done <<< "$SELECTED"

LATEST=$(printf '%s' "$SELECTED" | head -n1 | cut -f1)

REPO="$REPO" LATEST="$LATEST" SELECTED="$SELECTED" OUT="$INSTALL" python3 - <<'PY'
import json, os

repo = os.environ["REPO"]
out = os.environ["OUT"]
versions = []
for line in os.environ["SELECTED"].splitlines():
    if not line.strip():
        continue
    tag, _, published = line.partition("\t")
    versions.append({
        "version": tag,
        "manifest": f"manifest-{tag}.json",
        "published": published[:10],
        "notes": f"https://github.com/{repo}/releases/tag/{tag}",
    })

with open(os.path.join(out, "versions.json"), "w") as f:
    json.dump({"latest": os.environ["LATEST"], "versions": versions}, f, indent=2)
    f.write("\n")
PY

cp "$SRC/index.html" "$SRC/site.css" "$OUT/"
cp "$SRC/install/index.html" "$SRC/install/app.js" "$INSTALL/"
# Kept so a direct link to manifest.json still resolves to the current release.
cp "$INSTALL/manifest-${LATEST}.json" "$INSTALL/manifest.json"

# --- Vendor ESP Web Tools ---------------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Fetching esp-web-tools ${EWT_VERSION} …"
curl -fsSL --retry 3 \
  "https://registry.npmjs.org/esp-web-tools/-/esp-web-tools-${EWT_VERSION}.tgz" \
  -o "$TMP/ewt.tgz"

ACTUAL=$(python3 -c '
import base64, hashlib, sys
print("sha512-" + base64.b64encode(hashlib.sha512(open(sys.argv[1], "rb").read()).digest()).decode())
' "$TMP/ewt.tgz")

if [ "$ACTUAL" != "$EWT_INTEGRITY" ]; then
  echo "esp-web-tools integrity check FAILED — refusing to build." >&2
  echo "  expected: $EWT_INTEGRITY" >&2
  echo "  actual:   $ACTUAL" >&2
  exit 1
fi

tar xzf "$TMP/ewt.tgz" -C "$TMP" package/dist/web
mkdir -p "$INSTALL/ewt"
cp -R "$TMP/package/dist/web/." "$INSTALL/ewt/"
# The page loads this entry point; the rest are its lazily imported chunks.
test -s "$INSTALL/ewt/install-button.js"
echo "  vendored esp-web-tools ${EWT_VERSION} (integrity verified)"

echo "Built $OUT — latest ${LATEST}, $(grep -c '"version"' "$INSTALL/versions.json") version(s)."
