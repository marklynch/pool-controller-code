# Website

<https://poolcontrol.io> is the project website, served by an assets-only Cloudflare Worker built from `website/`. It has two pages:

- **`/`** — a home page summarising the project, with links out to the README, hardware repos and protocol notes. Static HTML and CSS, no scripts.
- **`/install/`** — the web installer, described below.
- **`/usage/`** — what the anonymous usage report sends, what's stored, and how to turn it off. `api.poolcontrol.io` redirects browsers here, so keep the path stable.

All pages share `website/site.css`.

## Web installer

<https://poolcontrol.io/install/> flashes the latest release onto an ESP32-C6 straight from the browser using [ESP Web Tools](https://github.com/esphome/esp-web-tools) and Web Serial — no ESP-IDF toolchain, no esptool. It needs desktop Chrome or Edge; Safari, Firefox and mobile browsers have no Web Serial support and get a "flash manually" fallback instead.

The page offers the **5 most recent releases** in a version picker, defaulting to the latest. Picking an older one shows a rollback warning. ESP Web Tools reads the manifest at click time, so switching versions just re-points the install button at that version's manifest.

## Page source

- `website/index.html` — the home page.
- `website/site.css` — styles shared by every page. In a file rather than inline so pages can keep a `style-src 'self'` CSP.
- `website/usage/index.html` — the usage-reporting page. Static, like the home page.
- `website/install/index.html` — the installer page. Static; it reads `versions.json` at runtime to build the picker, so it needs no build step.
- `website/install/app.js` — the installer's script. Kept out of the HTML so the page's CSP can be `script-src 'self'` with no `'unsafe-inline'`.
- `website/install/manifest.template.json` — the ESP Web Tools manifest, with `__VERSION__` / `__BINARY__` placeholders stamped once per offered version.
- `website/build-site.sh` — assembles the deployable site.
- `website/preview.sh` — builds it and serves it locally.
- `website/_headers` — response headers for every file, applied by Cloudflare.
- `website/wrangler.toml` — the Worker: name, custom domains, and `dist/` as its assets directory.

The published site is *not* these files as-is. `build-site.sh` queries the recent releases, downloads each `pool-controller-full-<tag>.bin`, and emits per-version manifests plus a `versions.json` index — none of which exist in the repo. Both the workflow and `preview.sh` call that one script, so a local preview is exactly what gets deployed. Serving the binaries same-origin rather than linking them from the releases keeps the flasher's fetch simple.

Releases without a `pool-controller-full-*.bin` asset are skipped, and the script keeps looking further back until it has 5 usable ones. Change the count via the `INSTALLER_VERSIONS` env in `website.yml`; at ~1.5 MB per binary, 5 versions is roughly a 7.5 MB site.

A deploy always publishes the newest 5 releases regardless of which tag triggered it, so re-running an old tag's build can't roll the public installer back to that version.

## Vendored flasher

ESP Web Tools is **vendored into the site**, not loaded from a CDN. The page holds a live Web Serial port and writes a full flash image, so the JavaScript it loads decides which bytes reach the device — that code is pinned and verified rather than resolved at page load from a third-party origin. A floating `@10` CDN range would mean every visitor executing whatever was published to npm most recently, with no review and no deploy step in between.

`build-site.sh` downloads the exact `EWT_VERSION` tarball from the npm registry, checks it against the registry's published `dist.integrity` hash pinned in `EWT_INTEGRITY`, and unpacks it to `install/ewt/`. A mismatch aborts the build. To upgrade, bump both:

```bash
curl -s https://registry.npmjs.org/esp-web-tools/<version> | jq -r .dist.integrity
```

then re-test flashing on real hardware before merging.

With every asset same-origin, the page sets a `default-src 'none'` CSP allowing `script-src 'self'` and no external origins, so nothing off-origin can inject code into the flashing page. That CSP is a `<meta>` tag; `_headers` adds `frame-ancestors 'none'` as a real header, since browsers ignore it in a meta tag, so no other site can frame the installer.

## Cloudflare setup

The site is an assets-only Worker, `poolcontrol-site`, in the same Cloudflare account and zone as `api.poolcontrol.io` (the usage-reporting Worker in `api/`). The workflow builds `website/dist` with `build-site.sh` and runs `wrangler deploy` from `website/`. The site isn't built by Cloudflare, because `build-site.sh` needs the `gh` CLI and a GitHub token to download release assets.

`wrangler.toml` turns off the `*.workers.dev` address and preview URLs, so `poolcontrol.io` and `www.poolcontrol.io` are the only places the installer is served from.

One-time setup:

1. **API token.** Cloudflare dashboard → My Profile → API Tokens → *Create Token* → *Edit Cloudflare Workers* template. Narrow it to this account and the `poolcontrol.io` zone.
2. **GitHub environment.** Repo Settings → Environments → create `production`. Under *Deployment branches and tags*, allow `main` and tags matching `v*`. Add two environment secrets: `CLOUDFLARE_API_TOKEN` (the token) and `CLOUDFLARE_ACCOUNT_ID` (shown on the account's Workers overview page). Because the secrets belong to the environment, other branches can't read them.
3. **Zone settings.** Make sure **Rocket Loader** is off (Speed → Optimization). It rewrites `<script>` tags and can break the installer's module script. Leave Web Analytics automatic setup, Email Address Obfuscation and Bot Fight Mode off as well. They all inject scripts, which the page CSP blocks.
4. **First deploy.** Run the workflow (Actions → Deploy Website → Run workflow). The `custom_domain` routes create the `poolcontrol.io` and `www` DNS records and certificates. The deploy fails if a DNS record already exists for either name, so delete any old ones first (e.g. GitHub Pages `A`/`AAAA`/`CNAME` records). Don't touch the `api` record.

`www.poolcontrol.io` serves the same site as the apex. The home page's `<link rel="canonical">` points search engines at the apex.

## Deploy triggers

- **Tag builds** — `workflow_run` on Build & Release, filtered to tags. Not `release: published`: releases published with the built-in `GITHUB_TOKEN` (as `build.yml` does) don't trigger further workflow runs, so that trigger would silently never fire.
- **Pushes to `main` touching `website/`** — republishes the site against the current latest release.
- **Manual** — Actions tab → Deploy Website → Run workflow, also against the latest release.

A Worker deploy switches over atomically, so visitors never see a mix of old and new files. To roll back, use the Worker's *Deployments* tab in the Cloudflare dashboard, or `npx wrangler rollback` from `website/`.

Runs of Build & Release that aren't tag builds show up as *skipped* here. That's expected: there's no release for them to publish.

## Testing locally

```bash
./website/preview.sh                                     # latest 5 releases
./website/preview.sh --limit 2                           # fewer, quicker to download
./website/preview.sh --bin build/pool-controller-full.bin --version dev
```

This builds into `website/dist` and serves it on <http://localhost:8000> (installer at `/install/`), which counts as a secure context, so the flasher really works. The Python server ignores `_headers`. To test the real Worker behaviour (headers, the `/install` → `/install/` redirect), run `npx wrangler dev` from `website/` after a build; it serves the same `dist/` on <http://localhost:8787>. Requires `gh` unless you pass `--bin`. Opening `website/install/index.html` from disk does **not** work — `manifest.json` and `versions.json` are generated, and `file://` can't fetch them anyway.
