Voluntary compatibility reports — implementation and private trial, 12 October 2026

The library rating remains local. The game settings action in every library style
opens a menu with Submit result for a classified game. An unrated game has only
Game settings. Submit result opens a preview; only Send creates a request.
Diagnostics settings, diagnostics tokens, game launch and library holds do not
participate in this feature.

The preview contains the exact immutable report snapshot, including its schema
version and random submission UUID. Missing graphics are displayed as unavailable
and serialized as null. A successful reply provides a copyable server report
number. Failed sends keep the same snapshot for a manual retry, including after
reopening the screen. There are no automatic retries. The single pending snapshot
is local app-private data and is discarded after confirmed success.

Build configuration: `-Plsx4CompatibilityReportUrl=https://…`.
Default: `https://lsx45.app/api/v1/compatibility-reports`. The client rejects HTTP,
credentials in the URL and redirects. It uses 8-second connect and 10-second read
timeouts and caps both request and response at 4096 bytes. No new permissions,
dependencies, diagnostics tokens or persistent device identifiers are used.
The existing independent device graphics probe supplies only the whitelisted
renderer/vendor and Vulkan device/API/driver fields to the payload. Guest runtimes
and their settings are unchanged.

Server

The Python standard-library service listens only on `127.0.0.1:8765`, under the
`lsx4-reports` system account. Caddy handles the exact API path and retains all
existing static site directives. There is no per-IP or global request-rate limit
and no application IP tracker. Sixteen concurrent handlers and 5-second connection
timeouts bound active resources without imposing a delay between submissions.

Strict schema v1 validation rejects extra or duplicate fields, invalid UTF-8,
wrong types, controls, oversized strings, invalid UUIDs and mismatched PS4/PS5
title IDs. SQLite uses parameterized statements, WAL, a unique submission UUID,
game/time indexes, a versioned migration, 3-second lock timeout, secure deletion,
100,000-row admission limit and 128 MiB page limit. Replaying identical content
returns the original receipt; reusing a UUID for different content returns 400.
The API exposes no report listing, retrieval or deletion endpoint.

Initial installation uses the existing local SSH alias `donweb-lsx45`. Its private
key is never copied into this repository, APK or server. Stage the files in
`tools/compatibility_reports`, then execute `install.sh` as root. That script saves
a Caddy backup, checks the expected site block, inserts one exact-path handler,
validates the configuration and restores it if validation/reload fails. Root is
used for installation; the API and maintenance processes run unprivileged.

Admin commands (execute on the VPS through SSH):

```
runuser -u lsx4-reports -- python3 -B /opt/lsx4-reports/service.py count
runuser -u lsx4-reports -- python3 -B /opt/lsx4-reports/service.py stats
runuser -u lsx4-reports -- python3 -B /opt/lsx4-reports/service.py show REPORT_UUID
runuser -u lsx4-reports -- python3 -B /opt/lsx4-reports/service.py delete REPORT_UUID
runuser -u lsx4-reports -- python3 -B /opt/lsx4-reports/service.py backup /var/lib/lsx4-reports/backups/NEW.sqlite3
```

The database and backups are outside the web root; their directory is 0700 and
files are 0600. Reports have no automatic expiry, including on service restart.
The daily maintenance timer takes an integrity-checked SQLite online backup.
Backup files rotate after 7 days; this does not expire history in the live database.
After a deletion request, any residual backup copy expires within 7 days.

The private `stats` CLI groups report counts by game/platform and reported
manufacturer/model/SoC, split into playable and not-playable reports. Identical
devices with different random submission UUIDs add separate reports. A manual
retry with the same UUID and content adds none. These are report counts, not
unique-user or unique-installation counts. No persistent identifier is collected.

Report preview, consent, outcomes, field names and actions are localized in all
six supported languages: English, Russian, Spanish, French, German and simplified
Chinese. Device values and game metadata remain as supplied by the device/game.

Restore is deliberately into a new path. Stop the service before activating a
restored database, preserve the current database and any WAL/SHM together, then
activate the tested restored file with owner `lsx4-reports`, mode 0600, and start
the service. Never overwrite a live SQLite database. Test restoration separately:

```
runuser -u lsx4-reports -- python3 -B /opt/lsx4-reports/service.py restore BACKUP.sqlite3 /var/lib/lsx4-reports/restored-NEW.sqlite3
```

Validation and release boundary

Run `python -B -m unittest -v test_service` in `tools/compatibility_reports`.
Android instrumentation `CompatibilityReportSmokeRunner` refuses to run outside
`app.lsx4.android.reports`; its companion test APK is not packaged in production.
It covers explicit preview/cancel, both results against the real HTTPS endpoint,
manual retry after a 400 with unchanged UUID/bytes, missing-graphics preview and
submission, HTTP rejection, unavailable server and simulated transport timeout.
The `uiOnly=true` pass exercises an actual five-second hold, rated/unrated menu
visibility and the shared menu handler with all seven library style values.

The tested APK uses the repository's explicit UI-only packaging tool and preserves
every baseline native core, graphics probe and asset byte. This is a private
feature test, not gameplay/FPS acceptance of the unmerged native development tree.
Normal Gradle runtime freshness gates remain unchanged. The original main app,
settings and game files on Poco are preserved. Source changes are LOCAL ONLY;
no GitHub branches, commits, comments or APK releases are published by this task.

Before a public APK release, publish the prepared policy text through the website's
existing review process and update Google Play Data safety for the entire app.
The draft below describes this feature's change only; it does not replace review
of existing analytics, purchases, SDKs and optional diagnostics.

Data safety draft for publisher review:

- Optional collection of the user-selected compatibility result and title:
  review the App activity / Other user-generated content category.
- Device hardware/software and app version used to investigate compatibility:
  review App info and performance / Diagnostics.
- Purpose: app functionality and investigating compatibility. No advertising or
  automatic publication; reports are private first-party data.
- Report data is retained without automatic expiry, so do not label it ephemeral.
  The reporting application does not retain or log request IP addresses.
- No installation/device ID is introduced by this feature. A random report UUID
  identifies one submission only. Existing analytics/diagnostics identifiers
  remain subject to their existing whole-app disclosures.
- HTTPS in transit; users can decline collection and request deletion using the
  server report number via the published contact address.

These category mappings are a proposed interpretation to review in Play Console,
not a submitted questionnaire. Official references consulted on 12 October 2026:
[Data safety guidance](https://support.google.com/googleplay/android-developer/answer/10787469?hl=en)
and [User Data policy](https://support.google.com/googleplay/android-developer/answer/10144311?hl=en).
