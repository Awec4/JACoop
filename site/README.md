# Co-op status — hosting integration

## If your site is AdvJKSite (Workers + D1)

The co-op page and API are integrated directly into
[AdvJKSite](https://github.com/koevetmc/AdvJKSite): `GET/POST
/api/coop/status` stores the host status in the existing D1 database
(`coop_status` table), and the React page lives at `/coop` in the site nav.
The standalone files in this directory are **not needed** for that site.

Setup for AdvJKSite:

1. Create the table once:
   `npx wrangler d1 execute advjk-accounts --file=./init_new_tables.sql`
   (in the AdvJKSite repo — it's the same file you ran for `server_status`).
2. The status POST is authenticated with the same **`SYNC_SECRET`** the mod
   already uses for `/api/sync` and RCON — sent as
   `Authorization: Bearer <secret>`. Nothing new to configure in Cloudflare.
3. In the host's JACoop console / `jacoop.cfg`:

   ```
   seta sv_statusURL      "https://<yoursite>/api/coop/status"
   seta sv_statusToken    "<your SYNC_SECRET>"
   seta sv_statusJoin     "<public address, e.g. 12.34.56.78:29070>"
   seta sv_statusInterval "20"          // optional, seconds
   ```

After that, hosting a co-op game updates the page every ~20 s; it flips to
offline after 2 minutes without an update.

## Standalone variant (any Cloudflare Pages site)

`coop.html` + `functions/api/coop-status.js` are a self-contained version
for a static Pages site. Drop `coop.html` at the site root,
`functions/api/coop-status.js` in `functions/api/`, then:

1. **KV namespace**: bind a KV namespace as `COOP_STATUS` in the Pages
   project settings.
2. **Secret**: add env var `COOP_STATUS_TOKEN` = a long random string.
3. Same `sv_status*` cvars, with `sv_statusURL` pointing at
   `/api/coop-status` (the standalone function checks `token` in the JSON
   body; the mod sends it in the body *and* as a Bearer header).

## How it flows

```
host's game ──POST /api/coop/status──> Worker/Pages Function ──> D1 or KV
browser     <──GET  /api/coop/status──┘
```

The `advjk://` link handler is registered by the launcher the first time
someone runs JACoop.exe after updating — no installer needed.
