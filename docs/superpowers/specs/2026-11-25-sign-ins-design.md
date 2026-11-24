# Sign-ins: design

Date: 2026-11-25. Status: waiting for approval.

## Intent

Skein cannot sign in to anything. A 401 or 403 ends with "not supported in this version",
FTP is anonymous only, gated Hugging Face repositories and private GitHub releases are
refused, and archive.org items that need an account cannot be downloaded. This is the
first step toward "download anything, anywhere": letting a download carry the user's
sign-in to the server that asks for it. The owner has no specific failing case; the scope
below is the author's recommendation, taken from the earlier code review.

## Goals

1. A user name and password for a web server (HTTP Basic) or an FTP server.
2. An access token for a site whose API takes one: Hugging Face, GitHub, and archive.org
   (its S3-style access key and secret).
3. Sign-ins are saved in the operating system's own credential store, never in Skein's
   files.
4. When a link in the file picker is refused for lack of a sign-in, the picker asks for
   one, then loads again.
5. A sign-in is only ever sent to the server it was saved for.

## Non-goals

- Reading cookies from a browser for file downloads (video downloads keep their existing
  yt-dlp cookie option). Sites that only work after a web-page login, other than the
  three above, stay unsupported in this step.
- OAuth or any sign-in flow in a browser window. Two-factor prompts.
- Custom request headers, referers or user agents per download. Proxies. SFTP and FTPS.
- Sign-ins for video downloads.
- Signing in to file lockers (MediaFire, Google Drive, OneDrive, WeTransfer).

## Anti-goals

- A secret must never be written to `settings.ini`, `history.json`, a staging folder's
  `state.json`, the log, or a command line.
- A secret must never be sent to a host other than the one it was saved for, including
  after a redirect.
- A secret must never be sent over an unencrypted connection without the user having
  been told and having agreed for that server.
- No sign-in window may pop up from a background download. Only an action the user just
  took in a window may open one.

## Constraints

- C++20, the two transports as they are (WinHTTP on Windows, libcurl on Linux), wxWidgets
  3.3 for the windows. Everything works on Windows and Linux.
- The engine library (`archive_core`) does not use wxWidgets. Only the window code may
  talk to the credential store.
- New text goes through the translation pipeline for all 13 languages.
- House rules for wxWidgets apps (native controls, layout, dark mode).

## Approaches considered

**Where secrets are kept**

- **A. The operating system's credential store through wxWidgets' `wxSecretStore`
  (chosen).** Windows Credential Manager on Windows; the Secret Service (GNOME Keyring,
  KWallet) on Linux. Encrypted by the system, tied to the user's login, nothing for Skein
  to protect. Costs: the feature is switched off in the build today (`SECRETSTORE` is in
  the list of disabled wxWidgets features in `CMakeLists.txt`), Linux needs libsecret at
  build time, the Flatpak needs permission to talk to `org.freedesktop.secrets`, and some
  Linux sessions (SteamOS game mode) have no such service.
- **B. An encrypted file in the profile folder.** Works everywhere, but the key has to
  live somewhere Skein can read without asking, so it protects little. Rejected.
- **C. Plain text in `settings.ini`.** Rejected: it breaks the first anti-goal.

When no credential store is available, sign-ins still work but only until the program
closes, and the window says so. Nothing is written to disk in that case.

**How a download finds its sign-in**

- **A. By host, looked up when the request is made (chosen).** A saved sign-in belongs to
  one host (and for a password, optionally one port). Jobs store nothing about it; the
  engine asks for the sign-in of the host it is about to contact. Changing or removing a
  sign-in takes effect for queued jobs at once.
- **B. Stored with each job.** Rejected: it would put secrets, or references that go
  stale, into the history file.

## Design

### What a sign-in is

Three kinds, each saved under a host name in lower case:

| Kind | Fields | Sent as |
|---|---|---|
| Password | user name, password | HTTP: `Authorization: Basic …`. FTP: `USER` / `PASS`. |
| Token | token | `Authorization: Bearer <token>` |
| archive.org keys | access key, secret | `Authorization: LOW <access>:<secret>` |

There is one sign-in per host (per host and port when a port was given): a host has a
password or a token, never both, and never two user names. That host (and port) is the
key in the credential store and in the list kept in `settings.ini`.

