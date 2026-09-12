# Sensor FX Binder

A [Sensor Hub](../sensor-hub/readme.md) *consumer* usermod that binds live
sensor readings to WLED's existing effect/state parameters **generically**
- without writing a bespoke effect per sensor, unlike
[`particle-tilt-effect`](../particle-tilt-effect/readme.md) (which
hardcodes one sensor triple to one hand-written effect). This works with
*whichever effect is currently selected* on the target segment - there is
deliberately no check on which effect is active, since that genericity is
the entire point.

Scope note: despite the name, this also covers brightness and color, not
just "FX" (effect) sliders - the name leads with the effect-slider use case
since that's the primary one.

## Bindable targets

Each row below is independently optional - leaving its **Sensor** dropdown
on "None" leaves that target completely untouched, so the normal
Effects-tab slider/color picker still works for anything not bound here.

**Continuous** (sensor value linearly mapped from `In-Min..In-Max` to
`Out-Min..Out-Max`):

| Target | Segment field | Notes |
|---|---|---|
| Speed | `speed` | |
| Intensity | `intensity` | |
| Custom1 | `custom1` | |
| Custom2 | `custom2` | |
| Custom3 | `custom3` | Only 0-31 representable (5-bit field) - Out-Max above 31 is clamped |
| Brightness | global `bri` | Not segment-scoped |
| Color 1 channels A/B/C | segment's primary color (`colors[0]`) | RGB or HSV, see below |
| Color 2 channels A/B/C | segment's secondary color (`colors[1]`) | RGB or HSV, see below |
| Color 3 channels A/B/C | segment's tertiary color (`colors[2]`) | RGB or HSV, see below |

Each color slot has its own **Mode** setting, **RGB** or **HSV**, that decides
what channels A/B/C mean for that slot:

| Mode | Channel A | Channel B | Channel C |
|---|---|---|---|
| RGB (default) | Red | Green | Blue |
| HSV | Hue | Saturation | Value |

In HSV mode, the three channels are converted to RGB (via WLED's own
`CHSV32`/`CRGBW`, the same conversion WLED's own color picker uses) before
being written - so e.g. binding only Channel A (Hue) while leaving B/C
unbound still works: the starting Saturation/Value are read back from the
segment's current color, not left at zero. Each color slot is composed and
written in a single `setColor()` call per tick, not one call per channel.

**Threshold** (sensor crosses a value, with hysteresis, to on/off):

| Target | Segment field |
|---|---|
| Check1 | `check1` |
| Check2 | `check2` |
| Check3 | `check3` |

A threshold row bound to an already-binary sensor (e.g. a Motion or
Contact sensor) is driven directly - the threshold/hysteresis fields are
ignored in that case.

## Known limitation

Many WLED effects only read `custom1-3`/`check1-3` **once**, when the
effect (re)initializes, not every frame - confirmed by
`particle-tilt-effect`'s own `mode_sensorhub_tilt()`, which only reads its
wrap-mode checkbox at initialization. A binding to Custom1-3/Check1-3 may
have **no visible effect** on some effects until something else causes
that effect to reset (changing the effect and back, for example).
Speed/Intensity/Brightness/Color don't have this limitation - virtually
every effect reads those live, every frame.

## Hardware / wiring

None - this usermod only talks to the Sensor Hub (`getValueByName()`/
`getValueBinaryByName()`) and to WLED's own segment/brightness/color state.
It requires the [Sensor Hub](../sensor-hub/readme.md) usermod and at least
one attached sensor provider to be present in the same build to be useful.

## Usage

Add `sensor-fx-binder` to `custom_usermods` next to the
[Sensor Hub](../sensor-hub/readme.md) and whichever provider(s) you want
to bind from.

## Usermod Settings

| Setting | Default | Description |
|---|---|---|
| Enabled | on | Master on/off switch |
| Segment ID | 0 | Which segment the segment-scoped targets apply to (brightness is global) |
| Update interval | 100 ms | How often bindings are re-evaluated and (if changed) applied |
| Color 1/2/3 Mode | RGB | RGB or HSV - see the color table above |
| *(per target)* Sensor | None | Which attached sensor drives this target - populated from whatever the Sensor Hub currently has attached |
| *(continuous targets)* In-Min / In-Max | 0 / 100 | Expected sensor value range - calibrate per binding, these are placeholders |
| *(continuous targets)* Out-Min / Out-Max | 0 / 255 (31 for Custom3) | Output range written to the target; Out-Min > Out-Max inverts the mapping |
| *(threshold targets)* Threshold | 50 | Numeric sensors: on above this value |
| *(threshold targets)* Hysteresis | 0 | Deadband around Threshold to avoid flicker right at the boundary |
