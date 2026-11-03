# FrameEyeOSC packet (FEO1), version 1

frameeyeosc (on the Steam Frame) sends one UDP datagram per eye sample to the FrameEyeOSC module for
VRCFaceTracking (on the PC). The module turns it into VRCFT's eye data, and VRCFT drives each avatar's parameters
itself (including the pupils, in whatever form the avatar takes them).

From 0.10 the same packets can also go to up to three more destinations ("extra outputs", format `feo1`; see
"Other formats" for `tracking_eye`, the other one). Every destination of one frameeyeosc gets the very same bytes:
each packet is built once per sample, with one `session` and one `sequence` for all of them.

The same file is kept in both repositories (frameeyeosc `docs/protocol.md` and VRCFT-FrameEyeOSC
`docs/protocol.md`); change both together.

## Transport

- UDP, one packet per datagram, nothing else in the datagram. A datagram is at most 1472 bytes.
- Default port **11112** on the PC (the module listens on every interface). It sits next to the LiveLink module's
  11111 and stays clear of VRChat's OSC (9000/9001), the ETVR module (8889) and 8888, which other eye trackers often
  use. Both ends let the port be changed (frameeyeosc's `port` setting, the module's `FrameEyeOSCConfig.json`).
- frameeyeosc sends every eye sample (about 90 a second, up to ~136 while streaming). The module reads every queued
  datagram each update and keeps the newest, so it never falls behind.
- While frameeyeosc has no eye data (the headset off, the eye server stopped) it sends a packet without the
  `TRACKING` flag every 500 ms, with relaxed open eyes looking ahead. It sends one such packet at once when the eye
  data stops, and when it stops sending there (paused, or the output or destination switched). A frameeyeosc that is
  killed sends nothing more; the module's 1 s timeout (below) covers that.
- frameeyeosc only sends. It opens no listening port, does no discovery and takes no remote control: destinations come
  only from its own settings (the panel, config.json, the command line).
- UDP is plaintext and a home network is assumed. With its `lan_only` setting on (the default) frameeyeosc sends only
  to loopback, private, link-local and 100.64/10 (VPN) addresses (for now the Steam Link PC is only warned about).

## Layout

108 bytes with brows, else 76 (the packet stops after `session`); little-endian, no padding. `f32` is IEEE 754
single precision. frameeyeosc sends 76-byte packets, and 108-byte packets with the brows block while
`brow_from_widen` is on (from 0.8.0, off by default). Those brows are raised from the widening (inner_up = outer_up,
lowerer = pinch = 0), since the eye cameras don't see brows. A longer packet carries the extension area after the
brows block (see "Extension area"); frameeyeosc 0.10 sends none.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | `u8[4]` | magic | ASCII `FEO1` (`46 45 4F 31`) |
| 4 | 2 | `u16` | version | `1` |
| 6 | 2 | `u16` | length | Bytes of the packet: `108` with brows, else `76` (more with extension records) |
| 8 | 4 | `u32` | sequence | +1 per packet, wraps; starts at 0 when frameeyeosc starts. It can skip one (a last packet sent to another destination); receivers always take a step forward |
| 12 | 4 | `u32` | flags | See below |
| 16 | 8 | `u64` | time_us | Sender's wall clock, microseconds since the Unix epoch (diagnostics only) |
| 24 | 24 | eye | left | The left eye |
| 48 | 24 | eye | right | The right eye |
| 72 | 4 | `u32` | session | Random and not 0, new each time frameeyeosc starts (see "Ordering") |
| 76 | 16 | brow | left_brow | The left eye's brow (only in a packet of 108 bytes or more) |
| 92 | 16 | brow | right_brow | The right eye's brow (only in a packet of 108 bytes or more) |
| 108 | | records | extension | Extension records (only in a packet longer than 108 bytes) |

Each eye (24 bytes):

