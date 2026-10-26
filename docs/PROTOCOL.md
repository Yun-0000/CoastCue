# CoastCue Protocol

This is the wire contract between the boat, the shore and the paired iPhone:
signed LoRa messages and the BLE interface. CoastCue accepts only the message
forms, UUIDs, framing, signing and state rules defined here. Any change must
update the boat firmware, shore firmware, iOS app, tests and this document
together before field use.

All signaling is advisory. It is not a delivery guarantee, a remote-control or
chat channel, a collision-avoidance system or a rescue dispatch system.

## Terms

| Term | Meaning |
| --- | --- |
| Incident | One Help or SOS condition, tracked until Resolved. |
| Receipt | The shore has durably accepted the Incident. It is not an operator decision or a rescue promise. |
| Acknowledged | An operator saw the Incident and silenced the alarm. The Incident stays active. |
| Resolved | Final operator state. It never goes back to Acknowledged. |
| Incident ID | Created by the boat and kept across retries. Separate from the per-transmission nonce and the shore's record ID. |

## LoRa message forms

There are four signed message forms:

- routine telemetry from boat to shore;
- an Incident uplink from boat to shore;
- an Incident-state downlink with `kind: "incident_state"`;
- a boat confirmation with `kind: "state_receipt"`.

Boat telemetry and Incident uplinks omit `kind`. Their signed `status` tells
`normal` telemetry from `help` or `sos`, and the compact `i` field identifies an
Incident. This keeps a packet with coordinates, GPS age and battery inside the
255-byte limit.

Boat frames also omit `boat_id`. The signed `node_uid` identifies the sender,
and the one-boat shore adds its configured `boat_id` only when it writes the
journal and app data.

## Signing and validation

Each LoRa frame is one UTF-8 JSON object of at most 255 bytes, the SX127x
transport limit. The sender writes its unsigned object in a fixed field order,
computes HMAC-SHA-256 over those exact bytes, truncates the result to 32
lowercase hex characters and appends `,"mac":"…"` as the last field. The
receiver checks the MAC over the exact bytes before it acts on the message.

Every transmission has a fresh `nonce` of exactly 16 lowercase hex characters.
The shore rejects a malformed, duplicate or replayed uplink nonce. It spots a
repeat by checking the nonces still in its journal and the last 64 it received,
so once a full journal drops its oldest rows, a frame recorded before then
could be accepted again. The boat checks the downlink nonce format and uses the
Incident ID plus the increasing `state_seq` to ignore repeats, so a repeated
downlink cannot move state backwards.

`node_uid` and `target_node_uid` must not be empty. Incident IDs are exactly 16
lowercase hex characters. `state_seq` is a positive integer in the unsigned
32-bit range. Receivers reject duplicate known top-level fields, fields that
belong to the other direction, and a message whose form does not match its
signed discriminator. Unknown fields may be ignored, but only after the MAC and
the required fields have been checked.

## Routine telemetry uplink

Unsigned field order: `nonce`, `node_uid`, optional `i`, `status`, `gps`,
optional `battery_percent`; `mac` is appended after signing:

```json
{"nonce":"0123abcd4567ef89","node_uid":"boat-node","status":"normal","gps":{"valid":true,"latitude":12.345678,"longitude":98.765432,"age_s":2},"battery_percent":85,"mac":"..."}
```

Required fields are `nonce`, `node_uid`, `status: "normal"`, `gps` and `mac`.
`gps.valid` is boolean. A valid fix needs checked `latitude` and `longitude`;
a bounded `age_s` is optional. `battery_percent`, when present, is an integer
from 0 to 100.

## Incident uplink

Unsigned field order: `nonce`, `node_uid`, Incident ID `i`, `status`, `gps`,
optional `battery_percent`; `mac` is appended after signing:

```json
{"nonce":"89abcdef01234567","node_uid":"boat-node","i":"0123abcd4567ef89","status":"help","gps":{"valid":true,"latitude":12.345678,"longitude":98.765432,"age_s":2},"battery_percent":85,"mac":"..."}
```

