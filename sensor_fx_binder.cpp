#include "wled.h"
#include "sensor_bus.h"

/*
 * Sensor FX Binder - binds live Sensor Hub readings to WLED's existing
 * effect/state parameters, generically, without writing a bespoke effect
 * per sensor (unlike ../particle-tilt-effect, which hardcodes one sensor
 * triple to one hand-written effect). This works with whichever effect is
 * currently selected on the target segment - there is deliberately no
 * check on which effect is active, since that genericity is the entire
 * point of this usermod.
 *
 * Scope note: despite the name, this also covers brightness and color,
 * not just "FX" (speed/intensity/custom1-3) sliders - the name leads with
 * the effect-slider use case since that's the primary one.
 *
 * Bindable targets, each independently optional (a "None" sensor leaves it
 * completely untouched, so the normal Effects-tab slider/color picker
 * still works for anything not bound here):
 *   - Continuous, linearly mapped (sensor inMin..inMax -> output outMin..outMax):
 *     Speed, Intensity, Custom1, Custom2, Custom3, Brightness, Red, Green, Blue.
 *   - Threshold (sensor crosses a value, with hysteresis -> on/off):
 *     Check1, Check2, Check3. A sensor that's already binary (Motion/
 *     Contact) is used directly instead, ignoring the threshold/hysteresis.
 *
 * Known limitation (not fixable generically - see readme.md): many WLED
 * effects only read custom1-3/check1-3 once, when the effect (re)initializes
 * (SEGMENT.call==0), not every frame - see ../particle-tilt-effect's own
 * mode_sensorhub_tilt(), which only reads SEGMENT.check3 at call==0. A
 * binding to those fields may have no visible effect until something else
 * causes the active effect to reset. Speed/Intensity/Brightness/Color
 * don't have this limitation - those are read live by essentially every effect.
 */

enum class ContinuousTarget : uint8_t { Speed, Intensity, Custom1, Custom2, Custom3, Brightness, Red, Green, Blue };
enum class ThresholdTarget  : uint8_t { Check1, Check2, Check3 };

// One row of the "continuous, linearly mapped" UI/config table (9 rows).
// Config/UI code treats all 9 uniformly (looped); the *apply* step does not
// (see SensorFxBinderUsermod::apply* methods) - Custom3/Red/Green/Blue each
// need field-specific handling that can't be generalized (bitfield / shared
// color composition), so this struct is purely data, not "the whole story".
struct ContinuousBinding {
  const char* jsonKey; // also used verbatim as the addToConfig()/JSON key
  ContinuousTarget target;
  String sensor;        // attached sensor name to read via getValueByName(); "" = unbound
  float inMin, inMax;    // sensor value range this binding expects
  uint8_t outMin, outMax; // output range written to the target (may be inverted, outMin > outMax is fine)
  int16_t lastApplied;   // -1 = never applied yet; used to only write/notify on an actual change

  ContinuousBinding(const char* key, ContinuousTarget t, uint8_t defaultOutMax = 255)
    : jsonKey(key), target(t), sensor(""), inMin(0.0f), inMax(100.0f),
      outMin(0), outMax(defaultOutMax), lastApplied(-1) {}
};

// One row of the "threshold" UI/config table (3 rows, check1-3).
struct ThresholdBinding {
  const char* jsonKey;
  ThresholdTarget target;
  String sensor;         // "" = unbound
  float threshold;
  float hysteresis;      // deadband around 'threshold' to avoid flicker right at the boundary
  bool lastApplied;
  bool hasApplied;       // false until the first successful evaluation

  ThresholdBinding(const char* key, ThresholdTarget t)
    : jsonKey(key), target(t), sensor(""), threshold(50.0f), hysteresis(0.0f),
      lastApplied(false), hasApplied(false) {}
};

class SensorFxBinderUsermod : public Usermod {
  private:
    SensorHub* hub = nullptr;

    bool enabled = true;
    unsigned long lastUpdate = 0;

    // config
    uint8_t segmentId = 0;         // which segment the segment-scoped targets below apply to; brightness is global/exempt
    uint16_t updateIntervalMs = 100;

