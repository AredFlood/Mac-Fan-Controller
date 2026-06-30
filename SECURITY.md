# Security Policy

Mac Fan Controller installs a privileged helper at:

```text
/Library/PrivilegedHelperTools/com.codex.macfanctl
```

The helper is intentionally small and only accepts a narrow command set:

- `status`
- `auto`
- `set-percent`
- `set-rpm`
- `max`

## Reporting a Vulnerability

If you find a security issue, please do not publish exploit details in a public
issue. Open a private advisory on GitHub if available, or contact the repository
owner directly.

## Supported Versions

Only the latest release is supported.

## Design Notes

- Write commands require the installed privileged helper.
- The app itself never runs as root.
- The helper does not execute arbitrary shell commands.
- The helper refuses to guess fan speeds when required SMC min/max values are
  missing.
- The app restores `AUTO` on exit by default.
