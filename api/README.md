# api.poolcontrol.io

The Cloudflare Worker behind `api.poolcontrol.io`. For now it serves one
endpoint, anonymous usage reporting: a D1 database that counts active pool
controller installs by firmware version and coarse location (country and
continent). It runs on the Workers free tier, with no servers or dashboards to
look after.

The firmware side is `main/usage_report.c`. What each device sends, and how a
user turns it off, is described in the main [README](../README.md#usage-reporting).

## API

### Report

`POST https://api.poolcontrol.io/v1/usage`

```json
{"id":"9f3a1c0b7e2d4a58","v":"1.8.2"}
```

| Response | Meaning |
|---|---|
| 204 | Stored |
| 400 | Body isn't JSON, or `id` isn't 16 lowercase hex chars, or `v` isn't `[\w.\-+]{1,48}` |
| 404 / 405 | Wrong path / wrong method |
| 413 | Body over 256 bytes |
| 503 | Database write failed |

### Delete

`DELETE https://api.poolcontrol.io/v1/usage/<id>`

Sent by a device when the user turns usage reporting off. Removes the
install's row from `devices` and all its rows from `device_versions`. Returns
204 whether or not anything was there, so a device retrying after a lost
response still gets its confirmation; 503 if the delete failed, and the device
tries again on its next run. The device keeps its ID, so if reporting is
turned back on it reappears as a new row under the same ID.

Like pings, deletes aren't authenticated, but an ID is never shown anywhere,
so in practice only the device that made it can delete it.

### What's stored

The Worker adds `request.cf.country` and `request.cf.continent`. It does not
store or log the client IP, city, region, postcode, coordinates, timezone, ASN
or ISP, and Workers observability is turned off in `wrangler.toml` so Cloudflare
doesn't keep request logs either.

## Deploy

Needs a Cloudflare account with `poolcontrol.io` on it, and
[wrangler](https://developers.cloudflare.com/workers/wrangler/).

```bash
cd api
npx wrangler login

# 1. Create the database, then paste the printed database_id into wrangler.toml
npx wrangler d1 create pool-controller-usage

# 2. Create the tables (safe to re-run)
npx wrangler d1 execute pool-controller-usage --remote --file schema.sql

# 3. Deploy. The custom_domain route creates the api.poolcontrol.io DNS record
#    and certificate.
npx wrangler deploy
```

Smoke test:

```bash
curl -i -X POST https://api.poolcontrol.io/v1/usage \
  -H 'Content-Type: application/json' \
  -d '{"id":"0000000000000000","v":"test"}'      # expect 204

# then remove the test rows the same way a device does when it opts out
curl -i -X DELETE https://api.poolcontrol.io/v1/usage/0000000000000000   # expect 204
```

Optional: add a Cloudflare rate-limiting rule on `/v1/usage` (for example 10
requests a minute per IP) to cap junk writes.

## Queries

Run with `npx wrangler d1 execute pool-controller-usage --remote --command "<sql>"`.

Each row carries the version twice: `version` is the full string the device
sent (`v1.20.1-7-gc410103-dirty`), kept for debugging, and `clean_version` is
the release tag it's based on (`v1.20.1`), which the queries below aggregate
on. The Worker always adds the `v`, so a version sent without one still groups
with its release. A development build therefore counts towards the release
it was built from. `device_versions` keeps one row per full string, so the
history queries first collapse it to one row per (device, release).

```sql
-- Active installs by current release (last 7 days)
SELECT clean_version, COUNT(*) AS devices FROM devices
WHERE last_seen > unixepoch() - 7*86400
GROUP BY clean_version ORDER BY devices DESC;

-- Active installs by country / continent (last 7 days)
SELECT continent, country, COUNT(*) AS devices FROM devices
WHERE last_seen > unixepoch() - 7*86400
GROUP BY continent, country ORDER BY devices DESC;

-- Every device that has ever run each release
SELECT clean_version, COUNT(DISTINCT id) AS devices,
       datetime(MIN(first_seen), 'unixepoch') AS first_seen
FROM device_versions WHERE clean_version IS NOT NULL
GROUP BY clean_version ORDER BY MIN(first_seen) DESC;

-- Adoption curve for a release
WITH r AS (
  SELECT id, MIN(first_seen) AS first_seen FROM device_versions
  WHERE clean_version = 'v1.20.1' GROUP BY id)
SELECT date(first_seen, 'unixepoch') AS day, COUNT(*) AS devices
FROM r GROUP BY day ORDER BY day;

-- Upgrade paths (release -> next release)
WITH r AS (
  SELECT id, clean_version, MIN(first_seen) AS first_seen FROM device_versions
  WHERE clean_version IS NOT NULL GROUP BY id, clean_version)
SELECT a.clean_version AS from_v, b.clean_version AS to_v, COUNT(*) AS n
FROM r a JOIN r b
  ON a.id = b.id AND b.first_seen > a.first_seen
WHERE NOT EXISTS (
  SELECT 1 FROM r c
  WHERE c.id = a.id AND c.first_seen > a.first_seen AND c.first_seen < b.first_seen)
GROUP BY 1, 2 ORDER BY n DESC;

-- Debugging: active installs not running a release build (dev builds, or
-- versions that didn't parse), by full version string
SELECT version, clean_version, COUNT(*) AS devices FROM devices
WHERE last_seen > unixepoch() - 7*86400
  AND (clean_version IS NULL OR version != clean_version)
GROUP BY version, clean_version ORDER BY devices DESC;
```

### Spotting fake pings

There's no authentication: any secret in public firmware could be extracted, so
anyone can send pings. These two queries make it easy to find junk, which can
then be deleted by hand.

```sql
-- IDs pinging far more often than expected. A healthy device sends 2 pings a
-- day (one per 12-hourly update check), plus 1 per reboot and 1 per press of
-- Home Assistant's update-check button, so more than ~10 a day sustained is
-- suspicious.
SELECT id, version, country, ping_count,
       ROUND(ping_count / MAX((last_seen - first_seen) / 86400.0, 1), 1) AS pings_per_day
FROM devices
WHERE ping_count > 20
ORDER BY pings_per_day DESC LIMIT 50;

-- New IDs per day. Someone rotating IDs shows up as a spike here (each fake
-- ID has ping_count = 1) rather than in the query above.
SELECT date(first_seen, 'unixepoch') AS day,
       COUNT(*) AS new_ids,
       SUM(ping_count = 1 AND last_seen < unixepoch() - 3*86400) AS seen_once_only
FROM devices GROUP BY day ORDER BY day DESC LIMIT 30;
```

## Retention

Rows are kept indefinitely; the volume is tiny. To prune installs not seen for
a year:

```sql
DELETE FROM device_versions WHERE id IN
  (SELECT id FROM devices WHERE last_seen < unixepoch() - 365*86400);
DELETE FROM devices WHERE last_seen < unixepoch() - 365*86400;
```

## Known limitations

- Counts can be inflated by fake pings (see above).
- A downgrade and re-upgrade (1.8 → 1.7 → 1.8) reuses the device's existing
  `1.8` row, so its `first_seen` is the first time that device ran 1.8.
- A factory reset makes a new install ID, so it counts as a new install.
- Devices without internet access, or with reporting turned off, aren't
  counted. GitHub's release-asset download counts are a useful cross-check.
