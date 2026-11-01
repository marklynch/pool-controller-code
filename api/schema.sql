-- Anonymous usage reporting. One row per install, plus one row per
-- (install, firmware version) pair. Nothing else is stored: no IP address,
-- city, region, ASN or anything about the pool.
--
-- version is the full string the device sent (e.g. "v1.20.1-7-gc410103-dirty"),
-- kept for debugging. clean_version is the release it is based on ("v1.20.1"),
-- derived by the Worker and used for aggregation; NULL if it didn't parse.

CREATE TABLE IF NOT EXISTS devices (
  id            TEXT PRIMARY KEY,           -- random install ID from the device (16 hex chars)
  version       TEXT NOT NULL,              -- current firmware version, in full
  clean_version TEXT,                       -- current firmware version, vMAJOR.MINOR.PATCH
  country       TEXT,                       -- request.cf.country (XX = unknown, T1 = Tor)
  continent     TEXT,                       -- request.cf.continent
  first_seen    INTEGER NOT NULL,           -- unix seconds
  last_seen     INTEGER NOT NULL,
  ping_count    INTEGER NOT NULL DEFAULT 1  -- total pings from this ID (spam signal)
);

CREATE TABLE IF NOT EXISTS device_versions (
  id            TEXT NOT NULL,
  version       TEXT NOT NULL,              -- full string, so each dev build gets its own row
  clean_version TEXT,
  first_seen    INTEGER NOT NULL,           -- first ping on this version
  last_seen     INTEGER NOT NULL,           -- last ping on this version
  PRIMARY KEY (id, version)
);

CREATE INDEX IF NOT EXISTS idx_dv_clean_version ON device_versions(clean_version);
CREATE INDEX IF NOT EXISTS idx_devices_last_seen ON devices(last_seen);