Required fields are `nonce`, `node_uid`, `i`, `status`, `gps` and `mac`.
`status` is `help` or `sos`; GPS and battery rules match routine telemetry. The
boat creates the Incident ID with the Incident and keeps it for every retry,
with a fresh nonce and current GPS data each time.

The shore checks the MAC and fields, drops duplicates and appends the Incident
to its journal before it counts the Incident as accepted. Only a verified
journal append creates the automatic Receipt. A failed or live-only append
creates none.

The boat first sends a bounded burst of up to three frames and stops early
once it has accepted and stored a shore state. While the Incident is still
Open, later reporting slots carry the same Incident ID until a Receipt or a
later state arrives. This avoids a fast endless retry loop without dropping an
Incident the shore has not received.

Pressing Help again, or SOS again, keeps the current Incident ID while the
Incident is not terminal. Help→SOS starts a new SOS Incident in Open, and the
shore supersedes the unresolved Help. The new ID forces a new alarm and a new
operator decision, so an Acknowledged Help can never silence a later SOS. A late
state frame for the old Help ID cannot change the SOS.

## Shore-to-boat Incident state

Unsigned field order: `kind`, `nonce`, `node_uid`, `target_node_uid`,
`incident_id`, `state`, `state_seq`; `mac` is appended after signing:

```json
{"kind":"incident_state","nonce":"fedcba9876543210","node_uid":"shore-node","target_node_uid":"boat-node","incident_id":"0123abcd4567ef89","state":"receipt","state_seq":1,"mac":"..."}
```

All fields are required.

- `target_node_uid` must match the boat; it ignores any other downlink.
- `state` is `receipt`, `acknowledged` or `resolved`.
- `state_seq` is the shore's revision number for the Incident and only goes
  up. A retransmission keeps its `state_seq` and uses a fresh nonce.
- `receipt` is sent automatically after the journal append described above.
- `acknowledged` and `resolved` are operator decisions at the shore. A
  paired-phone `ack` or `resolve` takes effect on its own, with no second
  confirmation on the shore unit. The shore button makes the same transitions
  when the phone is unavailable. Neither is a remote command to the boat.

The shore keeps pending revisions and replays them in later response slots
until the boat confirms them. A higher revision replaces lower pending ones for
the same Incident, so Resolved replaces any pending Receipt or Acknowledged.

## Operator lifecycle

An Incident starts Open once it is stored. From Open the operator may go to
Acknowledged or straight to Resolved; Acknowledged may later become Resolved.

Resolved is terminal. Later emergency uplinks with the same Incident ID do not
reopen it. Once the boat has accepted Resolved, it ignores a late Acknowledged
frame even with a valid MAC. In every state, the boat ignores a revision lower
than the latest `state_seq` it accepted for that Incident.

If shore storage is unavailable, a button click only mutes that unit's buzzer.
It creates no Acknowledged or Resolved transition, queues no downlink and leaves
the Incident Open; a new alert or a reboot brings the sound back.

## Boat state receipt

After the boat accepts an addressed downlink and saves it in its local Incident
checkpoint, it confirms it in a later uplink and receive cycle. Unsigned field
order: `kind`, `nonce`, `node_uid`, `incident_id`, `state_seq`; `mac` is
appended after signing:

```json
{"kind":"state_receipt","nonce":"0011223344556677","node_uid":"boat-node","incident_id":"0123abcd4567ef89","state_seq":3,"mac":"..."}
```

`state_seq` is the highest revision the boat has stored. If the boat cannot
update its Preferences checkpoint, it sends no state receipt, and the shore
keeps the revision for replay. The shore accepts a state receipt only when its
Incident ID and `state_seq` match a pending revision; it does not check the
sender's `node_uid`, which makes no difference with one boat. It then stops
replaying that revision and older ones. A missing, invalid or stale state receipt leaves the
revision pending.

## Half-duplex timing

The radio is half duplex, and the shore normally listens. Routine telemetry is
best effort and gets no ACK. The shore transmits only in the short receive
window that the boat opens right after its own uplink. It sends at most one
pending Incident-state frame per window and then returns to receive. Downlinks
therefore cannot become a general command channel.

