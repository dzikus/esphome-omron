# Changelog

## v1.2.0 (2026-09-23)

- HEM-7361T uses the HEM-7342T profile, which shares its memory map.
- A ring the cuff flags as full is read whole. A ring with no records and an
  unwritten ring slot read as empty. Unwritten settings fail the read.
- A ring whose cursor stood still while its unread counter moved is read.
- A warning when a later record number sits outside the slot the cursor named.
- A profile mismatch names the field and the key to use.
- Issue form for cuff reports.

## v1.1.2 (2026-09-19)

- A loop index flagged by CodeQL is widened.
- CI: per-job pip caches, ESP-IDF toolchains cached per architecture.

## v1.1.1 (2026-09-13)

- No change to the component.
- CI: tool pins read from the requirements file, per-job permissions, workflow
  audit and dependency review on pull requests, CodeQL and Scorecard.
- Dependency updates.

## v1.1.0 (2026-08-31)

- No change to the component.
- CI: shellcheck and actionlint hooks, conversion warnings in the host tests,
  tool pins read from the requirements file, the release runs the test
  workflow, `example.yaml` validated at the ESPHome floor, the event path
  compiled.

## v1.0.0 (2026-08-31)

- First release.