    ContinuousBinding continuous[9] = {
      ContinuousBinding("speed",      ContinuousTarget::Speed),
      ContinuousBinding("intensity",  ContinuousTarget::Intensity),
      ContinuousBinding("custom1",    ContinuousTarget::Custom1),
      ContinuousBinding("custom2",    ContinuousTarget::Custom2),
      ContinuousBinding("custom3",    ContinuousTarget::Custom3, 31), // uint8_t custom3 : 5 - only 0-31 is representable
      ContinuousBinding("brightness", ContinuousTarget::Brightness),
      ContinuousBinding("red",        ContinuousTarget::Red),
      ContinuousBinding("green",      ContinuousTarget::Green),
      ContinuousBinding("blue",       ContinuousTarget::Blue),
    };
    ThresholdBinding thresholds[3] = {
      ThresholdBinding("check1", ThresholdTarget::Check1),
      ThresholdBinding("check2", ThresholdTarget::Check2),
      ThresholdBinding("check3", ThresholdTarget::Check3),
    };

    static const char _name[];
    static const char _enabled[];
    static const char _segmentId[];
    static const char _updateInterval[];

    ContinuousBinding& cont(ContinuousTarget t) {
      for (auto& c : continuous) if (c.target == t) return c;
      return continuous[0]; // unreachable - every target above is always present
    }
    ThresholdBinding& thresh(ThresholdTarget t) {
      for (auto& b : thresholds) if (b.target == t) return b;
      return thresholds[0]; // unreachable
    }

    static uint8_t mapContinuous(float raw, float inMin, float inMax, uint8_t outMin, uint8_t outMax) {
      float t = (inMax != inMin) ? (raw - inMin) / (inMax - inMin) : 0.0f;
      t = constrain(t, 0.0f, 1.0f);
      return (uint8_t)roundf(outMin + t * ((float)outMax - (float)outMin));
    }

    // Speed/Intensity/Custom1/Custom2 are plain uint8_t fields - genuinely
    // generalizable via pointer-to-member. Custom3 is a `uint8_t : 5`
    // bitfield (like check1-3 below) - C++ can't take a pointer/reference
    // to a bitfield member, so it's handled separately, with an explicit
    // <=31 clamp regardless of the configured outMax (an out-of-range
    // bitfield assignment silently wraps mod 32 instead of saturating).
    struct ScalarField { ContinuousTarget target; uint8_t Segment::* field; };
    static constexpr ScalarField _scalarFields[4] = {
      { ContinuousTarget::Speed,     &Segment::speed },
      { ContinuousTarget::Intensity, &Segment::intensity },
      { ContinuousTarget::Custom1,   &Segment::custom1 },
      { ContinuousTarget::Custom2,   &Segment::custom2 },
    };

    bool applySegmentScalars(Segment& seg) {
      bool changed = false;
      float raw;
      for (auto& sf : _scalarFields) {
        ContinuousBinding& c = cont(sf.target);
        if (!c.sensor.length() || !hub->getValueByName(c.sensor.c_str(), raw)) continue;
        uint8_t mapped = mapContinuous(raw, c.inMin, c.inMax, c.outMin, c.outMax);
        if (c.lastApplied >= 0 && (uint8_t)c.lastApplied == mapped) continue;
        seg.*(sf.field) = mapped;
        c.lastApplied = mapped;
        changed = true;
      }

      ContinuousBinding& c3 = cont(ContinuousTarget::Custom3);
      if (c3.sensor.length() && hub->getValueByName(c3.sensor.c_str(), raw)) {
        uint8_t mapped = mapContinuous(raw, c3.inMin, c3.inMax, c3.outMin, c3.outMax);
        if (mapped > 31) mapped = 31; // bitfield safety net - see comment above
        if (c3.lastApplied < 0 || (uint8_t)c3.lastApplied != mapped) {
          seg.custom3 = mapped;
          c3.lastApplied = mapped;
          changed = true;
        }
      }
      return changed;
    }