While an Incident is not terminal, the boat opens a window after every
emergency transmission and every heartbeat. Heartbeats run every 3 s while it
listens, so a shore transition normally reaches the boat within one heartbeat;
a lost frame waits for a later window. A routine telemetry uplink gets a
response only when the shore already has a pending revision; telemetry never
causes one by itself. While the boat listens, its
telemetry carries the Incident's `i`. Telemetry from the active boat without
`i` means the boat has stopped listening, and the shore drops the pending
revisions instead of replaying them forever.

After accepting Resolved, the boat keeps opening windows on later heartbeats
until two in a row are quiet. A repeated Resolved frame resets that count and
gets another state receipt.

## Paired-iPhone BLE service

The shore advertises `CoastCue Shore` and offers this encrypted GATT service to
one paired iPhone:

| Item | UUID | Access |
| --- | --- | --- |
| Service | `7A1F0001-6B7C-4D8E-9F10-112233445566` | — |
| Status | `7A1F0002-6B7C-4D8E-9F10-112233445566` | read, notify |
| Events | `7A1F0003-6B7C-4D8E-9F10-112233445566` | notify |
| Command | `7A1F0004-6B7C-4D8E-9F10-112233445566` | write with response |
| Device info | `7A1F0005-6B7C-4D8E-9F10-112233445566` | read |

Device info is JSON with `name`, `node_uid`, `journal_epoch` and `firmware`.

Pairing is gated by the shore button: holding it for three seconds opens one
120-second pairing window, and pairing is closed at all other times. A first
connection only creates an untrusted candidate. The user enters the six-digit
passkey shown on the shore in the iOS pairing prompt and then confirms the
gateway in the app. Only then does the app pin both the CoreBluetooth
peripheral identifier and the full gateway identity. Later connections must
match both, and replacing the gateway needs the explicit "Forget shore gateway"
action. Status and Events that arrive before trust are not applied.

### Status JSON

Status is compact UTF-8 JSON of at most 182 bytes. Its keys are `f`, `c`, `n`,
`a`, `h`, `r`, `q`, `s`, `i`, `e` and `p`: storage-ready flag, journal count,
latest node UID, last-packet age, radio health, RSSI, SNR/quality, alert state,
active record ID, active severity and pairing-open flag. Values that are not
available may be left out. `s` is exactly `open`, `acknowledged` or
`resolved`; `receipt` is not an Incident lifecycle value and never appears in
`s`. `p` is true only during the 120-second pairing window. Status never
contains `boat_id`.

### Events framing and checksum

Events are UTF-8 JSON objects: `event`, `transition`, `sync_complete` and
`command_result`. An object that fits in 160 bytes and in the negotiated ATT
value size is sent as is. Larger objects are split into `SM`-framed chunks of
at most 160 JSON bytes, or fewer when the negotiated ATT payload is below 176
bytes:

| Byte range | Value |
| --- | --- |
| 0–1 | ASCII `SM` |
| 2 | format marker `1` |
| 3 | message type `1` (event JSON) |
| 4–7 | message ID, little-endian `uint32` |
| 8–9 | zero-based fragment index, little-endian `uint16` |
| 10–11 | fragment count, little-endian `uint16` |
| 12–15 | CRC-32 of the complete JSON, little-endian `uint32` |
| 16… | fragment bytes |

CRC-32 is the reflected IEEE form (initial `0xFFFFFFFF`, polynomial
`0xEDB88320`, final XOR `0xFFFFFFFF`). The receiver joins fragments by index,
checks the CRC, parses the JSON and drops an incomplete message after 15
seconds. A payload without the `SM` header is parsed directly as JSON.

### Event and transition JSON

Every durable journal row has a positive `record_id`. During a sync the shore
sends the row unchanged and adds the request's `sync_id`; live notifications
have no `sync_id`. The iPhone moves its sync cursor only on journal rows. It
uses the event nonce, scoped by gateway and boat identity, as the stable key
when a live-only emergency is later promoted to a journal row.

An `event` row:

