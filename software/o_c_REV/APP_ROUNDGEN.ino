// APP_ROUNDGEN.ino — "Roundgen": a self-playing, self-mutating canon
// (round) generator for Ornament & Crime.
//
// The melodic engine lives in roundgen/roundgen_core.{hpp,cpp} (the C++
// port of the roundgen Python library). This applet is a thin shell:
//   - generates a random melody and conforms it into a valid round
//   - outputs the melody as pitch CV + gate (mono) or two voices
//   - mutates the shared melody at every cycle boundary (still a valid
//     round, guaranteed by the solver)
//   - rests drift toward the rest-density setting (0 drains, 1 fills)
//
// Panel map (v16):
//   TR 1  clock           (external clock; rising edge = one beat)
//   TR 2  reset           (restart the round at beat 0)
//   TR 3  freeze          (gate high = pause mutation)
//   TR 4  reroll          (new round, same settings)
//   Out 1/2 channel A: pitch + gate of the voices assigned to A
//   Out 3/4 channel B: pitch + gate of the voices assigned to B
//   (Voice -> Chan: 0 = A, 1 = B, 2 = off; a channel with several
//   voices arpeggiates them, 1/kv beats each at their own canon
//   offset)
//
// Buttons: L = rerandomize (new round), R = menu select / edit (press
// on the Rerandomize row to fire it). D toggles the mutation freeze
// (v25 — TR3 held does the same; both show FRZ in the title bar and on
// the monitor). U is unused since v22 (it stepped the seed).
// Encoders: L = mutation rate (live), R = menu scroll / edit.
//
// Menu: Scale, Key, Voices (2..8), Melody len (8..24), Canon off
// (0 = auto; the row always shows the resolved offset that is actually
// playing), Mutation (0..5.0), Rest dens, Dissonance, Clock (int/ext),
// BPM (internal clock only: the row is not drawn with Clock = ext),
// Gate/Trig, Gate len (gate mode only: the gate is high for this % of
// the note) — then the per-voice sub-menu: Voice (the selector for the
// next two rows, no dash), - Oct (the voice's canonical octave number,
// 1..7), - Chan (A/B/off) — then the Rerandomize action row (new round,
// same settings) — press the R encoder on it to fire. Default voice
// routing: V1 -> A, V2..V8 off.
// v22 dropped the Seed, Transpose and Reset rows: the melody stream is
// continuous from boot (Rerandomize walks it, nothing to dial in),
// octave shifts are per voice (- Oct), and TR2 is the restart input.
// v23 adds the gate length (a percentage of the note, gate mode only;
// with an external clock the percentage is taken from the measured TR1
// period — see ROUNDGEN_CLOCK_SILENT_MS below and the README).
// v24 makes the per-voice octave a canonical octave number: 4 = the
// octave the melody itself is written in, 1..7 = three octaves either
// side of it (the old -2..+2 shift dialed the same idea as an offset
// from 4). The pitch output is referenced so C4 = 0V — that is what
// lets the whole 1..7 range fit the DAC's 10-octave pitch window (see
// ROUNDGEN_PITCH_ZERO_MIDI), and the screensaver now shows each voice's
// sounded note (the melody note transposed to its octave).
// v25 adds the Down-button freeze: a press latches the mutation freeze
// that TR3 applies while held (the effective state is either — see
// frozen()), and the state is shown as FRZ in the title bar and on
// voice 1's monitor row.

// Firmware revision, shown in the menu title bar (right column): check
// for "v25" after flashing to confirm the build is installed. (The app
// list carries the bare name "Roundgen" — no version to lag this
// string; the title bar is the one this build controls.)
#define ROUNDGEN_VERSION "v25"

// Gate mode with an external clock: no accepted TR1 pulse for this long
// (~1 kHz sections, i.e. ~4 s = 15 BPM at 1 PPQN) means the tempo is
// unknown — the channels output no gate at all rather than holding the
// last note. Cleared by the next pulse.
#define ROUNDGEN_CLOCK_SILENT_MS 4000

// The melody is generated across this octave (the make_scale calls in
// generate_new_round and the mutation loop), so it is the octave every
// voice sits in by default; a voice's octave setting is a canonical
// octave number (C4 = middle C, as the monitor prints note names), 1..7
// = three octaves either side of the melody's own.
#define ROUNDGEN_MELODY_OCTAVE 4

// Pitch output reference. The DAC's pitch path (kOctaveZero = 3) covers
// 10 octaves (-3V..+7V); the app refers its pitch to C4 = 0V, which is
// what fits the whole 1..7 octave range: octave 1 bottoms out at C1 =
// -3V and octave 7 (key B) tops out at +5.8V. In midi terms the pitch
// value written to the DAC is (sounded note - C4).
#define ROUNDGEN_PITCH_ZERO_MIDI (12 * (ROUNDGEN_MELODY_OCTAVE + 1))

#include "OC_digital_inputs.h"
#include "src/roundgen/roundgen_core.hpp"

enum ROUNDGEN_SETTING {
  ROUNDGEN_SETTING_SCALE,
  ROUNDGEN_SETTING_KEY,
  ROUNDGEN_SETTING_VOICES,
  ROUNDGEN_SETTING_LENGTH,        // melody length (notes per round)
  ROUNDGEN_SETTING_OFFSET,        // canon entry offset, global
  ROUNDGEN_SETTING_MUTATION,
  ROUNDGEN_SETTING_REST_DENSITY,
  ROUNDGEN_SETTING_DISSONANCE,
  ROUNDGEN_SETTING_CLOCK_SOURCE,  // int/ext; owns the BPM row below it
  ROUNDGEN_SETTING_BPM,           // drawn only under the internal clock
  ROUNDGEN_SETTING_GATE_TRIG,
  ROUNDGEN_SETTING_GATE_LEN,  // gate duty cycle, % of the note (gate mode)
  // per-voice sub-menu (dash-prefixed rows)
  ROUNDGEN_SETTING_VOICE_SEL,  // which voice the Oct/Chan rows edit
  ROUNDGEN_SETTING_OCT_SEL,    // menu row: octave of the selected voice
  ROUNDGEN_SETTING_CHAN_SEL,   // menu row: channel of the selected voice
  // storage-only per-voice rows (never drawn; edited via Voice/Oct/Chan)
  ROUNDGEN_SETTING_V1_OCT,
  ROUNDGEN_SETTING_V2_OCT,
  ROUNDGEN_SETTING_V3_OCT,
  ROUNDGEN_SETTING_V4_OCT,
  ROUNDGEN_SETTING_V5_OCT,
  ROUNDGEN_SETTING_V6_OCT,
  ROUNDGEN_SETTING_V7_OCT,
  ROUNDGEN_SETTING_V8_OCT,
  ROUNDGEN_SETTING_V1_CHAN,
  ROUNDGEN_SETTING_V2_CHAN,
  ROUNDGEN_SETTING_V3_CHAN,
  ROUNDGEN_SETTING_V4_CHAN,
  ROUNDGEN_SETTING_V5_CHAN,
  ROUNDGEN_SETTING_V6_CHAN,
  ROUNDGEN_SETTING_V7_CHAN,
  ROUNDGEN_SETTING_V8_CHAN,
  // Hidden layout marker: its presence changes the stored chunk size,
  // so the framework skips any chunk saved by an older layout instead
  // of silently misreading it (the restore has no version field; a
  // same-size reorder used to restore garbage). Bump its default on
  // every layout change. (Storage widened U16 -> U32 in v24: the octave
  // rows changed meaning, and the size change forces a clean reset —
  // v11 -> v12 did the same with U8 -> U16.)
  ROUNDGEN_SETTING_LAYOUT,
  ROUNDGEN_SETTING_LAST
};