    // Composes all three channels into one seg.setColor() call rather than
    // three independent read-modify-writes, and deadbands against the
    // segment's *current live* color (not our own last-written cache, the
    // way the scalar fields above do) - a manual color change made via the
    // UI in between our ticks should be respected until a bound channel's
    // mapped value has actually moved away from it, not silently reverted
    // to a stale cached value.
    bool applyColor(Segment& seg) {
      ContinuousBinding& rB = cont(ContinuousTarget::Red);
      ContinuousBinding& gB = cont(ContinuousTarget::Green);
      ContinuousBinding& bB = cont(ContinuousTarget::Blue);
      if (!rB.sensor.length() && !gB.sensor.length() && !bB.sensor.length()) return false;

      uint32_t cur = seg.colors[0];
      uint8_t r = R(cur), g = G(cur), b = B(cur), w = W(cur);
      bool changed = false;
      float raw;

      if (rB.sensor.length() && hub->getValueByName(rB.sensor.c_str(), raw)) {
        uint8_t mapped = mapContinuous(raw, rB.inMin, rB.inMax, rB.outMin, rB.outMax);
        if (abs((int)mapped - (int)r) >= 2) { r = mapped; changed = true; } // deadband: sensor noise shouldn't restart a fade every tick
      }
      if (gB.sensor.length() && hub->getValueByName(gB.sensor.c_str(), raw)) {
        uint8_t mapped = mapContinuous(raw, gB.inMin, gB.inMax, gB.outMin, gB.outMax);
        if (abs((int)mapped - (int)g) >= 2) { g = mapped; changed = true; }
      }
      if (bB.sensor.length() && hub->getValueByName(bB.sensor.c_str(), raw)) {
        uint8_t mapped = mapContinuous(raw, bB.inMin, bB.inMax, bB.outMin, bB.outMax);
        if (abs((int)mapped - (int)b) >= 2) { b = mapped; changed = true; }
      }

      if (!changed) return false;
      seg.setColor(0, RGBW32(r, g, b, w));
      return true;
    }

    bool applyBrightness() {
      ContinuousBinding& c = cont(ContinuousTarget::Brightness);
      if (!c.sensor.length()) return false;
      float raw;
      if (!hub->getValueByName(c.sensor.c_str(), raw)) return false;
      uint8_t mapped = mapContinuous(raw, c.inMin, c.inMax, c.outMin, c.outMax);
      if (c.lastApplied >= 0 && (uint8_t)c.lastApplied == mapped) return false;
      bri = mapped;
      c.lastApplied = mapped;
      return true;
    }

    // Returns true (and sets outState) if this threshold's checkbox should
    // change. Doesn't touch the Segment directly - check1-3 are `bool : 1`
    // bitfields, and C++ can't bind a reference to one, so the caller
    // assigns seg.checkN = outState itself.
    bool computeThreshold(ThresholdBinding& t, bool& outState) {
      if (!t.sensor.length()) return false;
      bool state;
      if (!hub->getValueBinaryByName(t.sensor.c_str(), state)) {
        // Not an already-binary sensor - fall back to a numeric reading
        // plus a Schmitt-trigger comparison so the checkbox doesn't
        // flicker right at the threshold.
        float raw;
        if (!hub->getValueByName(t.sensor.c_str(), raw)) return false;
        if (raw > t.threshold + t.hysteresis) state = true;
        else if (raw < t.threshold - t.hysteresis) state = false;
        else if (t.hasApplied) state = t.lastApplied; // inside the deadband - keep previous
        else state = false; // no previous state yet
      }
      if (t.hasApplied && t.lastApplied == state) return false;
      t.lastApplied = state;
      t.hasApplied = true;
      outState = state;
      return true;
    }

    bool applyThresholds(Segment& seg) {
      bool changed = false;
      bool v;
      if (computeThreshold(thresh(ThresholdTarget::Check1), v)) { seg.check1 = v; changed = true; }
      if (computeThreshold(thresh(ThresholdTarget::Check2), v)) { seg.check2 = v; changed = true; }
      if (computeThreshold(thresh(ThresholdTarget::Check3), v)) { seg.check3 = v; changed = true; }
      return changed;
    }

