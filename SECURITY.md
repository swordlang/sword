# Security

## Reporting a problem

Report it privately, through
[a security advisory](https://github.com/swordlang/sword/security/advisories/new),
not in an issue or a pull request: an issue is public from the moment it is
opened. Include what you would for a bug — the smallest program that shows it,
the version, the system — and what an attacker gets from it.

The fix is released as a new patch version, and the advisory is published with
it, crediting you unless you would rather it did not.

## What counts

Anything that lets input from outside a program do what the program did not
ask for:

- memory corruption in the runtime or the standard library reachable from what
  a program reads — a request, a file, a certificate;
- `std/tls` accepting a certificate it should have refused, or a connection it
  should not have made;
- `std/http` reading one request as another, or a request as something it is
  not.

A program the compiler accepts when it should refuse it — a race the checker
missed, an escape it did not see — is a bug, and a serious one, but an issue is
the right place for it: the fix is in the compiler, and every user has to
rebuild to get it either way.

## Supported versions

Sword is below 1.0. Fixes go into the latest release, and into `develop`; older
releases are not patched.
