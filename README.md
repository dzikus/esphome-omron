# ESPHome component for Omron Bluetooth blood pressure monitors

Reads measurements straight off an Omron cuff into Home Assistant, over BLE, with
no Omron cloud account and no phone involved. The cuff keeps its readings in an
internal memory; this component opens a session, copies what is there, and
publishes it.

**248 model names across 38 profiles.** One profile has been verified against
real hardware. Read [Confidence levels](#confidence-levels) before trusting a
reading from any of the others.

## Quick start

```yaml
external_components:
  - source: github://dzikus/esphome-omron

esp32_ble_tracker:

omron:
  - id: omron_cuff
    mac_address: !secret omron_mac
    profile: auto          # see "Which profile is mine" below
    # Everything that varies per person lives here, so a platform block below
    # only has to say which user it is for.
    users:
      - user: 1
        name_prefix: Alex

# One block per person, plus one for the cuff itself. Each block creates the
# whole set of entities it is entitled to; there is nothing to list by hand.
sensor:
  - platform: omron
    omron_id: omron_cuff          # the cuff's own: clock drift, poll duration, RSSI
  - platform: omron
    omron_id: omron_cuff
    user: 1                       # this person's readings

text_sensor:
  - platform: omron
    omron_id: omron_cuff
  - platform: omron
    omron_id: omron_cuff
    user: 1
```

A fuller node is in [`example.yaml`](example.yaml).

Flash it, then press the cuff's Bluetooth button briefly. The node connects,
reads, publishes and disconnects. A session takes five to seven seconds.

**A block without `user:` carries the cuff's own entities** (clock, status, RSSI);
a block **with** `user: N` carries one person's measurements. Configure only the
first kind and every record is read correctly and has nowhere to go. The
component says so at startup rather than leaving you to notice.

## Which profile is mine

Omron sells one cuff under several model codes and several cuffs under one trade
name, and the code on the box is not always the one that matters: `HEM-7155T`
alone covers four different memory layouts. Picking the wrong profile does not
fail. It reads a different region and publishes numbers that look like blood
pressure.

So do not guess. Set `profile: auto`, press the button once, and read the log:

```
[W] Detected HEM-7155T-MW3 from "X4 Smart" (trade name), hardware verified.
    Pin it with `profile: hem_7155t_mw3` in the yaml.
```

Then put that line in the yaml and leave it there. `auto` is for finding the
answer, not for running on: under it ESPHome cannot validate the user count or
the bind key requirement at config time, because neither is known until the cuff
has been asked.

If detection cannot decide, it says so and refuses rather than guessing. Two
trade names are genuinely ambiguous, and a cuff whose reported string means
nothing here is the expected case for models nobody has tried.

With a profile configured, the same check runs as a verification: it confirms the
profile, accepts a different entry over the same memory map, or reports a
mismatch. Loudly, and once per session, because a wrong map is the one failure
nothing downstream can catch.

## First run and pairing

Most Omron cuffs need to know this node before they will keep talking to it, and
they drop the bond of a host that pairs without registering. The component
handles that, but the button sequence is yours to press:

1. **Hold** the cuff's Bluetooth button until `-P-` blinks. The cuff is now
   offering pairing.
2. The node bonds, registers itself, and reads. One session.
3. From then on a **brief** press is enough.

Two options govern this, and neither belongs in a working config: both take
their answer from the profile, which is why the example writes neither.

- `require_bond` bonds before touching any characteristic. Cuffs on the newer
  stack refuse everything else, and their profiles say so.
- `keep_bond` lets the bond survive the session. No profile here asks for it to
  be dropped: a cuff that looks like it invalidates its side after every session
  is really discarding the bond of a host that never registered itself. Setting
  it to false means pairing mode every single time.

If a bond ever goes stale, the **Forget bond** button drops this node's own
record and only that one; the next pairing starts clean. Watch for
`Selective bond cleanup complete` before holding the button again.

### Cuffs that store a key instead

Older models do not rely on the Bluetooth bond at all. They keep a 16-byte key
in their own memory, and a host proves itself by presenting it. Roughly half the
profiles here work this way; the component says so at startup and refuses to
guess, so the sign of one is `requires an already-provisioned bindkey` in the
log.

**The key is yours to invent.** It is not issued by Omron and there is nothing
to look up: pick 32 hex characters, put them in `bindkey`, and the node writes
them into the cuff the first time it pairs. What matters is that the value never
changes afterwards, because the cuff stores it and every later session
authenticates with it. It is not tied to this node either - the same key from a
different node, with a different Bluetooth address, still works.

ESPHome treats the option as sensitive and masks it, so keep it in `!secret`
like the MAC.

```yaml
omron:
  profile: hem_7155t
  bindkey: !secret omron_bindkey   # 32 hex characters, yours to choose
```

Programming only happens while the cuff is showing `-P-`, so the first run needs
the long press exactly as above. **If a key is refused, power the cuff off before
trying again**: it latches the error and will reject even a correct key until it
is restarted and put back into pairing mode.

**Coming from another tool, the cuff already holds a key and you have to match
it.** Both omblepy and the Home Assistant `omron` integration write the same
fixed value, so a cuff paired with either wants:

```yaml
  bindkey: deadbeaf12341234deadbeaf12341234
```

Anything else is refused, since the cuff compares against what it stored. Pairing
again with a key of your own replaces it - and then the other tool stops working
until it is told the new one.

None of this path has been run against hardware here - no cuff of that kind was
available - though it is checked frame by frame against implementations that
have. Treat a first pairing as something to watch the log through.

## What it publishes

Per user: systolic, diastolic, pulse, pulse pressure, estimated MAP, shock index,
rate pressure product, ACC/AHA category, measurement timestamp, sequence number,
and the detection flags the model actually supports: cuff fit, body movement,
irregular pulse, and where the model has them, consecutive-measurement, artifact
and IHB counts. Also what the cuff stores about that person rather than measures:
their birth date, and on models that keep one, the settings version counter that
says whether a registration landed. Per cuff: device clock and its drift, model,
firmware, serial, poll duration, link state, RSSI, and a battery-low flag whose
meaning is unproven - see the note on it in the source.

Older readings still in the cuff's ring are emitted as `esphome.omron_measurement`
events with their original timestamps, watermarked in flash so each is sent once.

Platforms: `sensor`, `binary_sensor`, `text_sensor`, `button`, `switch`.

## Configuration

On the `omron:` block:

| option | meaning |
|---|---|
| `mac_address` | **required**, the cuff |
| `profile` | **required**, a profile name or `auto` |
| `bindkey` | 32 hex characters you choose, for profiles that store a key rather than bond; see above |
| `time_id` | a time source, used to report clock drift and to set the cuff's clock |
| `history_records` | how deep to read; omit for the whole ring, `0` for the newest only |
| `ignore_records_before` | drop readings older than this date |
| `require_bond`, `keep_bond` | see above |
| `register_as_user` | which user slot this node registers as when pairing |
| `clock_sync_threshold` | drift tolerated before the clock is rewritten; `0s` means every session |
| `auto_connect` | connect on a readable advertisement rather than waiting for the button |
| `full_read_on_pairing` | re-read every ring when the cuff advertises pairing, default off |
| `device_id` | sub-device for the cuff's own entities |
| `users` | one entry per person, see below |

Everything that varies per person goes in one entry under `users:`, so a
platform block only has to say which user it is for:

```yaml
omron:
  - id: omron_cuff
    mac_address: !secret omron_mac
    profile: auto
    register_as_user: 2
    device_id: cuff_device
    users:
      - user: 1
        device_id: user1_device
        name_prefix: Alex
        birth_date: 1955-11-05
        # Needed only for someone this node does not register as.
        write_birth_date: true
      - user: 2
        device_id: user2_device
        name_prefix: Sam

sensor:
  - platform: omron
    omron_id: omron_cuff
    user: 1
```

| per-user option | meaning |
|---|---|
| `device_id` | sub-device this person's entities file under |
| `name_prefix` | prefix for their entity names, defaulting to the sub-device's name |
| `birth_date` | written into their settings block, and read back to confirm it landed |
| `write_birth_date` | send the date without registering as them |

The prefix is not cosmetic. Two people would otherwise both get an entity
called "Systolic blood pressure", and an entity's API key is a hash of its name
with no device in it, so they collapse into one on MQTT and on any client that
addresses by key. Without a prefix or a sub-device name to borrow, it falls
back to "User 1" and "User 2".

## Confidence levels

Every profile states how much is actually known about it, and the node says so at
startup:

| level | count | what it means |
|---|---|---|
| `hardware verified` | 1 | read off a real cuff of that model |
| `reference tested` | 9 | another project reports the model as working |
| `transcribed from a catalog` | 28 | the map has not been confirmed on that device by anyone in the chain |

The last group is the majority, and those profiles log a warning every boot for
that reason. **If your model is in that group, compare the first few readings
against the cuff's own screen.** A map that is internally consistent is not the
same as a map that is right.

## What this does not do

- **No backfill into Home Assistant history.** New measurements arrive seconds
  after the cuff finishes; older ones are emitted as events with their real
  timestamps, and what happens to them is up to you. HA does not accept
  historical states, only hourly statistics buckets, which would merge a TruRead
  series into one point.
- **No live readings during a measurement.** The standard blood pressure
  characteristic is subscribed only while the model is unknown, where it is the
  whole of what a session can offer. With a profile the memory read is the
  source, and nothing else that talks to these cuffs subscribes that
  characteristic alongside one either.
- **No guessing.** An unrecognised model gets no profile, and a cuff whose memory
  map cannot be resolved gets a live-only session rather than an invented map.
- **No writes you did not ask for**, but not none either. Three things go into
  the cuff's memory: the clock, once per session; the "not yet collected" marker
  of every person whose readings were just taken, which is how the cuff is told
  they arrived; and, only in a session that pairs, that user's settings block.
  Nothing else is written, and no measurement is ever altered.

## Requirements

ESP32 with `esp32_ble_tracker`. The component runs its own GATT client and takes
one connection slot. Bonding on ESP32 with these cuffs is delicate and the
sequencing here took a long time to get right; if you are sharing the node with
other BLE clients, raise `esp32_ble.max_connections` accordingly.

## Development

`bash tests/run_protocol_tests.sh` builds the host test suite the way the
firmware is built (`gnu++20`, `-Wall -Wextra -Werror`) and runs it. It ends with
`ok: N test groups passed`; anything else, including a silent exit, is a failure.

Set `OMRON_TEST_LOG=1` to see the component's own log lines while the suite runs.
They are the lines the device prints, and they are off by default so that a run
shows its assertions and nothing else.

## License

GPL-3.0. See [`LICENSE`](LICENSE).