// Menu action rows: rows with no stored value, fired with the R encoder
// (no edit mode). The visible row order is the setting enum order with
// two conditional rows skipped (see roundgen_row_id() below), so a row
// that is not drawn (BPM under an external clock, Gate len in trigger
// mode) can never be landed on or edited. The per-voice storage rows
// and the layout marker sit at higher enum values and are never drawn.
// (v19 pinned the action row directly after the visible settings: it
// used to sit at a fixed cursor row left over from the removed CV1-4
// amt rows, which made the draw loop render the V1..V4 octave storage
// rows as phantom "+octave" entries.)
enum {
  ROUNDGEN_ROW_REROLL = 0x80,  // action row: a new round, same settings
};

const char * const roundgen_scale_names[10] = {
  "major", "natural minor", "dorian", "phrygian", "lydian",
  "mixolydian", "locrian", "penta major", "penta minor", "blues",
};
const char * const roundgen_key_names[12] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};
const char * const roundgen_clock_names[2] = { "int", "ext" };
const char * const roundgen_ch_names[3] = { "A", "B", "off" };
const char * const roundgen_gate_names[2] = { "gate", "trig" };

class RoundgenApp : public settings::SettingsBase<RoundgenApp, ROUNDGEN_SETTING_LAST> {
public:
  uint8_t get_scale() const { return values_[ROUNDGEN_SETTING_SCALE]; }
  uint8_t get_key() const { return values_[ROUNDGEN_SETTING_KEY]; }
  uint8_t get_voices() const {
    // clamp: a stale/misaligned EEPROM chunk could hold anything here
    uint8_t v = values_[ROUNDGEN_SETTING_VOICES];
    return (v < 2) ? 2 : (v > 8 ? 8 : v);
  }
  uint8_t get_offset_setting() const { return values_[ROUNDGEN_SETTING_OFFSET]; }
  uint8_t get_length() const { return values_[ROUNDGEN_SETTING_LENGTH]; }
  uint8_t get_mutation_setting() const { return values_[ROUNDGEN_SETTING_MUTATION]; }
  uint8_t get_rest_setting() const { return values_[ROUNDGEN_SETTING_REST_DENSITY]; }
  uint8_t get_dissonance_setting() const { return values_[ROUNDGEN_SETTING_DISSONANCE]; }
  uint8_t get_bpm() const { return values_[ROUNDGEN_SETTING_BPM]; }
  uint8_t get_clock_source() const {
    // clamp: a stale/misaligned EEPROM chunk could hold anything here;
    // a garbage value silently switches to the external clock and the
    // music never advances on the internal one.
    uint8_t c = values_[ROUNDGEN_SETTING_CLOCK_SOURCE];
    return c > 1 ? 0 : c;
  }
  // the voice the Oct/Chan menu rows edit (1-based, clamped to Voices)
  int get_voice_sel() const {
    int v = values_[ROUNDGEN_SETTING_VOICE_SEL];
    int voices = get_voices();
    if (v < 1) v = 1;
    if (v > voices) v = voices;
    return v;
  }
  uint8_t get_gate_trig() const { return values_[ROUNDGEN_SETTING_GATE_TRIG]; }
  // gate length: 0 = the note is never on, 100 = never off (hold), 75 =
  // the gate is high for 3/4 of the note. Only gate mode uses it (the
  // row is hidden with Gate/Trig = trig).
  uint8_t get_gate_len() const { return values_[ROUNDGEN_SETTING_GATE_LEN]; }
  // per-voice octave: the canonical octave number (1..7) the voice
  // plays the melody in, returned as the transposition from the melody's
  // own octave (0 = unison, -3..+3). Clamped: a stale EEPROM could hold
  // anything in these slots.
  int get_voice_oct(int v) const {
    if (v < 0 || v > 7) return 0;
    int raw = values_[ROUNDGEN_SETTING_V1_OCT + v];
    if (raw < 1) raw = 1;
    if (raw > 7) raw = 7;
    return raw - ROUNDGEN_MELODY_OCTAVE;
  }
  // per-voice output channel: 0 = A/B, 1 = C/D, 2 = off
  int get_voice_chan(int v) const {
    if (v < 0 || v > 7) return 0;
    int c = values_[ROUNDGEN_SETTING_V1_CHAN + v];
    return (c < 0 || c > 2) ? 0 : c;
  }
  // the canonical octave number of a voice's copy: 1..7, the Oct row's
  // value (the row shows this, not the transposition)
  int get_voice_oct_setting(int v) const {
    return get_voice_oct(v) + ROUNDGEN_MELODY_OCTAVE;
  }

  // the effective mutation freeze: TR3 held, or the Down-button latch
  bool frozen() const { return freeze_ || freeze_toggle_; }

  void Init();
  // A new round, same settings: drawn from the continuous RNG stream,
  // so every call sounds a different melody (menu Rerandomize row, TR4,
  // L). Also the boot call — the stream starts from a fixed point, so
  // the power-on melody is the same one every time until you reroll.
  void generate_new_round();

  // Playback state, written by the ISR, read by loop().
  volatile float pos_;          // beat position within the current cycle
  volatile int cycle_;          // completed cycles since (re)start
  volatile uint32_t clocked_accum_;  // consumed ONLY by the ~1 kHz ISR
                                    // section (beat advance, TR2 reset)
  volatile bool reroll_pending_;    // TR4 edge latched for loop()
  volatile bool freeze_;            // TR3 held (written by the ISR)
  // Down-button freeze latch (v25): a button press toggles it, it is
  // not stored in EEPROM. The effective freeze is freeze_ (TR3, held)
  // OR this latch — see frozen().
  bool freeze_toggle_;
  // Set by a reset (TR2, or boot): the ~1 kHz ISR section actively
  // reinitializes the note-tracker state (drops any held gate, clears
  // the boundary/pitch caches) so the round restarts cleanly at beat 0
  // instead of continuing with the interrupted note's memory. (The menu
  // Reset row is gone in v22 — TR2 is the restart input.)
  volatile bool note_reset_pending_;
  // Set by loop() after solving a mutation into slot_a_: the melody is
  // swapped at the next fresh cycle wrap (never mid-cycle — publishing
  // between clock pulses rewinds pos_ and swallows beats). Cleared by
  // generate_new_round(): a deliberate reroll/regen supersedes it.
  bool pending_publish_;

