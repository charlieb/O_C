// roundgen_core.hpp — the melodic core of roundgen, ported to dependency-
// free C++11 for embedded use (Teensy / Ornament & Crime). No STL, no
// heap, no libm: fixed-size arrays, float math, a compact log2, and a
// 12-TET ratio table. See roundgen_core.cpp for the implementation and
// README.md for integration notes.
//
// Faithful port of roundgen/rounds.py + roundgen/notes.py. NOT
// bit-identical to the Python implementation (the PRNG differs), but the
// contracts that matter are preserved and host-tested:
//   - check_round(): the same cyclic-overlap consonance rules
//   - conform_round(): minimal-change, valid rounds; rests/durations kept
//   - mutate_round(): every result valid; rests drift toward
//     rest_probability (0 drains, 1 fills, 0.5 keeps ~half)
//   - deterministic given a seed
//
// Rests are midi < 0 (-1). All arrays are bounded by RG_MAX_NOTES.

#ifndef ROUNDGEN_CORE_HPP
#define ROUNDGEN_CORE_HPP

#include <cstdint>

namespace roundgen {

// Caps tuned for the o_C's RAM: the solver lives on the stack, so bigger
// caps mean deeper worst-case stack at mutation time. 24 notes keeps the
// mutation stack ~7 KB (headroom ~11.7 KB); 384 >= 24*23/2 covers every
// possible constraint pair.
constexpr int RG_MAX_NOTES = 24;
constexpr int RG_MAX_SCALE = 33;   // scale + chromatic pool entries
constexpr int RG_MAX_POOL = 33;
constexpr int RG_MAX_EDGES = 384;
constexpr int RG_MAX_DEGREE = RG_MAX_NOTES - 1;

// ---------------------------------------------------------------------------
// Deterministic PRNG (splitmix64). Draw order mirrors the Python code.
// ---------------------------------------------------------------------------
class Rng {
 public:
  Rng() : state_(0) {}
  explicit Rng(uint64_t seed);
  double next();                  // uniform in [0, 1)
  int randint(int lo, int hi);    // inclusive
  int randrange(int n);           // [0, n)
  int choice(const int* values, int n);
  // Partial Fisher-Yates: permutes `values` so the first min(k, n) entries
  // are a uniform sample without replacement.
  void sample(int* values, int n, int k);

 private:
  uint64_t state_;
};

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
struct Note {
  int midi = -1;        // < 0 = rest
  float beats = 1.0f;
  float velocity = 0.8f;
  Note() {}
  Note(int m, float b, float v) : midi(m), beats(b), velocity(v) {}
  bool is_rest() const { return midi < 0; }
};

struct Round {
  int n = 0;
  Note notes[RG_MAX_NOTES];
  float offset_beats = 0.0f;
  float total_beats() const;
};

// ---------------------------------------------------------------------------
// Pitch math and scales
// ---------------------------------------------------------------------------
float midi_to_freq(int midi);
float cents_between(float f1, float f2);

extern const char* SCALE_PALETTE[10];
extern const int SCALE_PATTERNS[10][7];
extern const int SCALE_LENS[10];

// The C-major-two-octaves pool used when no scale is given.
const int* default_scale();
int default_scale_len();

// A MIDI scale spanning `octaves` octaves, written into `out` (capacity
// RG_MAX_SCALE). Returns the number of scale notes. `index` selects from
// SCALE_PALETTE (0..9); the string overload is a host convenience.
int make_scale(int index, int key, int octave, int octaves, int* out);
int make_scale(const char* name, int key, int octave, int octaves, int* out);

// ---------------------------------------------------------------------------
// Harmony
// ---------------------------------------------------------------------------
bool is_consonant(float f1, float f2, float tolerance_cents,
                  float dissonance);
bool check_round(const Round& r, int voices, float tolerance_cents,
                 float dissonance, int* violations = nullptr);

// ---------------------------------------------------------------------------
// Generation and conformance
// ---------------------------------------------------------------------------
Round generate_random_round(int length, float offset_beats,
                            const int* scale, int scale_len, int max_beats,
                            float rest_probability, Rng& rng);

bool conform_round(const Round& in, int voices, float offset_beats,
                   const int* scale, int scale_len, float tolerance_cents,
                   int max_nodes, const float* interval_weights, Round* out);

bool generate_round(int length, int voices, float offset_beats,
                    const int* scale, int scale_len, int max_beats,
                    float rest_probability, const float* interval_weights,
                    Rng& rng, Round* out);

// ---------------------------------------------------------------------------
// Live mutation
// ---------------------------------------------------------------------------
Round mutate_round(const Round& current, int voices, Rng& rng,
                   const int* scale, int scale_len, const Round* home,
                   float pull, int max_changes, int max_steps,
                   float tolerance_cents, float dissonance,
                   float rest_probability, int max_nodes,
                   const float* interval_weights);

Round transpose_round(const Round& r, int semitones);
Round fold_into_range(const Round& r, const int* scale, int scale_len);

// ---------------------------------------------------------------------------
// Scheduling helper
// ---------------------------------------------------------------------------
int note_at(const Round& r, float beat);

}  // namespace roundgen

#endif  // ROUNDGEN_CORE_HPP