    void addSensorDropdown(Print& settingsScript, const char* jsonKey) {
      settingsScript.printf_P(PSTR("dd=addDropdown('SensorFxBinder','%s:sensor');addOption(dd,'None','');"), jsonKey);
      if (!hub) return;
      uint8_t n = hub->getSensorCount();
      for (uint8_t i = 0; i < n; i++) {
        const char* name = hub->getSensorNameAt(i);
        if (name) settingsScript.printf_P(PSTR("addOption(dd,'%s','%s');"), name, name);
      }
    }

  public:
    void setup() override {}

    void loop() override {
      if (!enabled) return;
      if (!hub) hub = getSensorHub(); // Sensor Hub usermod may finish init after us
      if (!hub) return;

      unsigned long now = millis();
      if (now - lastUpdate < (unsigned long)updateIntervalMs) return;
      lastUpdate = now;

      bool changed = applyBrightness();

      // getSegment() silently falls back to the main segment on an
      // out-of-range id (WLED core behavior) - guard explicitly instead of
      // relying on that, and skip a segment that's mid-reconfiguration.
      if (segmentId < strip.getSegmentsNum()) {
        Segment& seg = strip.getSegment(segmentId);
        if (seg.isActive()) {
          changed |= applySegmentScalars(seg);
          changed |= applyColor(seg);
          changed |= applyThresholds(seg);
        }
      }

      if (changed) {
        stateChanged = true;
        stateUpdated(CALL_MODE_DIRECT_CHANGE);
      }
    }

    // Mirrors addToConfig()/readFromConfig() below, but under WLED *state*
    // (serializeState()/deserializeState(), wled00/json.cpp) instead of
    // cfg.json - this is what makes bindings preset-capturable: presets are
    // just saved/restored state snapshots (wled00/presets.cpp), and state
    // is exactly what these two hooks expose. A preset saved before this
    // usermod had a "SensorFxBinder" key, or one that just never touched
    // it, has no such key - readFromJsonState() below leaves every current
    // binding alone in that case, so old presets stay harmless.
    void addToJsonState(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[FPSTR(_segmentId)] = segmentId;
      top[FPSTR(_updateInterval)] = updateIntervalMs;
      for (auto& c : continuous) {
        JsonObject o = top.createNestedObject(c.jsonKey);
        o[F("sensor")] = c.sensor;
        o[F("inMin")] = c.inMin;
        o[F("inMax")] = c.inMax;
        o[F("outMin")] = c.outMin;
        o[F("outMax")] = c.outMax;
      }
      for (auto& t : thresholds) {
        JsonObject o = top.createNestedObject(t.jsonKey);
        o[F("sensor")] = t.sensor;
        o[F("threshold")] = t.threshold;
        o[F("hysteresis")] = t.hysteresis;
      }
    }