  // Count of gate opens per output channel, bumped by the ISR whenever a
  // note start actually opens a gate (gate mode, trigger pulses, a
  // re-articulated repeat — but not when the gate is held from the
  // previous note, when Gate len is 0, or while an external clock is
  // silent). The screensaver's '*' flash reads it, so the flash shows
  // the engine's own gate events instead of guessing at them.
  volatile uint8_t gate_opens_[2];

  // The round being played (published by swap). The ISR only ever reads
  // *playing_; loop() only mutates into non-playing buffers, so there is
  // no data race: pointer swaps are atomic on the M4.
  const roundgen::Round* playing() const { return playing_; }

  void publish(roundgen::Round& next);

  // Cached melody facts for the menu / quick access.
  roundgen::Round rounds_[3];
  const roundgen::Round* playing_;
  roundgen::Round* slot_a_;
  roundgen::Round* slot_b_;
  roundgen::Rng rng_;
};

void RoundgenApp::Init() {
  InitDefaults();
  playing_ = &rounds_[0];
  slot_a_ = &rounds_[1];
  slot_b_ = &rounds_[2];
  pos_ = 0.0f;
  cycle_ = 0;
  clocked_accum_ = 0;
  reroll_pending_ = false;
  freeze_ = false;
  freeze_toggle_ = false;
  note_reset_pending_ = false;
  pending_publish_ = false;
  gate_opens_[0] = 0;
  gate_opens_[1] = 0;
  rng_ = roundgen::Rng(1);
  generate_new_round();
}

void RoundgenApp::publish(roundgen::Round& next) {
  pos_ = 0.0f;             // restart the cycle at beat 0 BEFORE the swap,
  playing_ = &next;        // so the ISR never sees the new round at a
                           // stale mid-cycle position (gate/pitch blip)
  roundgen::Round* tmp = slot_a_;
  slot_a_ = slot_b_;
  slot_b_ = tmp;
  // cycle_ is deliberately NOT reset here: it counts completed cycles
  // and loop()'s mutation trigger (static last_cycle) tracks it. Zeroing
  // it after a publish makes the very next wrap look like a TR2 reset
  // (cycle_ < last_cycle), which clears pending_publish_ and discards
  // the mutation that was just solved — the melody then only changes
  // every other wrap and half the conform solves are wasted. Genuine
  // restarts (TR2, boot) still zero it, and loop()
  // detects those via the backward jump.
}

void RoundgenApp::generate_new_round() {
  pending_publish_ = false;  // this round supersedes any pending mutation
  int scale[roundgen::RG_MAX_SCALE];
  int scale_len =
      roundgen::make_scale((int)get_scale(), get_key(),
                           ROUNDGEN_MELODY_OCTAVE, 2, scale);
  int voices = get_voices();
  int length = get_length();
  float offset = (float)get_offset_setting();
  if (offset < 1.0f) offset = -1.0f;  // auto: solver picks the best offset
  float rests = get_rest_setting() / 100.0f;
  // write into a non-playing slot, then publish the pointer
  roundgen::Round& next = *slot_a_;
  // A fixed canon offset is honored by the core (conform solves at
  // exactly that offset; values that cannot land inside the cycle are
  // clamped to half of it there). Conforming at a fixed offset can
  // still fail for a given melody — retry a few times (each attempt
  // draws fresh notes from the same continuous RNG stream, so it is a
  // different melody) before giving up (leaves the previous round).
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (roundgen::generate_round(length, voices, offset, scale, scale_len, 2,
                                 rests, NULL, rng_, &next)) {
      publish(next);
      return;
    }
  }
}

SETTINGS_DECLARE(RoundgenApp, ROUNDGEN_SETTING_LAST) {
  { 0, 0, 9, "Scale", roundgen_scale_names, settings::STORAGE_TYPE_U8 },
  { 0, 0, 11, "Key", roundgen_key_names, settings::STORAGE_TYPE_U8 },
  { 3, 2, 8, "Voices", NULL, settings::STORAGE_TYPE_U8 },
  { 12, 8, 24, "Melody len", NULL, settings::STORAGE_TYPE_U8 },
  { 2, 0, 16, "Canon off", NULL, settings::STORAGE_TYPE_U8 },
  { 10, 0, 50, "Mutation", NULL, settings::STORAGE_TYPE_U8 },
  { 30, 0, 100, "Rest dens", NULL, settings::STORAGE_TYPE_U8 },
  { 0, 0, 100, "Dissonance", NULL, settings::STORAGE_TYPE_U8 },
  { 0, 0, 1, "Clock", roundgen_clock_names, settings::STORAGE_TYPE_U8 },
  { 120, 40, 300, "BPM", NULL, settings::STORAGE_TYPE_U8 },
  { 0, 0, 1, "Gate/Trig", roundgen_gate_names, settings::STORAGE_TYPE_U8 },
  { 50, 0, 100, "Gate len", NULL, settings::STORAGE_TYPE_U8 },
  // per-voice sub-menu (dash prefix marks the voice-scoped rows; the
  // Voice selector itself is the sub-menu header and stays un-dashed)
  { 1, 1, 8, "Voice", NULL, settings::STORAGE_TYPE_U8 },
  // - Oct is a canonical octave number: 4 = the octave the melody is
  // written in, 1..7 = three octaves either side (the widest the DAC
  // pitch window holds; see ROUNDGEN_PITCH_ZERO_MIDI)
  { 4, 1, 7, "- Oct", NULL, settings::STORAGE_TYPE_U8 },
  { 0, 0, 2, "- Chan", roundgen_ch_names, settings::STORAGE_TYPE_U8 },
  // storage-only per-voice rows (edited via - Voice + - Oct / - Chan;
  // never drawn, so no names — saves ~130 bytes of flash)
  { 4, 1, 7, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 4, 1, 7, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 4, 1, 7, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 4, 1, 7, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 4, 1, 7, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 4, 1, 7, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 4, 1, 7, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 4, 1, 7, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 0, 0, 2, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 2, 0, 2, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 2, 0, 2, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 2, 0, 2, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 2, 0, 2, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 2, 0, 2, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 2, 0, 2, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 2, 0, 2, NULL, NULL, settings::STORAGE_TYPE_U8 },
  { 7, 0, 65535, "Layout", NULL, settings::STORAGE_TYPE_U32 },
};

