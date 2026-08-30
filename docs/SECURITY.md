# Security policy

This repo is [VorticonCmdr](https://github.com/VorticonCmdr)'s fork of
[dosbox-automation/dosbox-automation](https://github.com/dosbox-automation/dosbox-automation)
(upstream). This policy covers this fork's own code (including the debugger
REST/Lua area and both web UIs, which upstream doesn't have). For upstream
dosbox-automation itself, report to upstream's own repo, not here.

## Supported versions

The [`main` branch](https://github.com/VorticonCmdr/dosbox-automation/tree/main)
of this fork is supported with security updates. There is no tagged release
yet.

## Reporting a vulnerability

Raise a
[bug report](https://github.com/VorticonCmdr/dosbox-automation/issues/new?template=bug_report.yml&title=Security%20issue:%20)
on this repo. We believe in open technical discourse about security; findings,
analysis, and patches are welcome in public.

## Security model in short

Most of the mechanism below is inherited from upstream unchanged; upstream's
own [security documentation](https://dosbox-automation.org/0.84-da3/automation/security/)
describes it in more detail (that page does not cover this fork's own
additions: the auth opt-out below, and a stricter `webserver_allow_remote`
check than upstream's - it now rejects any non-loopback bind address, not
just the two wildcard forms).

The REST API gives full control over the emulated machine, which makes the
webserver an attack surface by design. The short version:

- Every API request needs a bearer token by default (64-char random hex,
  fresh per start, never fully logged, constant-time comparison). No default
  credential. Authentication can be turned off entirely
  (`webserver_require_auth = false`) for local development - this is this
  fork's own addition, not something upstream has. The server refuses to
  start with it off unless bound to a loopback address
  (`127.0.0.1`/`::1`/`localhost`) with `webserver_allow_remote = false`, and
  the token file is not served over HTTP while auth is off.
- The webserver binds to localhost only by default; any `webserver_bind_address`
  other than `127.0.0.1`, `::1`, or `localhost` needs `webserver_allow_remote=true`
  (stricter than upstream, which only special-cases `0.0.0.0`/`::`). Host
  header validation rejects DNS rebinding. No CORS headers are set, OPTIONS
  preflight is refused. Request bodies are capped.
- Every MOUNT, BOOT, and drive-swap path is validated before a drive is
  constructed: paths must resolve, symlink components are rejected, system
  directories are blocked, and disk images must pass structural validation.
  With the webserver enabled, directory mounts are whitelist-restricted,
  and a one-way mount lock can freeze the mount configuration until
  restart. Whitelists are read from the primary config only, so a bundled
  game config cannot widen them.
- Lua scripts run sandboxed: no filesystem or process access, bytecode
  rejected, dangerous globals removed, instruction and pattern-complexity
  limits against runaway scripts.

If a security fix changes any of this, this file and the manual page above
are updated together with the fix.