| Offset | Type | Field | Range | Meaning |
|---:|---|---|---|---|
| +0 | `f32` | gaze_x | -1..1 | Sideways gaze, + right; 1 = 45° (frameeyeosc's `v2/EyeLeftX`) |
| +4 | `f32` | gaze_y | -1..1 | Up/down gaze, + up; 1 = 45° (frameeyeosc's `v2/EyeLeftY`) |
| +8 | `f32` | eyelid | 0..1 | VRCFT eyelid: 0 closed, 0.75 relaxed open, 1 fully widened (frameeyeosc's `v2/EyeLidLeft`) |
| +12 | `f32` | squint | 0..1 | Eye-camera squint; 0 unless the eye's `CAMERA_LID` flag is set |
| +16 | `f32` | pupil_mm | mm | Pupil diameter in millimetres; 0 unless the eye's `PUPIL` flag is set |
| +20 | `f32` | pupil_dilation | 0..1 | Pupil dilation, 0 smallest to 1 largest for this wearer; 0 unless `PUPIL` is set |

Each eye's gaze is its own with frameeyeosc's "move eyes separately", else both carry the combined gaze (as in
frameeyeosc's other outputs). See "Conventions" for what the gaze and eyelid values mean.

Each brow (16 bytes):

| Offset | Type | Field | Range | Meaning |
|---:|---|---|---|---|
| +0 | `f32` | brow_inner_up | 0..1 | Inner brow raised; 0 unless the eye's `BROW` flag is set |
| +4 | `f32` | brow_outer_up | 0..1 | Outer brow raised; 0 unless `BROW` is set |
| +8 | `f32` | brow_lowerer | 0..1 | Brow lowered; 0 unless `BROW` is set |
| +12 | `f32` | brow_pinch | 0..1 | Brows pinched together; 0 unless `BROW` is set |

### Flags

| Bit | Name | Meaning |
|---:|---|---|
| 0 | `TRACKING` | The eye values are live. Clear: no eye data, the values are relaxed open eyes looking ahead |
| 1 | `LEFT_CAMERA_LID` | The left eye's eyelid comes from the eye camera this sample, and its `squint` is valid |
| 2 | `RIGHT_CAMERA_LID` | The same for the right eye |
| 3 | `LEFT_PUPIL` | The left eye's `pupil_mm` and `pupil_dilation` are valid |
| 4 | `RIGHT_PUPIL` | The same for the right eye |
| 5 | `LEFT_BROW` | The left eye's brow fields are valid (only counts in a packet with the brows block) |
| 6 | `RIGHT_BROW` | The same for the right eye |
| 7-31 | | Reserved: sent as 0; receivers MUST ignore them |

## Conventions

These hold for every format frameeyeosc sends (FEO1 here, `tracking_eye` below, and its VRChat and LiveLink outputs).

- **Gaze** is relative to the headset and linear in angle: ±1 = ±45°, clamped to -1..1, + right and + up. It is the
  gaze after the eye fit and after the user's gaze scale: the same values the main output sends and the panel's gaze
  dots show. A raw or unscaled variant would only ever come as a new opt-in setting or extension record for one
  destination; the default never changes.
- **Eyelids** are on VRCFT's scale: 0 closed, 0.75 relaxed open, 1 fully widened.
- **Time:** `time_us` is the sender's wall clock and is for diagnostics only (the two clocks are not in step).
  Receivers judge a packet's age by when it arrives.
- **Pause:** while sending is paused, nothing goes to any destination. On pausing, and when a destination's host, port
  or format changes, the old destination first gets its normal final message (FEO1: a packet without `TRACKING`).

## Receiving rules

- Drop a datagram that is shorter than 24 bytes, does not start with `FEO1`, has another `version`, has `length`
  below 72 or above 1472, or is shorter than `length`. Receivers MUST accept any `length` from 72 to 1472 with
  `version` 1, and MUST ignore the bytes they don't understand. A packet with `length` 72
  to 75 (the first draft, without `session`) is read with `session` 0.
- Reserved flag bits 7-31 MUST be ignored.
- The brows block is read only when `length` is at least 108. A packet with `length` 72 to 107 has no brows, whatever
  its `BROW` flags say, and anything after `session` in it is ignored.
- Drop a packet with any non-finite float, the brow floats included when the brows block is read. (A non-finite float
  in an extension record only makes that record unusable, not the packet.)
- Brow values are clamped to 0..1.
- Ordering: a packet whose `session` differs from the newest one taken is taken, and the window below starts over
  from it: frameeyeosc started again, and its `sequence` with it. From then on a packet from a session before (not 0;
  the module remembers the last 4) is a straggler from an old run and is dropped. Within one session, a packet whose
  `sequence` is at most 1000 behind the newest one taken (`(int32)(sequence - newest)` in -1000..0) is a duplicate or
  late and is dropped; anything else is taken (a jump back further than that is also read as a restart, for senders
  without a `session`). After a timeout, any packet is taken.

## Extension area

The framing is fixed now; no record types are assigned yet, and frameeyeosc 0.10 sends none.

- A packet whose `length` is above 108 always contains the brows block (zeros with both `BROW` flags clear when no
  brows are sent), and its extension area is bytes 108 up to `length`. No flag bit signals it: `length` alone does.
- The area is a run of records. Each is a little-endian `u16 type` and `u16 len` (payload bytes), then the payload,
  then zeros up to the next multiple of 4 bytes, which `len` does not count.
- The sender counts the last record's padding in `length`; a receiver accepts a `length` with or without it (the
  padding is never read).
- Receivers skip a type they don't know by its `len`. A type appears at most once per packet; if a type appears again
  anyway, the receiver uses the first one and skips the others. A payload only ever
  grows by appending, so a receiver reads `min(len, what it knows)`. A record that runs past `length` ends the
  parsing (the fixed part is still used), and so does type `0x0000` or `0xFFFF`.
- Types: `0x0000` and `0xFFFF` are invalid; `0x0001`-`0x7FFF` are assigned only in this file, and never reused;
  `0x8000`-`0xEFFF` are experimental and never in a release build; `0xF000`-`0xFFFE` are reserved for an integrity or
  authentication tag, which must be the last record.
- A record is sent only to a destination that asks for it with a setting of its own. The main output and every
  default packet stay 76 or 108 bytes for good.

## Compatibility

Frozen for good: offsets 0-107 with their units and meanings, flag bits 0-6, the session and sequence rules, and
`version` 1.

- **Additions** come as extension records (above), never as new fixed fields or flag bits.
- **Deprecation:** a field keeps its offset, its flag is cleared, it carries a neutral value, and it is documented as
  "deprecated since x.y". It is never removed and never reused.
- **A breaking change** gets a separate format, `feo2`, that a destination has to choose; `feo1` stays the default
  and stays supported. (A different `version` is dropped by every receiver of today, so it is never used for an
  addition.)
- The format names `feo1` and `tracking_eye` are never renamed or changed; any variant (per-eye `tracking_eye`, VMC,
  ...) gets a new name.
- **Not contracts:** status.json, the panel's gaze-dot stream (`FEOD`) and the panel's `--action` commands are
  frameeyeosc's own and change without notice. Only this file is a contract.

## How the module maps it to VRCFT

Per eye, from a `TRACKING` packet:

- `Gaze = (gaze_x, gaze_y)` as is. VRCFT sends Gaze on as `v2/EyeLeftX/Y` and turns it into VRChat's eye angles as
  `atan(x)` / `-atan(y)`, which is what frameeyeosc's LiveLink and VRChat outputs give it too.
- `Openness = clamp(eyelid / 0.75, 0, 1)` and the `EyeWide` shape `= clamp((eyelid - 0.75) / 0.25, 0, 1)`, so VRCFT's
  `v2/EyeLid` (`Openness * 0.75 + EyeWide * 0.25`) comes back to `eyelid`.
- The `EyeSquint` shape `= squint` while `CAMERA_LID` is set, else 0.
- Pupils, while at least one eye's `PUPIL` is set (an eye without it takes the other eye's values):
  `PupilDiameter_MM = pupil_mm` (VRCFT's `v2/PupilDiameterLeft/Right/PupilDiameter` are those in cm, like
  frameeyeosc's VRChat output), and VRCFT's dilation range is set around their mean `A` so that its
  `v2/PupilDilation` (`(A - min) / (max - min)`) is the mean `pupil_dilation` `D`: `min = A - D * S`,
  `max = min + S`. The default `S` is 2.655 mm (eyecam's default dilation range). If floating-point rounding changes
  `D`, the module tries a power-of-two range with `D * S` between `A` and `2A` (or `S = 1` when `A = 0`). It uses
  that range only if its bounds are finite, avoid VRCFT's reserved "unset" value and recover `D` exactly. This preserves
  boolean and bit-packed thresholds without changing either measured diameter. Negative mean diameters and extreme
  inputs that cannot use a finite exact range retain the default bounds. Without any `PUPIL` the pupils are left as they were.
- Brows, while the packet has the brows block and the eye's `BROW` flag is set: the `BrowInnerUp`, `BrowOuterUp`,
  `BrowLowerer` and `BrowPinch` shapes `= brow_inner_up`, `brow_outer_up`, `brow_lowerer`, `brow_pinch`. Otherwise
  that eye's 4 brow shapes are left alone (unset, so VRCFT leaves the brows to whatever else drives them), except that
  brows this module set before are set to 0 once, so a brow doesn't stick when the brows stop coming.
- A packet without `TRACKING`, or no packet for 1 s: relaxed open eyes looking ahead (`Openness` 1, `EyeWide` 0,
  `EyeSquint` 0, `Gaze` 0); the pupils stay; brows as without `BROW` (set to 0 once if this module set them).

## Example

Sequence 7, flags `TRACKING | LEFT_CAMERA_LID | LEFT_PUPIL | RIGHT_PUPIL` (27), time_us 1700000000123456;
left eye 0.25, -0.5, 0.875, 0.5, 4.0, 0.25; right eye 0.125, -0.5, 0.375, 0.0, 3.5, 0.75; session 0x2a5f11c3; no
brows (76 bytes):

```
46454f3101004c00070000001b00000040222018240a0600
0000803e000000bf0000603f0000003f000080400000803e
0000003e000000bf0000c03e00000000000060400000403f
c3115f2a
```

## Test vectors

Both repositories' tests run these through their receivers.

1. The example above (76 bytes).
2. The same packet with the brows block (108 bytes): flags 123 (the example's 27 with `LEFT_BROW | RIGHT_BROW`), the
   left brow 0.5, 0.5, 0, 0 and the right brow 0.25, 0.25, 0, 0:

   ```
   46454f3101006c00070000007b00000040222018240a0600
   0000803e000000bf0000603f0000003f000080400000803e
   0000003e000000bf0000c03e00000000000060400000403f
   c3115f2a0000003f0000003f00000000000000000000803e
   0000803e0000000000000000
   ```

3. Vector 2 with `length` 140, flag bits 7-31 set (flags `0xFFFFFFFB`) and one record of the unassigned type `0x0FFF`
   with a 28-byte payload (bytes 1 to 28). It must be read exactly like vector 2:

   ```
   46454f3101008c0007000000fbffffff40222018240a0600
   0000803e000000bf0000603f0000003f000080400000803e
   0000003e000000bf0000c03e00000000000060400000403f
   c3115f2a0000003f0000003f00000000000000000000803e
   0000803e0000000000000000ff0f1c000102030405060708
   090a0b0c0d0e0f101112131415161718191a1b1c
   ```

4. Vector 2 with `length` 116 and a record whose `len` (64) runs past `length`. The record is dropped and the fixed
   part read like vector 2:

   ```
   46454f3101007400070000007b00000040222018240a0600
   0000803e000000bf0000603f0000003f000080400000803e
   0000003e000000bf0000c03e00000000000060400000403f
   c3115f2a0000003f0000003f00000000000000000000803e
   0000803e0000000000000000ff0f400001020304
   ```

## Other formats: `tracking_eye`

An extra output in the `tracking_eye` format sends VRChat's own eye tracking input, for apps that read it (a gaze or
foveation tool, say): plain OSC 1.0 messages, one per UDP datagram, no bundles and no time tags. frameeyeosc never adds
addresses to this set.

| Address | Tags | Value | Sent |
|---|---|---|---|
| `/tracking/eye/CenterVec` | `,fff` | A unit vector `normalize(tan(gaze_x·π/4), tan(gaze_y·π/4), 1)` for the combined gaze (see "Conventions"): Unity axes, +x right, +y up, +z forward | Every sample |
| `/tracking/eye/EyesClosedAmount` | `,f` | `clamp(1 - (eyelid_left + eyelid_right) / 2 / 0.75, 0, 1)`: 0 open (relaxed or widened), 1 closed | Every sample, only with the destination's `lids` on |

- The set of addresses depends only on the destination's own `lids` setting. It is always `CenterVec` (also with "move
  eyes separately"), never `PitchYaw` or `LeftRightVec`, and never pupils, squint or brows.
- When the eye data stops, when sending pauses, and when the destination changes or is turned off: one `CenterVec`
  0, 0, 1 (and `EyesClosedAmount` 0 with `lids`), then nothing. There is no keepalive: silence is this format's "no
  data". (VRChat falls back after its own timeout.)
- A `tracking_eye` destination on the port where Steam Link's own eye OSC arrives means two senders on one port; turn
  one of them off.