RoundgenApp roundgen_app;
struct {
  menu::ScreenCursor<menu::kScreenLines> cursor;
} roundgen_app_state;

// ---------------------------------------------------------------------------
// Menu layout. The cursor walks visible ROWS, and a row maps to whatever
// it edits: a setting index (values_ / value_attr) or the Rerandomize
// action row. The rows are the setting enum order —
//   Scale Key Voices Melody len Canon off Mutation Rest dens Dissonance
//   Clock BPM Gate/Trig Gate len Voice - Oct - Chan  [Rerandomize]
// — minus the two conditional rows: BPM belongs to the internal clock
// (with Clock = ext it is not the tempo source) and Gate len to gate
// mode (a trigger pulse has no length to set). Hiding one shifts every
// row below it up, and every row -> id lookup goes through
// roundgen_row_id(), so the cursor can never land on (or edit) a row
// that is not drawn. menu_map_test.cpp pins this order for all four
// visibility combinations.
// ---------------------------------------------------------------------------

// rows when nothing is hidden: Scale..- Chan (15 settings) + the action
#define ROUNDGEN_MENU_ROWS_MAX (ROUNDGEN_SETTING_CHAN_SEL + 2)

// number of rows the cursor may visit
static int roundgen_menu_rows() {
  int n = ROUNDGEN_MENU_ROWS_MAX;
  if (roundgen_app.get_clock_source() != 0) --n;  // no BPM row
  if (roundgen_app.get_gate_trig() != 0) --n;     // no Gate len row
  return n;
}

// visible row -> menu id, -1 past the end of the list
static int roundgen_row_id(int row) {
  if (row < 0) return -1;
  int id = row;
  if (id >= ROUNDGEN_SETTING_BPM && roundgen_app.get_clock_source() != 0)
    ++id;  // skip the hidden BPM row
  if (id >= ROUNDGEN_SETTING_GATE_LEN && roundgen_app.get_gate_trig() != 0)
    ++id;  // skip the hidden Gate len row
  if (id > ROUNDGEN_SETTING_CHAN_SEL) {
    // the action row is the next one; anything past it does not exist
    return id == ROUNDGEN_SETTING_CHAN_SEL + 1 ? (int)ROUNDGEN_ROW_REROLL : -1;
  }
  return id;
}

// Screensaver monitor state. The '*' note-start flash is event-based: a
// row flashes when its voice's note actually starts sounding on its
// output channel — the channel's gate opens (a rest becoming a note, or
// a new note instance while the gate was released by the gate length),
// or its pitch key changes, where the key is midi+12*voice-octave, the
// part of the CV that is per-voice — and the event is attributed to the
// voice owning the channel slot (at most one voice per channel at a
// time).
// The tracker mirrors the ISR's channel state: it persists across
// mutation publishes (like the engine's gate/pitch memory, which only
// a real reset clears) and is re-armed when the screensaver appears.
static int16_t monitor_last_cv[2] = { -1, -1 };  // pitch key, or -1 =
                                               // rest / gate closed
// The channel's gate-open count as of the last redraw: with a gate
// length below 100 the engine re-articulates every note (a repeated
// pitch included), and the count is the engine's own word for it.
static uint8_t monitor_last_opens[2] = { 0, 0 };
static uint8_t monitor_flash[8];  // countdown, ~1 ms per decrement

