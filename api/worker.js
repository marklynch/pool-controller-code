// api.poolcontrol.io — backend for the pool controller firmware.
//
// Anonymous usage reporting:
//   POST   /v1/usage       {"id":"<16 lowercase hex>","v":"<firmware version>"}
//   DELETE /v1/usage/<id>  sent when the user opts out: remove that install's rows
//
// Any other path outside /v1/ redirects to the GitHub repo.
//
// Stores the install ID, the version (in full, and as vMAJOR.MINOR.PATCH for
// aggregation), and the country/continent Cloudflare derives from the
// request. Deliberately stores and logs nothing else: no IP address, city,
// region, postcode, coordinates, timezone, ASN or ISP.

const PATH = "/v1/usage";
const API_PREFIX = "/v1/";
// Where a browser opening any other page (the bare domain, say) is sent.
// 302 rather than 301, so browsers don't cache it and it can change later.
const REDIRECT_URL = "https://github.com/marklynch/pool-controller-code";
const DELETE_PATH_RE = /^\/v1\/usage\/([0-9a-f]{16})$/;
const MAX_BODY = 256;
const ID_RE = /^[0-9a-f]{16}$/;
const VERSION_RE = /^[\w.\-+]{1,48}$/;
// The release a build is based on: "v1.20.1-7-gc410103-dirty" -> "v1.20.1".
const CLEAN_VERSION_RE = /^v?(\d+\.\d+\.\d+)(?:[-+].*)?$/i;

const empty = (status) => new Response(null, { status });

// vMAJOR.MINOR.PATCH, matching the release tags, for aggregation; null when
// the version doesn't parse. The "v" is always added, so a version sent
// without one still groups with the tagged release. The full string is
// stored alongside it for debugging.
const cleanVersion = (v) => {
  const m = CLEAN_VERSION_RE.exec(v);
  return m ? `v${m[1]}` : null;
};

async function ping(req, env) {
  // Reject an oversized body from its header before reading it.
  const declared = Number(req.headers.get("content-length") ?? 0);
  if (declared > MAX_BODY) return empty(413);
  const text = await req.text();
  if (text.length > MAX_BODY) return empty(413);

  let b;
  try { b = JSON.parse(text); } catch { return empty(400); }
  if (b === null || typeof b !== "object" ||
      typeof b.id !== "string" || !ID_RE.test(b.id) ||
      typeof b.v  !== "string" || !VERSION_RE.test(b.v)) {
    return empty(400);
  }

  const country   = req.cf?.country   ?? "";
  const continent = req.cf?.continent ?? "";
  const clean     = cleanVersion(b.v);

  try {
    // One transaction: both rows change together or not at all.
    await env.DB.batch([
      env.DB.prepare(
        `INSERT INTO devices (id, version, clean_version, country, continent, first_seen, last_seen)
         VALUES (?1, ?2, ?3, ?4, ?5, unixepoch(), unixepoch())
         ON CONFLICT(id) DO UPDATE SET
           version=?2, clean_version=?3, country=?4, continent=?5, last_seen=unixepoch(),
           ping_count=ping_count+1`
      ).bind(b.id, b.v, clean, country, continent),
      env.DB.prepare(
        `INSERT INTO device_versions (id, version, clean_version, first_seen, last_seen)
         VALUES (?1, ?2, ?3, unixepoch(), unixepoch())
         ON CONFLICT(id, version) DO UPDATE SET last_seen=unixepoch()`
      ).bind(b.id, b.v, clean),
    ]);
  } catch {
    // Pings aren't retried, so there is nothing useful to say beyond "not stored".
    return empty(503);
  }
  return empty(204);
}

// Idempotent: 204 whether or not the install had any rows, so a device that
// retries after a lost response still gets its confirmation.
async function forget(id, env) {
  try {
    await env.DB.batch([
      env.DB.prepare(`DELETE FROM device_versions WHERE id = ?1`).bind(id),
      env.DB.prepare(`DELETE FROM devices WHERE id = ?1`).bind(id),
    ]);
  } catch {
    return empty(503);   // The device keeps the request pending and tries again
  }
  return empty(204);
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);

    if (url.pathname === PATH) {
      return req.method === "POST" ? ping(req, env) : empty(405);
    }

    const m = DELETE_PATH_RE.exec(url.pathname);
    if (m) {
      return req.method === "DELETE" ? forget(m[1], env) : empty(405);
    }

    // A wrong API path is a client bug: a plain 404 says so, where a redirect
    // to a web page would only confuse it.
    if (url.pathname.startsWith(API_PREFIX)) return empty(404);

    return Response.redirect(REDIRECT_URL, 302);
  },
};
