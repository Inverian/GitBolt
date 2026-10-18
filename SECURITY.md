# Security Policy

## Reporting a vulnerability

Please report security problems privately. Don't open a public issue,
pull request or discussion for them.

Report through GitHub: open the repository's
[security page](https://github.com/Inverian/GitBolt/security) and click
**Report a vulnerability**, or go straight to
[the reporting form](https://github.com/Inverian/GitBolt/security/advisories/new).
Only you and the security reviewers can see the report.

Helpful details:

- The GitBolt version (Help › About GitBolt) and your operating system;
- What an attacker can do, and what they need first (for example a
  crafted repository, a malicious remote, or local access);
- Steps to reproduce, or a proof of concept;
- Any fix or workaround you know of.

## Who reviews reports

Security reports are reviewed by
[@ThePieMonster](https://github.com/ThePieMonster) and
[@zbraasch](https://github.com/zbraasch).

## What to expect

- We aim to acknowledge a report within 7 days.
- We assess it, keep you informed in the report, and may ask you for
  more details.
- Once a fix is ready, we publish it in a new release together with a
  GitHub security advisory. We credit you there unless you prefer not
  to be named.
- Please give us a reasonable time to ship the fix before you share
  details publicly, and tell us if you plan a disclosure date.

## Scope

In scope:

- The GitBolt desktop application in this repository, including the
  installers and packages on the Releases page;
- GitBolt's handling of repositories, remotes, credentials and the
  commands it runs, and its usage statistics and crash reports (see
  [PRIVACY.md](PRIVACY.md) for what those send).

Problems in git, libgit2, Qt or your operating system themselves
belong with those projects. If GitBolt makes one exploitable, for
example by passing untrusted input to it, or through the patches it
applies to its bundled libgit2, please report that here.
