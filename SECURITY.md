# Security Policy

## Supported versions

Latest release only.

## Reporting a vulnerability

Use GitHub private vulnerability reporting:
https://github.com/dzikus/esphome-omron/security/advisories/new

Do not open a public issue.

Include the component version or commit, the ESPHome version, the cuff model
and profile, the configuration and the node log. Remove the MAC address and the
`bindkey` from anything you attach.

There is no response-time commitment.

## Scope

The code in this repository. It handles blood pressure readings and, on cuffs
that store a key, the `bindkey` that authenticates the node.

## Out of scope

- ESPHome, ESP-IDF and Home Assistant. Report to those projects.
- Transport to Home Assistant. Readings go over the ESPHome native API, which
  is unencrypted unless `api: encryption: key:` is set on the node.
- Cuffs paired by omblepy or the Home Assistant `omron` integration. Both write
  the same public key; see
  [Cuffs that store a key instead](README.md#cuffs-that-store-a-key-instead).