// ---------------------------------------------------------------------------
// ISR: 16.666 kHz. Reads inputs, advances the beat and drives the DACs at
// ~1 kHz. No solver work in here.
// ---------------------------------------------------------------------------
void FASTRUN ROUNDGEN_isr() {
  // accumulate clocked edges; TR4 is latched into its own flag for
  // loop() (loop() must never read/clear clocked_accum_ — it races the
  // ~1 kHz section below and swallows external clock pulses)
  roundgen_app.clocked_accum_ |= OC::DigitalInputs::clocked();
  if (roundgen_app.clocked_accum_ & OC::DIGITAL_INPUT_4_MASK)
    roundgen_app.reroll_pending_ = true;
  roundgen_app.freeze_ = OC::DigitalInputs::read_immediate<OC::DIGITAL_INPUT_3>();

  // Smoothing of the CV inputs removed (v16): the CV modulation was
  // not useful and the per-tick ADC reads cost core-ISR time. Only the
  // trigger inputs (TR1 clock, TR2 reset, TR3 freeze, TR4 reroll) are
  // read now; the internal clock tempo is set by the BPM menu row.

  // ~1 kHz section: beat + note tracking + DAC output
  static uint8_t tick;
  if (++tick < 16) return;
  tick = 0;

  const roundgen::Round* round = roundgen_app.playing();
  // Cache the melody facts (n / total / offset) together with the round
  // pointer: the round only changes at publish/reroll, so the per-tick
  // 1 kHz section skips the O(n) total_beats() walk (keeps the core ISR
  // inside its 60 us budget). The same check invalidates the per-voice
  // boundary cache in the note section below — without it, the cached
  // next_change belongs to the OLD melody after a publish and the voice
  // holds the old pitch for up to a note (the boundary glitch, worst in
  // duo/arp modes).
  static const roundgen::Round* last_round = nullptr;
  static int cached_n = 0;
  static float cached_total = 0.0f, cached_offset = 0.0f;
  static float last_t[2] = { -1.0f, -1.0f };
  static float next_change[2] = { -1.0f, -1.0f };
  static int last_idx[2] = { -1, -1 };
  if (round != last_round) {
    last_round = round;
    cached_n = round->n;
    cached_total = round->total_beats();
    cached_offset = round->offset_beats;
    for (int v = 0; v < 2; ++v) {
      last_t[v] = -1.0f;
      next_change[v] = -1.0f;
      last_idx[v] = -1;
    }
  }
  const int n = cached_n;
  if (n == 0) return;
  const float total = cached_total;
  const float offset = cached_offset;
  uint32_t clocked = roundgen_app.clocked_accum_;
  roundgen_app.clocked_accum_ = 0;

  // ~1 kHz section counter: wall-clock timebase for the TR1 debounce.
  // (It must advance every section, pulse or not — a counter that only
  // advanced on pulses measures "pulses since the last accepted edge"
  // and rejects 2 of every 3 pulses of ANY clock, however slow.)
  static uint16_t section_sec = 0;
  ++section_sec;

  // Section of the last accepted TR1 edge. Seeded one silence window
  // into the past so a boot with Clock = ext and no clock attached
  // starts out silent (no gate) until the first pulse arrives.
  static uint16_t tr1_last = (uint16_t)-(ROUNDGEN_CLOCK_SILENT_MS + 1);

  // Wall-clock length of one beat, for the gate-length math: internal
  // clock = 60000/BPM, external = the measured TR1 period (an accepted
  // edge closes the interval). Under an external clock with no pulse
  // for ROUNDGEN_CLOCK_SILENT_MS the tempo is unknown and the channels
  // output no gate at all ("the note is never on") instead of holding
  // the last note forever.
  static uint16_t beat_ms = 500;
  const bool clock_silent =
      roundgen_app.get_clock_source() != 0 &&
      (uint16_t)(section_sec - tr1_last) > ROUNDGEN_CLOCK_SILENT_MS;

  // advance the beat
  if (roundgen_app.get_clock_source() == 0) {
    // internal clock (BPM menu row). Integer math: the BPM row is a
    // byte, and the gate-length deadline only needs the beat in ms
    // (60000/BPM) — a float divide here cost more ISR code than the
    // whole deadline loop.
    uint32_t bpm = roundgen_app.get_bpm();
    if (bpm < 20) bpm = 20;
    if (bpm > 400) bpm = 400;
    beat_ms = (uint16_t)(60000u / bpm);
    roundgen_app.pos_ += static_cast<float>(bpm) / 60000.0f;
  } else if (clocked & OC::DIGITAL_INPUT_1_MASK) {
    // Debounce TR1 (~3 ms window, in ~1 kHz sections). A marginal input
    // edge can ring into a second falling edge a few ms later; each
    // extra edge advances pos_ by one beat — a skipped note AND a pitch
    // change mid-step (both observed). Real clocks are >= 1 ms apart,
    // so a 3 ms window only rejects spurious retriggers.
    if ((uint16_t)(section_sec - tr1_last) >= 3) {
      // The accepted edge closes the interval: its length becomes the
      // beat estimate for the notes that start from here (steady
      // clocks are exact; a tempo change is tracked with one beat of
      // lag). Averaged with the previous estimate so a swung or jittery
      // clock — or one edge missed by the debounce — does not swing the
      // gate lengths, and clamped to 5..4000 ms (12000..15 BPM), which
      // is wider than any musical 1 PPQN clock.
      const uint16_t period = (uint16_t)(section_sec - tr1_last);
      if (period >= 5 && period <= 4000)
        beat_ms = (uint16_t)(((uint32_t)beat_ms + period) >> 1);
      roundgen_app.pos_ += 1.0f;  // one clock pulse = one beat
      tr1_last = section_sec;
    }
  }
  if (clocked & OC::DIGITAL_INPUT_2_MASK) {  // reset
    roundgen_app.pos_ = 0.0f;
    roundgen_app.cycle_ = 0;
    roundgen_app.note_reset_pending_ = true;
  }
  while (roundgen_app.pos_ >= total) {
    roundgen_app.pos_ -= total;
    ++roundgen_app.cycle_;
  }

  // note tracking + DAC writes for each output channel.
  // Channel A = outs 1+2 (pitch A + gate B), channel B = outs 3+4
  // (pitch C + gate D). Each channel plays the voices assigned to it
  // (Voice -> Chan): one voice plain, several arpeggiated (1/kv beats
  // each, at each voice's own canon offset).
  {
    // One gate-off deadline per channel, in ~1 kHz sections (0 = no
    // deadline: hold the gate, the v21 legato). Trigger mode sets it to
    // a short pulse, gate mode to the gate length (a percentage of the
    // note); a silent external clock closes the gate outright.
    static uint16_t pulse_until[2] = { 0, 0 };
    static int32_t last_pitch[2] = { -1, -1 };
    static bool gate_on[2] = { false, false };
    // A reset (TR2) actively reinitializes this
    // tracker memory: any held gate is dropped and the per-channel
    // boundary/pitch caches are cleared, so the note at the reset
    // position is re-evaluated and re-articulated from scratch. Without
    // this the tracker keeps the interrupted note's state — a gate it
    // opened before the reset can keep sounding notes the restarted
    // round should not sound, and the reset has no audible downbeat.
    if (roundgen_app.note_reset_pending_) {
      roundgen_app.note_reset_pending_ = false;
      for (int ch = 0; ch < 2; ++ch) {
        const DAC_CHANNEL gate_ch = ch == 0 ? DAC_CHANNEL_B : DAC_CHANNEL_D;
        if (gate_on[ch]) {
          OC::DAC::set(gate_ch, OC::DAC::get_zero_offset(gate_ch));
          gate_on[ch] = false;
        }
        last_t[ch] = -1.0f;
        next_change[ch] = -1.0f;
        last_idx[ch] = -1;
        last_pitch[ch] = -1;
        pulse_until[ch] = 0;
      }
    }
    // channel -> assigned voices, rebuilt every 1 kHz tick (the channel
    // assignments can change at any time; 8 U8 reads are cheap)
    int ch_voices[2][8];
    int ch_n[2] = { 0, 0 };
    for (int v = 0; v < 8; ++v) {
      const int ch = roundgen_app.get_voice_chan(v);
      if (ch < 2) ch_voices[ch][ch_n[ch]++] = v;
    }
    // Gate-off deadlines: the ~8 ms trigger pulse, or the gate-length
    // percentage of the note, whichever mode is selected. (section_sec
    // wraps every ~65 s; the int16 difference is the wrap-safe "deadline
    // passed" test. The two channels sit <= 30 s apart by construction.)
    for (int ch = 0; ch < 2; ++ch) {
      if (gate_on[ch] &&
          (clock_silent ||
           (pulse_until[ch] &&
            (int16_t)(section_sec - pulse_until[ch]) >= 0))) {
        const DAC_CHANNEL gate_ch = ch == 0 ? DAC_CHANNEL_B : DAC_CHANNEL_D;
        OC::DAC::set(gate_ch, OC::DAC::get_zero_offset(gate_ch));
        gate_on[ch] = false;
        pulse_until[ch] = 0;
      }
    }
    for (int ch = 0; ch < 2; ++ch) {
      const int kv = ch_n[ch];
      if (kv == 0) continue;  // no voices on this channel
      // Channel time: a single voice at its own canon position (voice v
      // enters at v*offset); an arpeggio frames itself on the integer
      // beats like the old mono arp.
      float t;
      if (kv == 1) {
        t = roundgen_app.pos_ + ch_voices[ch][0] * offset;
        while (t >= total) t -= total;
      } else {
        t = roundgen_app.pos_;
      }
      // Boundary cache (per channel): the note can only change at
      // note/slot boundaries, so in steady state this costs one float
      // compare per channel per tick. A backward jump (t < last_t) is
      // the cycle wrap — always recompute.
      if (!(t < last_t[ch]) && next_change[ch] >= 0.0f && t < next_change[ch])
        continue;
      last_t[ch] = t;
      int idx;
      int midi;
      int voct;
      if (kv > 1) {
        // arpeggiate the channel's voices: 1/kv beats each, in order,
        // each at its own canon offset
        const int beat = (int)t;
        const float slot = 1.0f / kv;
        int m = (int)((t - beat) / slot);
        if (m >= kv) m = kv - 1;
        const int v = ch_voices[ch][m];
        idx = beat * 16 + m;
        voct = roundgen_app.get_voice_oct(v);
        float pos = beat + m * slot + v * offset;
        while (pos >= total) pos -= total;
        int ni = 0;
        float acc = 0.0f;
        for (int i = 0; i < n; ++i) {
          if (pos < acc + round->notes[i].beats) { ni = i; break; }
          acc += round->notes[i].beats;
        }
        midi = round->notes[ni].midi;
        next_change[ch] = beat + (m + 1) * slot;
      } else {
        // plain voice: its own cyclic position (the round repeats
        // forever, so the note changes at the note's end)
        const int v = ch_voices[ch][0];
        int ni = 0;
        float acc = 0.0f;
        for (int i = 0; i < n; ++i) {
          if (t < acc + round->notes[i].beats) { ni = i; break; }
          acc += round->notes[i].beats;
        }
        idx = ni;
        midi = round->notes[ni].midi;
        voct = roundgen_app.get_voice_oct(v);
        float end = acc + round->notes[ni].beats;
        next_change[ch] = end < total ? end : total;
      }
      if (idx == last_idx[ch]) continue;
      last_idx[ch] = idx;
      if (midi >= 0) {
        const DAC_CHANNEL pitch_ch = ch == 0 ? DAC_CHANNEL_A : DAC_CHANNEL_C;
        const DAC_CHANNEL gate_ch = ch == 0 ? DAC_CHANNEL_B : DAC_CHANNEL_D;
        // Pitch: the voice's copy of the melody, transposed by its
        // octave setting and referenced to C4 = 0V so the whole 1..7
        // range fits the DAC window (C1 = -3V at the bottom, +5.8V at
        // the top of octave 7 with key B). x128, not <<7: a voice below
        // the melody's own octave makes this negative.
        const int32_t pitch =
            (midi + 12 * voct - ROUNDGEN_PITCH_ZERO_MIDI) * 128;
        if (pitch != last_pitch[ch]) {
          OC::DAC::set_pitch(pitch_ch, pitch, 0);
          last_pitch[ch] = pitch;
        }
        // Gate. Nothing opens while the external clock has gone silent
        // (tempo unknown: the note is never on), nor under Gate len 0.
        // Otherwise a note start opens the gate; a gate already open
        // just keeps sounding, which at Gate len 100 is the legato
        // between consecutive notes (v21) and below it only happens
        // when the note boundary arrives before the deadline.
        const bool trig = roundgen_app.get_gate_trig() == 1;
        const uint8_t gate_len = roundgen_app.get_gate_len();
        if (clock_silent || (!trig && gate_len == 0)) {
          if (gate_on[ch]) {
            OC::DAC::set(gate_ch, OC::DAC::get_zero_offset(gate_ch));
            gate_on[ch] = false;
            pulse_until[ch] = 0;
          }
        } else if (!gate_on[ch]) {
          OC::DAC::set(gate_ch, OC::DAC::get_octave_offset(
              gate_ch, OCTAVES - OC::DAC::kOctaveZero - 0x2));
          gate_on[ch] = true;
          ++roundgen_app.gate_opens_[ch];  // the monitor's note-start event
          if (trig) {
            pulse_until[ch] = section_sec + 8;  // ~8 ms trigger pulse
          } else if (gate_len < 100) {
            // Gate length: gate_len% of the note's remaining length, in
            // wall time (a silent external clock never gets here).
            // Integer math: this target builds soft-float, so a float
            // chain here costs flash and cycles for nothing. Notes are
            // 1..max_beats beats and beat_ms never exceeds 4 s, so the
            // deadline stays far inside the int16 window the close test
            // uses.
            int32_t ms = (int32_t)((next_change[ch] - t) * (float)beat_ms);
            ms = ms * (int32_t)gate_len / 100;
            if (ms < 1) ms = 1;  // Gate len 0 never gets here (above)
            pulse_until[ch] = (uint16_t)(section_sec + (uint16_t)ms);
          } else {
            pulse_until[ch] = 0;  // 100% = hold (legato, as before)
          }
        }
      } else if (midi < 0 && gate_on[ch]) {
        const DAC_CHANNEL gate_ch = ch == 0 ? DAC_CHANNEL_B : DAC_CHANNEL_D;
        OC::DAC::set(gate_ch, OC::DAC::get_zero_offset(gate_ch));
        gate_on[ch] = false;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// App shell
// ---------------------------------------------------------------------------
void ROUNDGEN_init() {
  // Start the menu cursor at the TOP (Scale) so the global melody
  // section is visible on entry; the mutation rate is still
  // live-editable from the L encoder anywhere.
  roundgen_app_state.cursor.Init(0, roundgen_menu_rows() - 1);
  roundgen_app.Init();
}

size_t ROUNDGEN_storageSize() {
  return RoundgenApp::storageSize();
}

size_t ROUNDGEN_save(void *storage) {
  return roundgen_app.Save(storage);
}

size_t ROUNDGEN_restore(const void *storage) {
  return roundgen_app.Restore(storage);
}

void ROUNDGEN_handleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_SUSPEND:
    case OC::APP_EVENT_RESUME:
    case OC::APP_EVENT_SCREENSAVER_ON:
    case OC::APP_EVENT_SCREENSAVER_OFF:
      // the event tracker keeps its last channel state across menu
      // time; at worst the first note shown after the screensaver
      // appears blinks once (the tracker re-syncs on its first update)
      break;
  }
}

void ROUNDGEN_loop() {
  RoundgenApp &app = roundgen_app;

  // TR4 reroll, latched by the ISR into its own flag. (The loop used
  // to read/clear clocked_accum_ directly, racing the ~1 kHz section
  // that advances the beat — with an external clock almost every pulse
  // was wiped before the section could consume it.)
  if (app.reroll_pending_) {
    app.reroll_pending_ = false;
    app.generate_new_round();
  }

  // settings snapshot: regenerate when structure-relevant settings have
  // changed AND then settled. A regeneration is a full conform solve —
  // tens to hundreds of ms at length 24, worst cases worse — so
  // regenerating on every encoder step made scale changes extremely
  // laggy. A ~200 ms debounce collapses a scroll gesture into ONE
  // regeneration (the old melody keeps playing meanwhile).
  static uint8_t last_scale = 0xFF, last_key = 0xFF, last_voices = 0xFF;
  static uint8_t last_offset = 0xFF, last_length = 0xFF;
  static uint32_t last_change_ms = 0;
  static bool regen_pending = false;
  if (app.get_scale() != last_scale || app.get_key() != last_key ||
      app.get_voices() != last_voices || app.get_offset_setting() != last_offset ||
      app.get_length() != last_length) {
    last_scale = app.get_scale(); last_key = app.get_key();
    last_voices = app.get_voices(); last_offset = app.get_offset_setting();
    last_length = app.get_length();
    regen_pending = true;
    last_change_ms = millis();
    return;
  }
  if (regen_pending && (uint32_t)(millis() - last_change_ms) >= 200) {
    regen_pending = false;
    app.generate_new_round();
    return;
  }

  // Automatic mutation, published strictly on cycle boundaries. The
  // conform solve can take tens of ms to seconds (length 24 worst
  // cases), so the flow is: when a wrap is seen, SOLVE the next melody
  // while the current one keeps playing, then PUBLISH it at the NEXT
  // fresh wrap. A publish that lands between clock pulses resets pos_
  // and rewinds the beat count, swallowing whatever pulses arrived
  // during the solve (the "ignoring several external clocks" / ragged
  // internal tempo bugs) — so the new melody only ever starts exactly
  // on a boundary, no matter how long the solve took.
  if (!app.frozen() && app.get_mutation_setting() > 0 && app.playing()->n > 0) {
    static int last_cycle = 0;
    int cycle = app.cycle_;
    if (cycle != last_cycle) {
      if (cycle < last_cycle) {  // a reset restarted the round
        last_cycle = cycle;
        app.pending_publish_ = false;  // the reset wins over a pending change
        return;
      }
      last_cycle = cycle;
      if (app.pending_publish_) {
        if (roundgen_app.pos_ > 0.05f)
          return;  // the wrap is stale (a long solve just finished):
                   // wait for the next one — never publish mid-cycle
        app.pending_publish_ = false;
        app.publish(*app.slot_a_);
      }
      const int voices = app.get_voices();
      int scale[roundgen::RG_MAX_SCALE];
      int scale_len =
          roundgen::make_scale((int)app.get_scale(), app.get_key(),
                               ROUNDGEN_MELODY_OCTAVE, 2, scale);
      float rate = app.get_mutation_setting() / 10.0f;
      if (rate > 5.0f) rate = 5.0f;
      float rests = app.get_rest_setting() / 100.0f;
      if (rests > 1.0f) rests = 1.0f;
      float dissonance = app.get_dissonance_setting() / 100.0f;
      if (dissonance > 1.0f) dissonance = 1.0f;

      roundgen::Round& next = *app.slot_a_;
      int steps = static_cast<int>(rate);
      if (app.rng_.next() < rate - steps) ++steps;
      next = *app.playing();
      for (int s = 0; s < steps; ++s) {
        next = roundgen::mutate_round(next, voices, app.rng_, scale, scale_len,
                                      NULL, 0.3f, 2, 2, 40.0f, dissonance,
                                      rests, 20000, NULL);
      }
      app.pending_publish_ = true;  // publish at the next fresh wrap
    }
  }
}

// ---------------------------------------------------------------------------
// The mutation freeze tag (v25): drawn by the menu (ahead of the revision)
// and by the monitor (right of voice 1's row, the first row). A function,
// not an inline test at both sites: the frozen() check and the tag text
// then exist once — and flash is at the ceiling.
// ---------------------------------------------------------------------------
static void roundgen_freeze_tag() {
  if (roundgen_app.frozen()) graphics.print("FRZ ");
}

void ROUNDGEN_menu() {
  menu::DualTitleBar::Draw();
  graphics.print("Roundgen");
  menu::DualTitleBar::SetColumn(1);
  // the revision, with the freeze tag ahead of it (v25); the canon
  // offset is shown resolved on its own menu row (Canon off), and the
  // cycle counter is a mutation detail that belongs in the monitor,
  // not the menu
  roundgen_freeze_tag();
  graphics.print(ROUNDGEN_VERSION);
  // The row count depends on the clock source (BPM only exists under
  // the internal clock): keep the cursor's end in sync. A row past the
  // end (the clock source changed while the cursor sat below it) maps
  // to -1 and is skipped below; the next scroll pulls the cursor back
  // inside, as Scroll() clamps to start_/end_.
  roundgen_app_state.cursor.AdjustEnd(roundgen_menu_rows() - 1);
  menu::SettingsList<menu::kScreenLines, 0, menu::kDefaultValueX - 12>
      settings_list(roundgen_app_state.cursor);
  menu::SettingsListItem list_item;
  while (settings_list.available()) {
    const int row = settings_list.Next(list_item);
    const int id = roundgen_row_id(row);
    if (id < 0) continue;  // not a visible row (BPM just disappeared)
    if (id == ROUNDGEN_SETTING_OFFSET) {
      // The row always shows what is actually playing: 0 = auto (the
      // solver picks the offset), and fixed offsets that fall outside
      // the cycle length get clamped to half of it at generation — so
      // the raw setting and the sound can differ, and the row shows
      // the sound.
      int shown = static_cast<int>(roundgen_app.playing()->offset_beats);
      if (roundgen_app.playing()->n == 0)
        shown = roundgen_app.get_value(ROUNDGEN_SETTING_OFFSET);
      list_item.DrawDefault(shown, RoundgenApp::value_attr(id));
    } else if (id == ROUNDGEN_SETTING_OCT_SEL) {
      // the Oct row shows/edits the SELECTED voice's octave
      list_item.DrawDefault(
          roundgen_app.get_voice_oct_setting(
              roundgen_app.get_voice_sel() - 1),
          RoundgenApp::value_attr(id));
    } else if (id == ROUNDGEN_SETTING_CHAN_SEL) {
      const int v = roundgen_app.get_voice_sel() - 1;
      list_item.DrawDefault(roundgen_app.get_voice_chan(v),
                            RoundgenApp::value_attr(id));
    } else if (id == ROUNDGEN_ROW_REROLL) {
      list_item.SetPrintPos();
      graphics.print("Rerandomize");
      list_item.DrawCustom();
    } else {
      list_item.DrawDefault(roundgen_app.get_value(id),
                            RoundgenApp::value_attr(id));
    }
  }
}

// ---------------------------------------------------------------------------
// Screensaver: live voice monitor. The framework redraws the
// screensaver every ~1 ms, so this tracks the music in real time. One
// row per voice:
//   "*V2B C2"     - '*' flashes ~120 ms when the voice starts a note
//                   (a note start event on its channel: the gate
//                   opens or the sounded note changes), then the voice
//                   number + the output channel it is routed to
//                   (A/B, '-' = off)
//   " V1A C4"     - voice 1 is always listed
// The note shown is the one the voice is *sounding*: the melody note
// transposed to its octave setting (v24). While mutation is frozen,
// voice 1's row is followed by FRZ (v25).
// Voices routed to 'off' are skipped, except voice 1.
// ---------------------------------------------------------------------------

// Note name for the monitor ("C4", "F#3"); octave via print. (No
// roundgen-typed free functions: the sketch preprocessor cannot
// prototype those, so the melody walk lives inline in the screensaver.)
static void monitor_note_name(int midi) {
  static const char* const kNames[12] = {
      "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int pc = midi % 12;
  if (pc < 0) pc += 12;
  graphics.print(kNames[pc]);
  graphics.print((midi / 12) - 1);
}

void ROUNDGEN_screensaver() {
  const roundgen::Round* round = roundgen_app.playing();
  const int n = round->n;
  if (n < 1) return;
  const float total = round->total_beats();
  const float pos = roundgen_app.pos_;
  const float offset = round->offset_beats;

  // Channel map and the voice owning each channel's current slot this
  // redraw (same slot math as the ISR's note section; -1 = none). The
  // per-voice rows reuse their own melody walk for the event check.
  int ch_voices[2][8];
  int ch_n[2] = { 0, 0 };
  int slot_owner[2] = { -1, -1 };
  int8_t ch_of[8];  // each voice's channel, cached for the row pass
  for (int v = 0; v < 8; ++v) {
    const int c = roundgen_app.get_voice_chan(v);
    ch_of[v] = (int8_t)c;
    if (c < 2) ch_voices[c][ch_n[c]++] = v;
  }
  for (int ch = 0; ch < 2; ++ch) {
    const int kv = ch_n[ch];
    if (kv == 1) {
      slot_owner[ch] = ch_voices[ch][0];
    } else if (kv > 1) {
      const int beat = (int)pos;
      const float slot = 1.0f / kv;
      int m = (int)((pos - beat) / slot);
      if (m >= kv) m = kv - 1;
      slot_owner[ch] = ch_voices[ch][m];
    }
    // a channel with no voices simply has no owner: the tracker stays
    // as it was (the engine's pitch memory does the same)
  }

  // The tracker persists across mutation publishes exactly like the
  // engine's gate/pitch memory (only a real reset clears it), so the
  // new round's first notes flash iff the engine actually writes them.

  int row = 0;
  for (int v = 0; v < 8; ++v) {
    const int chan = ch_of[v];
    if (v > 0 && chan == 2) continue;  // routed voices only; V1 always
    // the voice's own melody position (pos + v*offset, cyclic)
    float t = pos + v * offset;
    while (t >= total) t -= total;
    int step = 0;
    float acc = 0.0f;
    for (int i = 0; i < n; ++i) {
      if (t < acc + round->notes[i].beats) { step = i; break; }
      acc += round->notes[i].beats;
    }
    const int midi = round->notes[step].midi;
    // the note the voice is sounding: the melody note transposed to its
    // octave setting — the row describes the output, like the engine's
    // pitch write, and doubles as the event key below
    const int cv = midi + 12 * roundgen_app.get_voice_oct(v);
    if (chan < 2 && v == slot_owner[chan]) {
      // this voice is the one the channel is sounding right now: a note
      // start event on the channel belongs to it and flashes its row.
      // Two keys: the sounded note (with -1 standing for a closed gate —
      // a rest->note always fires, and an equal pitch across a rest
      // still counts, exactly like the engine's gate) and the engine's
      // own gate-open count, which catches the re-articulated repeats
      // and trigger pulses that leave the pitch unchanged.
      if (midi >= 0) {
        const uint8_t opens = roundgen_app.gate_opens_[chan];
        if (opens != monitor_last_opens[chan] ||
            cv != monitor_last_cv[chan])
          monitor_flash[v] = 120;
        monitor_last_cv[chan] = cv;
        monitor_last_opens[chan] = opens;
      } else {
        monitor_last_cv[chan] = -1;
      }
    }
    graphics.setPrintPos(0, row * 8);
    graphics.print(monitor_flash[v] ? '*' : ' ');
    graphics.print("V");
    graphics.print(v + 1);
    graphics.print(chan == 0 ? "A " : (chan == 1 ? "B " : "- "));
    if (midi >= 0)
      monitor_note_name(cv);
    else
      graphics.print("R");
    ++row;
    if (monitor_flash[v]) --monitor_flash[v];
  }
  // the freeze tag, right of voice 1's row (v25). Called after the row
  // loop on purpose: a call inside it costs ~40 bytes of register spills
  // alone (flash is at the ceiling).
  graphics.setPrintPos(96, 0);
  roundgen_freeze_tag();
}

void ROUNDGEN_leftButton() {
  // rerandomize: a new round, same settings (same as TR4 / the menu row)
  roundgen_app.generate_new_round();
}

void ROUNDGEN_rightButton() {
  const int id = roundgen_row_id(roundgen_app_state.cursor.cursor_pos());
  if (id == ROUNDGEN_ROW_REROLL) {
    roundgen_app.generate_new_round();
  } else if (id >= 0) {
    roundgen_app_state.cursor.toggle_editing();
  }
}

void ROUNDGEN_handleButtonEvent(const UI::Event &event) {
  if (UI::EVENT_BUTTON_PRESS == event.type) {
    switch (event.control) {
      // UP is unused since v22 (it stepped the seed, now gone)
      case OC::CONTROL_BUTTON_L: ROUNDGEN_leftButton(); break;
      case OC::CONTROL_BUTTON_R: ROUNDGEN_rightButton(); break;
      // Down toggles the mutation freeze (v25); TR3 freezes while held,
      // and both states show FRZ (title bar + monitor)
      case OC::CONTROL_BUTTON_DOWN:
        roundgen_app.freeze_toggle_ = !roundgen_app.freeze_toggle_;
        break;
    }
  }
}

void ROUNDGEN_handleEncoderEvent(const UI::Event &event) {
  if (OC::CONTROL_ENCODER_L == event.control) {
    // live mutation-rate knob
    roundgen_app.change_value(ROUNDGEN_SETTING_MUTATION, event.value);
  } else if (OC::CONTROL_ENCODER_R == event.control) {
    if (roundgen_app_state.cursor.editing()) {
      const int id = roundgen_row_id(roundgen_app_state.cursor.cursor_pos());
      if (id == ROUNDGEN_SETTING_OCT_SEL) {
        // the Oct row edits the selected voice's octave
        roundgen_app.change_value(ROUNDGEN_SETTING_V1_OCT +
                                      roundgen_app.get_voice_sel() - 1,
                                  event.value);
      } else if (id == ROUNDGEN_SETTING_CHAN_SEL) {
        roundgen_app.change_value(ROUNDGEN_SETTING_V1_CHAN +
                                      roundgen_app.get_voice_sel() - 1,
                                  event.value);
      } else if (id >= 0) {
        roundgen_app.change_value(id, event.value);
      }
    } else {
      roundgen_app_state.cursor.Scroll(event.value);
    }
  }
}
