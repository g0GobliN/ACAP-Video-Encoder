# Security

The encoder is a headless ACAP service with one FastCGI endpoint and no operator page. Its posture
is limited by design: it trusts the companion workflow that calls it, restricts every path to the
SD card, and validates its inputs before starting an encode. It is not a general-purpose web
service and it has not been security-audited.

## Trust boundary

- The caller is the companion workflow running on the same camera. The encoder does not
  authenticate end users, issue sessions, or manage access beyond what ACAP provides for the
  configured endpoint.
- The endpoint is exposed with `access: operator` in the package manifest, so access is governed by
  the camera's existing operator credentials and ACAP's access model, not by anything this package
  adds.
- Manifest and output paths are resolved with `realpath` and rejected if they resolve outside the
  SD-card root; request parameters are bounded before parsing.

## Known limitations

- Cancellation is best-effort. The encoder signals the FFmpeg PID it finds in the `.pid` file and
  does not verify the result or clean up side files on the caller's behalf.
- The lock file is a simple exclusive-file lock. It prevents two starts from running at once, but
  it is not crash-proof or power-fail-safe.
- The main process ignores `SIGCHLD`, so terminated children are reaped by the kernel. The parent
  does not track child exit status itself.
- The FFmpeg child is not sandboxed beyond filesystem path checks, lowered priority, and OOM-score
  adjustment.

## Reporting a vulnerability

Please report security issues privately rather than through a public issue — GitHub's private
vulnerability reporting for this repository is the intended channel.

Do not include credentials, camera passwords, private URLs, or hostnames in a report. If a
credential appears in this repository, treat rotation as the real fix: deleting a file does not
undo history.

## Scope

This file documents intended behavior and known limitations. It is not a security audit, a
production-readiness assessment, or a guarantee that the endpoint, the packaging, or the
camera-side deployment are free of issues.