Which hosts a sign-in covers (always exact host names, never their subdomains, except
where a rule below says so):

- A password covers exactly its host (and port, when one was given).
- The Hugging Face token is saved under `huggingface.co` and is also sent to `hf.co`.
  Hugging Face's storage hosts, including subdomains of `hf.co`, get nothing.
- The GitHub token is saved under `github.com` and is also sent to `api.github.com`.
  GitHub's storage hosts get nothing.
- The archive.org keys are saved under `archive.org` and are also sent to its download
  servers: hosts that end in `.archive.org`. Not to `web.archive.org` (the Wayback
  Machine), which takes no sign-in.

These three sites are offered by name in the window, each with a link to the page where
the user creates the token or keys. Any other host can be given a password or a token.
A token for another host is sent as `Authorization: Bearer` on every request to that
exact host; products that want a different header form are not supported in this step.

A sign-in applies to every request the program makes to a host it covers: file listings
and API calls as well as file downloads. It is attached in one place, where a request is
built, not at each call site.

Private GitHub releases need one more change: GitHub does not accept a token on the
`https://github.com/.../releases/download/...` links the release data lists. When a
GitHub token is saved, release assets are downloaded from their API address
(`https://api.github.com/repos/<owner>/<repo>/releases/assets/<id>` with
`Accept: application/octet-stream`), which redirects to a signed storage address; that
API address is then the file's identity in the queue. Without a token nothing changes.

### Sending it safely

- The engine holds the sign-ins in memory only, in a table the window fills
  (`archive::setSignIns(...)`); it never reads the credential store.
- For each request the engine looks up the request's own host. A sign-in is attached only
  when the host matches as above.
- Redirects: a request that carries a sign-in is not allowed to follow redirects on its
  own. The engine follows each redirect itself and looks the next host up again, so the
  secret goes only to hosts it covers. Hugging Face and GitHub both redirect downloads to
  storage hosts with signed addresses that need no sign-in; those hops get none. (On
  Windows, headers added to a WinHTTP request stay on it across redirects, so this cannot
  be left to the transport.)
  This is the largest engine change in the design, not a switch. Today both transports
  treat any status other than 200 and 206 as an error, nothing reads `Location`, and the
  probe, mirror and multi-connection code rely on the transport having followed
  redirects and on `Request::url()` giving the final address. The builder adds a
  no-follow request that returns a 3xx answer with its `Location`, and a redirect loop
  around it (same limit of 10 hops, never from `https` down to `http`) that keeps those
  callers' view unchanged. Only requests to a host that has a sign-in take this path;
  all other requests keep today's behavior exactly.
- Encryption: over `https` a sign-in is always sent. Over `http` or `ftp` it is sent only
  when that saved sign-in has "Allow on an unencrypted connection" ticked; the window
  warns in plain words when the user ticks it. Tokens and archive.org keys are never sent
  unencrypted.
- A sign-in is not sent with the first request to a server unless the server is one of
  the three named sites or the sign-in is a token. For a password over HTTP the engine
  sends it only after the server answers 401 with a Basic challenge, as browsers do.
  FTP has no challenge: a saved FTP password is used for the login on the first
  connection. FTP is never encrypted, so an FTP sign-in can only be saved with the
  unencrypted-connection tick.
- Logs never contain a secret: header values for `Authorization` are not logged, and FTP
  addresses are logged without user information (they already are).
- Multi-connection downloads and resume work unchanged: each connection's request gets
  the sign-in the same way.

### When a sign-in is missing or wrong

The engine reports it as its own kind of error (`SignInNeeded`: the host, and whether the
server offered Basic), distinct from other 401 and 403 answers.

- In the file picker, loading a link: the picker opens a small dialog, "This server needs
  a sign-in", with the host shown and fields for the kind that fits (a token for the
  three named sites, a user name and password otherwise), a "Remember" tick (on by
  default when a credential store is available), and the unencrypted-connection tick when
  the link is `http` or `ftp`. OK saves it and loads again. A second refusal says the
  sign-in was not accepted and offers to try again.