    void readFromJsonState(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return; // this state update/preset doesn't touch bindings - leave everything as-is
      getJsonValue(top[FPSTR(_enabled)], enabled);
      getJsonValue(top[FPSTR(_segmentId)], segmentId);
      getJsonValue(top[FPSTR(_updateInterval)], updateIntervalMs);
      for (auto& c : continuous) {
        JsonObject o = top[c.jsonKey];
        if (o.isNull()) continue; // this particular target wasn't included - leave it bound as it was
        getJsonValue(o[F("sensor")], c.sensor);
        getJsonValue(o[F("inMin")], c.inMin);
        getJsonValue(o[F("inMax")], c.inMax);
        getJsonValue(o[F("outMin")], c.outMin);
        getJsonValue(o[F("outMax")], c.outMax);
      }
      for (auto& t : thresholds) {
        JsonObject o = top[t.jsonKey];
        if (o.isNull()) continue;
        getJsonValue(o[F("sensor")], t.sensor);
        getJsonValue(o[F("threshold")], t.threshold);
        getJsonValue(o[F("hysteresis")], t.hysteresis);
      }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[FPSTR(_segmentId)] = segmentId;
      top[FPSTR(_updateInterval)] = updateIntervalMs;
      for (auto& c : continuous) {
        JsonObject o = top.createNestedObject(c.jsonKey);
        o[F("sensor")] = c.sensor;
        o[F("inMin")] = c.inMin;
        o[F("inMax")] = c.inMax;
        o[F("outMin")] = c.outMin;
        o[F("outMax")] = c.outMax;
      }
      for (auto& t : thresholds) {
        JsonObject o = top.createNestedObject(t.jsonKey);
        o[F("sensor")] = t.sensor;
        o[F("threshold")] = t.threshold;
        o[F("hysteresis")] = t.hysteresis;
      }
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top[FPSTR(_segmentId)], segmentId);
      configComplete &= getJsonValue(top[FPSTR(_updateInterval)], updateIntervalMs);
      for (auto& c : continuous) {
        JsonObject o = top[c.jsonKey];
        configComplete &= getJsonValue(o[F("sensor")], c.sensor);
        configComplete &= getJsonValue(o[F("inMin")], c.inMin);
        configComplete &= getJsonValue(o[F("inMax")], c.inMax);
        configComplete &= getJsonValue(o[F("outMin")], c.outMin);
        configComplete &= getJsonValue(o[F("outMax")], c.outMax);
      }
      for (auto& t : thresholds) {
        JsonObject o = top[t.jsonKey];
        configComplete &= getJsonValue(o[F("sensor")], t.sensor);
        configComplete &= getJsonValue(o[F("threshold")], t.threshold);
        configComplete &= getJsonValue(o[F("hysteresis")], t.hysteresis);
      }
      return configComplete;
    }

    void appendConfigData(Print& settingsScript) override {
      settingsScript.print(F("addInfo('SensorFxBinder:segmentId',1,'which segment the effect-parameter/color targets below apply to - brightness is global, not segment-scoped');"));
      settingsScript.print(F("addInfo('SensorFxBinder:updateInterval',1,'milliseconds between binding updates');"));

      if (!hub) hub = getSensorHub();
      for (auto& c : continuous) {
        addSensorDropdown(settingsScript, c.jsonKey);
        settingsScript.printf_P(PSTR("addInfo('SensorFxBinder:%s:inMin',1,'sensor value mapped from (low end)');"), c.jsonKey);
        settingsScript.printf_P(PSTR("addInfo('SensorFxBinder:%s:inMax',1,'sensor value mapped from (high end)');"), c.jsonKey);
        settingsScript.printf_P(PSTR("addInfo('SensorFxBinder:%s:outMin',1,'output value at inMin (outMin > outMax inverts the mapping)');"), c.jsonKey);
        settingsScript.printf_P(PSTR("addInfo('SensorFxBinder:%s:outMax',1,'output value at inMax');"), c.jsonKey);
      }
      for (auto& t : thresholds) {
        addSensorDropdown(settingsScript, t.jsonKey);
        settingsScript.printf_P(PSTR("addInfo('SensorFxBinder:%s:threshold',1,'numeric sensors: on above this value - ignored for sensors that are already on/off (e.g. Motion/Contact)');"), t.jsonKey);
        settingsScript.printf_P(PSTR("addInfo('SensorFxBinder:%s:hysteresis',1,'deadband around the threshold to avoid flicker');"), t.jsonKey);
      }
    }
};

constexpr SensorFxBinderUsermod::ScalarField SensorFxBinderUsermod::_scalarFields[4];

const char SensorFxBinderUsermod::_name[]           PROGMEM = "SensorFxBinder";
const char SensorFxBinderUsermod::_enabled[]        PROGMEM = "enabled";
const char SensorFxBinderUsermod::_segmentId[]      PROGMEM = "segmentId";
const char SensorFxBinderUsermod::_updateInterval[] PROGMEM = "updateInterval";

static SensorFxBinderUsermod sensor_fx_binder;
REGISTER_USERMOD(sensor_fx_binder);