| Field | Requirement and meaning |
| --- | --- |
| `kind` | Required; exactly `event`. |
| `record_id` | Required positive journal ID, or an explicitly non-durable live-only ID. |
| `uptime_ms` | Required shore uptime when received. |
| `unix_ms`, `time_quality` | `unix_ms` only once the phone has set the time; quality is `phone_set` or `uptime_only`. |
| `rssi`, `snr`, `packet_b64` | Radio readings and the original signed LoRa frame. |
| `nonce`, `status` | Boat transmission nonce and `normal`, `help` or `sos`. |
| `node_uid`, `boat_id` | Signed boat node and the shore's configured display ID. |
| `latitude`, `longitude`, `gps_age_s` | Only for a checked location; age is optional. |
| `incident_id`, `radio_state_seq` | For current Incident events; the first durable revision is `1`. |
| `battery_percent` | Optional integer from 0 to 100. |
| `alert_state` | For Help/SOS: `open`, or `resolved` if the event was absorbed. |
| `supersedes_record_id` | When this Help/SOS replaces the active record. The old alert becomes `superseded`; the new one is Open and needs fresh attention. |
| `source` | `absorbed` only when a Help arrives under an active SOS and must not reopen it. |
| `durable` | Required on live notifications. Only `true` rows belong to journal sync and may move the cursor. |
| `recovered_from_checkpoint`, `promotes_record_id` | Optional recovery data. Promotion replaces the matching live-only row in place and does not create a second Incident. |

A `transition` row records a state change of an existing alert:

| Field | Requirement and meaning |
| --- | --- |
| `kind` | Required; exactly `transition`. |
| `record_id`, `target_record_id` | New journal row and the alert it changes. |
| `state` | `acknowledged`, `resolved` or `superseded`. |
| `source` | `ble`, `button`, `superseded` or `absorbed`. |
| `incident_id`, `radio_state_seq` | For operator Acknowledged/Resolved revisions sent back to the boat. |
| `uptime_ms`, `unix_ms`, `time_quality` | Same as for an event. |
| `durable` | Always true for a published transition. A failed append is neither published nor applied as operator state. |

`superseded` is a local alert-history state only and is never sent over LoRa. A
replacement event's `supersedes_record_id` counts even if the matching
`transition` row could not be appended, so clients must apply both without
double effects. An absorbed Help is kept as a resolved history row, and the
active SOS keeps its Acknowledged/Resolved progress.

### Commands and sync

Commands are compact JSON:

- `{"op":"sync","after":<record_id>,"limit":<1..20>,"sync_id":"<id>","unix_ms":<optional epoch ms>}`
- `{"op":"ack","record_id":<active record id>,"command_id":"<unique id>"}`
- `{"op":"resolve","record_id":<active record id>,"command_id":"<unique id>"}`
- `{"op":"time","unix_ms":<epoch ms>}`

Results:

- `sync` returns zero or more `event` or `transition` objects, then exactly one
  `sync_complete` with `first_record_id`, `last_record_id`, `count`,
  `has_more`, `truncated`, `storage_ready` and `sync_id`. It may also list
  missing or truncated record ranges.
- If the shore cannot start or finish a sync page, it sends a `command_result`
  with `op` = `sync`, the request's `sync_id`, `ok` = `false` and an error such
  as `busy`, `notify_failed` or `storage`, instead of `sync_complete`.
- `ack` and `resolve` echo their `command_id` in a `command_result`. The phone
  matches results by `command_id` only and ignores results it cannot match. A
  sync failure has no `command_id`, so the phone ignores it too; the sync page
  then times out after 20 s and is retried once.
- In a successful alert result, `record_id` names the alert the shore changed,
  and the phone updates that alert as well as the one it asked about. `source`
  is for information only.
- Operator results go ahead of sync traffic, and a committed operator
  transition is never dropped silently. If the success result cannot be
  delivered on Events, the shore sends a matching `command_result` with
  `ok` = `false` and `error` = `notify_failed`, or the phone may see the
  updated Status `s` value.
- Invalid JSON, an invalid time, an unsupported operation or an inactive alert
  returns `ok: false` with an error code.