- In the queue, a download: the job fails at once (it is not retried automatically) with
  "Sign-in needed for <host>. Add it in Settings, then resume." No dialog appears. This
  is the queue's existing Failed state, which Resume clears; no new job state and no
  change to the history file.
- FTP: a refused login is "sign-in needed" when no sign-in was tried and "not accepted"
  when one was (the code tracks which, since neither FTP library says).
- A sign-in that the server rejects (401 after sending it) is reported as "The sign-in
  for <host> was not accepted", in both places.
- A 403 with a valid sign-in (signed in, but not allowed, such as a gated Hugging Face
  repository whose terms were not accepted) keeps a specific message naming what to do on
  the site.

### Managing sign-ins

The Settings window gets a "Sign-ins" section: a list of saved hosts with their kind, and
Add, Edit and Remove buttons. Add offers "Hugging Face", "GitHub", "archive.org" and
"Another server". Secrets are shown as dots and can never be read back out of the window;
Edit replaces them. Remove deletes the entry from the credential store.

The list of hosts and kinds (no secrets) is kept in `settings.ini` so the window can show
it without unlocking anything; the secrets themselves are read from the credential store
at startup and when changed, and handed to the engine. They are read at startup only when
that list is not empty, because on Linux reading may make the system ask to unlock the
keyring; that prompt belongs to the system and appears as the program starts, never from
a background download.

A listed host whose secret cannot be read (the store was cleared, is locked, or is not
reachable) is shown as "not available" in the list and behaves as if it had no sign-in.

Sign-ins that are not remembered live only in memory, in the window code's own table and
the engine's. The "Remember" tick alone decides whether one is written to the credential
store; when no store is usable the tick is disabled and unticked.

### FTP

`ftp://host/path` uses the saved password for that host when there is one, and anonymous
otherwise. A user name in the link itself (`ftp://user@host/…`) uses the saved sign-in
when its user name is the same; otherwise the picker asks for that user's password, and
saving it replaces the host's sign-in. A password written in a link
(`ftp://user:pass@host/…`) is accepted for that load, is offered to be saved, and is
removed from the link before the link is stored in a job, the history or the log.
Today the FTP address parser rejects any user name or password; it, and the two other
places that parse the same address (the transports' `Request::Url` and the folder
loader), are extended together.

### Build and packaging

- Remove `SECRETSTORE` from the disabled wxWidgets features. Linux builds need the
  libsecret development package; `BUILDING.md` and the Linux build scripts say so.
- The Flatpak manifest gains `--talk-name=org.freedesktop.secrets`.
- The AppImage uses the system's Secret Service when present.
- When `wxSecretStore` reports it is not usable, the window shows, in the Sign-ins
  section and in the sign-in dialog, that sign-ins will be kept only until Skein closes.

## Left to the builder

- Names and shapes of functions and types; the layout of the dialogs within the house
  rules.
- How the three named sites' tokens are validated before saving, if at all.
- Unit and loopback-server tests: the host matching table, redirect handling (a sign-in
  must not reach a second host), Basic after a challenge, the unencrypted rule, the
  `SignInNeeded` error, and that no secret appears in the log, the history or a staging
  folder after a download.

## Calls made by the author (not decided by the owner)

- The scope: passwords and tokens now, browser cookies later.
- archive.org through its access key and secret rather than browser cookies.
- No automatic retry for a job that needs a sign-in.
- A password is sent only after a Basic challenge; tokens are sent at once.
- Sign-ins are per host, not per folder or per link.
- Session-only sign-ins when there is no credential store, rather than a file.
- The "Sign-ins" section lives in the existing Settings window.

## Not verified while writing this

- That archive.org's `Authorization: LOW` keys unlock downloads of account-only items on
  its download servers. It is documented for its S3-style API; the builder must confirm
  it against a real restricted item before this part is built, and report if it does not
  hold.
- How each transport treats an `Authorization` header across redirects in the versions
  in use. The design does not depend on it, since redirects are followed by the engine.
- That GitHub serves a private release asset from its API address with a token and
  `Accept: application/octet-stream`. The builder confirms it against a real private
  release before building that part.
- That the AppImage can reach the host's Secret Service. It depends on the host; where it
  cannot, sign-ins last until the program closes, as on any system without one.
