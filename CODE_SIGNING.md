# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

> **Status:** application pending. Releases published before approval (including v0.1.0) are **unsigned**. Every release is still built by GitHub Actions and carries a build provenance attestation (see [Verifying a download](#verifying-a-download)).

## What gets signed

- Only `McScope.exe`, built from a tagged commit of this repository by [`.github/workflows/build.yml`](.github/workflows/build.yml) on **GitHub-hosted runners**.
- Signing happens only for version tags (`v*`) and needs a maintainer's approval in SignPath.
- Nothing built outside that workflow is ever signed.
- Third-party code compiled into the exe is built from source in the same workflow: [Dear ImGui](https://github.com/ocornut/imgui) (MIT) and [nlohmann/json](https://github.com/nlohmann/json) (MIT). No proprietary components are included.

## Team roles

| Role | Members |
|---|---|
| Committers and reviewers | [tarekFerdous](https://github.com/tarekFerdous) |
| Approvers | [tarekFerdous](https://github.com/tarekFerdous) |

- **Committers** may change the source code directly.
- **Reviewers** review every contribution from outside the team before it is merged.
- **Approvers** approve each signing request.
- All team members use multi-factor authentication for GitHub and SignPath.

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

McScope has no network code at all. Its settings and log stay on your computer, in `%APPDATA%\McScope\`.

## Verifying a download

Every release zip comes with a SHA-256 checksum file and a GitHub build provenance attestation. To confirm a download was built from this repository by GitHub Actions:

```powershell
gh attestation verify McScope-<version>-win-x64.zip -R tarekFerdous/McScope
```
