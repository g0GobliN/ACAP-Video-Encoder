# Contributing

This is a small, single-purpose companion component, not a project with a maintainer team. The useful
contributions are reproducible bug reports and focused changes that match the existing shape of the code.

## Reporting a bug

Include enough to reproduce the failure:

- encoder version and API level (`encoder` and `api` from the GET response);
- target architecture and SDK version used to build;
- the operation (`start` or `cancel`) and the exact form fields, with paths and credentials replaced by
  placeholders;
- any side files produced (`.log`, `.done`, `.progress`);
- what you expected and what happened.

If the failure depends on camera-specific paths or permissions, say so. Source inspection can explain a
lot, but SD-card access, permissions, and real encoding behavior need the hardware.

## Proposing a change

Open an issue or a pull request that says what should change and why. Because the codebase is small, a
tight diff is usually better than a large one. If the change touches the HTTP interface, call out whether
it is compatible with the existing API version or is a breaking change.

This repository is a personal portfolio component. Issues and pull requests are welcome as a way to
document reproducibility and proposed changes; they are not a support channel with a service level or a
commitment to adopt any particular change.
