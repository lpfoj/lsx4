Replacement text for the existing privacy policy, prepared locally for the website
review process. Not deployed by this feature test; no public APK has been released.

Section 1, last sentence:

Game compatibility marks made by holding a game in the library stay on your
device. In versions with the separate Submit result action, a compatibility report
is sent only after you review its values and press Send.

Section 9:

Holding a game in your library can mark it Playable or Not playable (stored as
Supported or Unsupported). An unrated or cleared game has No rating and cannot
be submitted as a test result. These marks are stored locally. Holding a game,
opening a game, starting the app or enabling diagnostics does not submit a report.

Submit result opens a preview showing every value to be sent. Send submits that
report over HTTPS; Cancel sends nothing. Each submission is voluntary and separate
from diagnostics consent. An error preserves your local rating and offers manual
retry; the same random submission UUID prevents duplicates after a network failure.

The report contains only:

1. Game title, title ID, PS4 or PS5 platform and the result you chose.
2. LSX4 version, device manufacturer/model, Android version/API level,
   processor/SoC and CPU architecture.
3. GPU renderer/vendor, Vulkan device/API/driver versions and total RAM, where
   available. Unavailable device or graphics fields are sent as null.
4. Report format version and a random UUID for this submission only. This is not
   an installation or device identifier.

The report does not include game files, paths, logs, screenshots, free text,
account details, location or permanent device/installation identifiers. The server
adds a random report number and UTC receipt time. The HTTPS server sees the
request IP as part of the connection; the reporting application does not retain
it or write it to reports or application request logs. Request-rate limits are
currently disabled.

Reports are in a private database accessible to the server administrator. They
are used to investigate device-specific game compatibility, not for advertising.
They are not automatically published or added to a public compatibility list.
Successful submission shows a copyable report number for deletion requests.

Section 10, replace future compatibility-report sentence:

A compatibility report requires a connection only when you press Send. Games and
local compatibility ratings continue to work without a connection.

Section 11, add:

Compatibility reports are retained without automatic expiry. To request deletion,
write to contact@lsx4.app and provide the report number shown after submission.
We remove the report from the live database; any residual backup copy expires
within 7 days. Not sending another report stops further collection by this feature.

Section 13, consent paragraph addition:

We rely on your consent for each voluntary compatibility submission, given by
pressing Send after reviewing the report. Declining a submission does not affect
your local ratings or ability to run games.
