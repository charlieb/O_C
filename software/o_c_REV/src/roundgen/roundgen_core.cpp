// roundgen_core.cpp — implementation of the roundgen melodic core.
// Faithful C++11 port of roundgen/rounds.py + roundgen/notes.py.
// No STL / heap / libm: fixed-size arrays and float math.

#include "roundgen_core.hpp"

#include <cmath>

namespace roundgen {

// ---------------------------------------------------------------------------
// PRNG (splitmix64)
// ---------------------------------------------------------------------------
namespace {

uint64_t splitmix64(uint64_t& x) {
  x += 0x9E3779B97F4A7C15ull;
  uint64_t z = x;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

Rng::Rng(uint64_t seed) : state_(seed) {}

double Rng::next() {
  return static_cast<double>(splitmix64(state_) >> 11) * (1.0 / 9007199254740992.0);
}

int Rng::randint(int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + static_cast<int>(next() * static_cast<double>(hi - lo + 1));
}

int Rng::randrange(int n) {
  if (n <= 0) return 0;
  return static_cast<int>(next() * static_cast<double>(n));
}

int Rng::choice(const int* values, int n) {
  if (n <= 0) return 0;
  return values[randrange(n)];
}

void Rng::sample(int* values, int n, int k) {
  if (k > n) k = n;
  for (int i = 0; i < k; ++i) {
    int j = i + randrange(n - i);
    int tmp = values[i];
    values[i] = values[j];
    values[j] = tmp;
  }
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
float Round::total_beats() const {
  float t = 0.0f;
  for (int i = 0; i < n; ++i) t += notes[i].beats;
  return t;
}

// ---------------------------------------------------------------------------
// Pitch math and scales
// ---------------------------------------------------------------------------
float midi_to_freq(int midi) {
  // 12-TET semitone ratios; no libm.
  static const float ratio[12] = {
      1.0f, 1.0594631f, 1.1224620f, 1.1892071f, 1.2599210f, 1.3348399f,
      1.4142136f, 1.4983071f, 1.5874011f, 1.6817928f, 1.7817974f, 1.8877486f};
  int n = midi - 69;
  int oct = n / 12;
  int k = n % 12;
  if (k < 0) {
    k += 12;
    oct -= 1;
  }
  return 440.0f * ratio[k] * static_cast<float>(ldexp(1.0, oct));
}

// Accurate compact log2 (frexp + atanh series); no libm dependency.
static float log2_fast(float x) {
  if (x <= 0.0f) return 0.0f;
  int e = 0;
  float m = frexpf(x, &e);        // x = m * 2^e, m in 0.5..1
  float u = 2.0f * (m - 0.5f);    // log2(1+u), u in 0..1
  float z = u / (2.0f + u);       // atanh series: |z| < 1/3, fast convergence
  float z2 = z * z;
  float ln1p = 2.0f * z * (1.0f + z2 * (1.0f / 3.0f + z2 * (1.0f / 5.0f +
              z2 * (1.0f / 7.0f + z2 * (1.0f / 9.0f + z2 / 11.0f)))));
  return (static_cast<float>(e) - 1.0f) + ln1p * 1.4426950f;  // * log2(e)
}

float cents_between(float f1, float f2) {
  if (f1 > f2) {
    float tmp = f1;
    f1 = f2;
    f2 = tmp;
  }
  if (f1 <= 0.0f || f2 <= 0.0f) return 0.0f;
  return 1200.0f * log2_fast(f2 / f1);
}

const char* SCALE_PALETTE[10] = {
    "major", "natural minor", "dorian", "phrygian", "lydian",
    "mixolydian", "locrian", "pentatonic major", "pentatonic minor", "blues",
};
const int SCALE_PATTERNS[10][7] = {
    {0, 2, 4, 5, 7, 9, 11},        // major
    {0, 2, 3, 5, 7, 8, 10},        // natural minor
    {0, 2, 3, 5, 7, 9, 10},        // dorian
    {0, 1, 3, 5, 7, 8, 10},        // phrygian
    {0, 2, 4, 6, 7, 9, 11},        // lydian
    {0, 2, 4, 5, 7, 9, 10},        // mixolydian
    {0, 1, 3, 5, 6, 8, 10},        // locrian
    {0, 2, 4, 7, 9, 0, 0},         // pentatonic major
    {0, 3, 5, 7, 10, 0, 0},        // pentatonic minor
    {0, 3, 5, 6, 7, 10, 0},        // blues
};
const int SCALE_LENS[10] = {7, 7, 7, 7, 7, 7, 7, 5, 5, 6};

const int* default_scale() {
  static const int s[15] = {
      60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83, 84};
  return s;
}
int default_scale_len() { return 15; }

int make_scale(int index, int key, int octave, int octaves, int* out) {
  int idx = (index >= 0 && index < 10) ? index : 0;
  const int len = SCALE_LENS[idx];
  const int base = 12 * (octave + 1) + key;
  if (octaves < 1) octaves = 1;
  int count = 0;
  for (int o = 0; o < octaves && count < RG_MAX_SCALE; ++o)
    for (int s = 0; s < len && count < RG_MAX_SCALE; ++s)
      out[count++] = base + 12 * o + SCALE_PATTERNS[idx][s];
  return count;
}

int make_scale(const char* name, int key, int octave, int octaves, int* out) {
  int idx = 0;
  for (int i = 0; i < 10; ++i) {
    const char* a = name;
    const char* b = SCALE_PALETTE[i];
    while (*a && *a == *b) {
      ++a;
      ++b;
    }
    if (*a == 0 && *b == 0) {
      idx = i;
      break;
    }
  }
  return make_scale(idx, key, octave, octaves, out);
}

// ---------------------------------------------------------------------------
// Harmony
// ---------------------------------------------------------------------------
namespace {

// Consonant intervals (cents) and the dissonant ladder, mild -> harsh.
const int CONSONANT_CENTS[8] = {0, 300, 400, 500, 700, 800, 900, 1200};
const int DISSONANT_CENTS[5] = {200, 1000, 600, 100, 1100};

// Cents permitted at this dissonance level, written into `out` (capacity
// 13). Returns the count.
int allowed_cents(float dissonance, int* out) {
  float d = dissonance < 0.0f ? 0.0f : (dissonance > 1.0f ? 1.0f : dissonance);
  for (int i = 0; i < 8; ++i) out[i] = CONSONANT_CENTS[i];
  int n = static_cast<int>(d * 5.0f + 0.5f);
  if (n > 5) n = 5;
  for (int i = 0; i < n; ++i) out[8 + i] = DISSONANT_CENTS[i];
  return 8 + n;
}

int index_in(const int* v, int n, int x) {
  for (int i = 0; i < n; ++i)
    if (v[i] == x) return i;
  return -1;
}

int index_in_i8(const int8_t* v, int n, int x) {
  for (int i = 0; i < n; ++i)
    if (v[i] == x) return i;
  return -1;
}

// Earliest beat where the cyclic spans intersect, else false.
bool overlap_beat(float a_start, float a_len, float b_start, float b_len,
                  float total, float* out_beat) {
  for (int m = -1; m <= 1; ++m) {
    float lo = a_start > b_start + m * total ? a_start : b_start + m * total;
    float hi1 = a_start + a_len;
    float hi2 = b_start + m * total + b_len;
    float hi = hi1 < hi2 ? hi1 : hi2;
    if (lo < hi) {
      // fmod-like positive remainder
      float r = lo - static_cast<float>(static_cast<int>(lo / total)) * total;
      if (r < 0.0f) r += total;
      *out_beat = r;
      return true;
    }
  }
  return false;
}

}  // namespace

bool is_consonant(float f1, float f2, float tolerance_cents,
                  float dissonance) {
  float cents = cents_between(f1, f2);
  // octave-reduce to 0..1200
  int oct = static_cast<int>(cents / 1200.0f);
  cents -= static_cast<float>(oct) * 1200.0f;
  int allowed[13];
  int n = allowed_cents(dissonance, allowed);
  for (int i = 0; i < n; ++i) {
    float diff = cents - static_cast<float>(allowed[i]);
    if (diff < 0.0f) diff = -diff;
    if (diff <= tolerance_cents) return true;
  }
  return false;
}

bool check_round(const Round& r, int voices, float tolerance_cents,
                 float dissonance, int* violations) {
  int bad = 0;
  if (r.n == 0) {
    if (violations) *violations = 0;
    return true;
  }
  const float offset = r.offset_beats;
  const float total = r.total_beats();
  float starts[RG_MAX_NOTES];
  float pos = 0.0f;
  for (int i = 0; i < r.n; ++i) {
    starts[i] = pos;
    pos += r.notes[i].beats;
  }
  for (int i = 0; i < r.n; ++i) {
    if (r.notes[i].is_rest()) continue;
    for (int j = i + 1; j < r.n; ++j) {
      if (r.notes[j].is_rest()) continue;
      for (int k = 1; k < voices; ++k) {
        float shift = k * offset;
        int soct = static_cast<int>(shift / total);
        shift -= static_cast<float>(soct) * total;
        float t;
        bool found = overlap_beat(starts[i], r.notes[i].beats,
                                  starts[j] + shift, r.notes[j].beats,
                                  total, &t);
        if (!found)
          found = overlap_beat(starts[j], r.notes[j].beats,
                               starts[i] + shift, r.notes[i].beats,
                               total, &t);
        if (found) {
          if (!is_consonant(midi_to_freq(r.notes[i].midi),
                            midi_to_freq(r.notes[j].midi),
                            tolerance_cents, dissonance))
            ++bad;
          break;  // one representative pair per (i, j) is enough
        }
      }
    }
  }
  if (violations) *violations = bad;
  return bad == 0;
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------
Round generate_random_round(int length, float offset_beats,
                            const int* scale, int scale_len, int max_beats,
                            float rest_probability, Rng& rng) {
  Round out;
  if (length > RG_MAX_NOTES) length = RG_MAX_NOTES;
  const int* pool = scale != nullptr ? scale : default_scale();
  const int pool_len = scale != nullptr ? scale_len : default_scale_len();
  if (max_beats < 1) max_beats = 1;
  for (int i = 0; i < length; ++i) {
    float beats = static_cast<float>(rng.randint(1, max_beats));
    if (rest_probability > 0.0f && rng.next() < rest_probability) {
      out.notes[i] = Note{-1, beats, 0.8f};
    } else {
      out.notes[i] = Note{rng.choice(pool, pool_len), beats, 0.8f};
    }
  }
  out.n = length;
  if (offset_beats < 0.0f) {
    float total = out.total_beats();
    int hi = static_cast<int>(total) - 1;
    if (hi < 1) hi = 1;
    offset_beats = static_cast<float>(rng.randint(1, hi));
  }
  out.offset_beats = offset_beats;
  return out;
}

// ---------------------------------------------------------------------------
// Conformance solver (fixed arrays, int8 scratch)
// ---------------------------------------------------------------------------
namespace {

struct Solver {
  // Variable pool: scale + chromatic semitones once dissonance permits.
  int8_t pool[RG_MAX_POOL];
  int pool_len;
  int8_t original[RG_MAX_NOTES];
  int n;
  float tolerance;
  float dissonance;
  int max_nodes;
  const float* weights;  // 12 interval weights or nullptr

  // Domains ordered by distance from the original pitch.
  int8_t domains[RG_MAX_NOTES][RG_MAX_POOL];
  uint8_t domain_len[RG_MAX_NOTES];
  // Home pitch per variable (-1 = none) pinned first in its domain.
  int8_t home[RG_MAX_NOTES];
  // Constraint graph.
  uint8_t adj[RG_MAX_NOTES][RG_MAX_DEGREE];
  uint8_t adj_len[RG_MAX_NOTES];
  // Static variable order: most-constrained first, index tiebreak.
  uint8_t order[RG_MAX_NOTES];
  int8_t assignment[RG_MAX_NOTES];
  int nodes;

  void build(const int* scale, int scale_len, const int* originals, int n_,
             const int* home_map, float tolerance_, float dissonance_,
             int max_nodes_, const float* weights_) {
    n = n_;
    tolerance = tolerance_;
    dissonance = dissonance_;
    max_nodes = max_nodes_;
    weights = weights_;
    nodes = 0;
    for (int i = 0; i < n; ++i) {
      original[i] = static_cast<int8_t>(originals[i]);
      assignment[i] = -1;
      home[i] = (home_map != nullptr) ? static_cast<int8_t>(home_map[i]) : -1;
      adj_len[i] = 0;
    }
    // pool: the scale, plus chromatic semitones when dissonance permits.
    const int* sc = scale != nullptr ? scale : default_scale();
    const int sl = scale != nullptr ? scale_len : default_scale_len();
    pool_len = sl > RG_MAX_POOL ? RG_MAX_POOL : sl;
    int lo = sc[0], hi = sc[sl - 1];
    for (int i = 0; i < pool_len; ++i) pool[i] = static_cast<int8_t>(sc[i]);
    if (dissonance > 0.0f) {
      for (int p = lo; p <= hi && pool_len < RG_MAX_POOL; ++p) {
        if (index_in(sc, sl, p) < 0) pool[pool_len++] = static_cast<int8_t>(p);
      }
    }
    // domains: distances, home pitch first.
    for (int i = 0; i < n; ++i) {
      int count = 0;
      int target = originals[i];
      for (int d = 0; d < pool_len; ++d) {
        int best = -1;
        int best_dist = 1 << 30;
        for (int p = 0; p < pool_len; ++p) {
          int used = 0;
          for (int q = 0; q < count; ++q)
            if (domains[i][q] == pool[p]) used = 1;
          if (used) continue;
          int dist = pool[p] - target;
          if (dist < 0) dist = -dist;
          if (dist < best_dist) {
            best_dist = dist;
            best = p;
          }
        }
        if (best < 0) break;
        domains[i][count++] = pool[best];
      }
      domain_len[i] = static_cast<uint8_t>(count);
      // home first
      if (home[i] >= 0) {
        int hi2 = index_in_i8(domains[i], count, home[i]);
        if (hi2 >= 0) {
          int8_t hp = domains[i][hi2];
          for (int q = hi2; q > 0; --q) domains[i][q] = domains[i][q - 1];
          domains[i][0] = hp;
        }
      }
    }
  }

  void set_edges(const uint8_t* edge_a, const uint8_t* edge_b, int edge_count) {
    for (int i = 0; i < n; ++i) adj_len[i] = 0;
    for (int e = 0; e < edge_count; ++e) {
      uint8_t a = edge_a[e], b = edge_b[e];
      adj[a][adj_len[a]++] = b;
      adj[b][adj_len[b]++] = a;
    }
    for (int i = 0; i < n; ++i) order[i] = static_cast<uint8_t>(i);
    // insertion sort: descending degree, index tiebreak
    for (int i = 1; i < n; ++i) {
      uint8_t v = order[i];
      int j = i - 1;
      while (j >= 0) {
        uint8_t w = order[j];
        if (adj_len[w] > adj_len[v] ||
            (adj_len[w] == adj_len[v] && w < v))
          break;
        order[j + 1] = w;
        --j;
      }
      order[j + 1] = v;
    }
  }

  bool consonant(int p, int q) const {
    return is_consonant(midi_to_freq(p), midi_to_freq(q), tolerance, dissonance);
  }

  bool feasible(int i, int p) const {
    for (int e = 0; e < adj_len[i]; ++e) {
      int j = adj[i][e];
      if (assignment[j] >= 0 && !consonant(p, assignment[j])) return false;
    }
    return true;
  }

  float weighted_score(int var, int p) const {
    float cost = static_cast<float>(p - original[var]);
    if (cost < 0.0f) cost = -cost;
    for (int e = 0; e < adj_len[var]; ++e) {
      int j = adj[var][e];
      if (assignment[j] < 0) continue;
      int pc = p - assignment[j];
      if (pc < 0) pc = -pc;
      pc %= 12;
      float w = weights != nullptr ? weights[pc] : 0.5f;
      cost += 1.0f - w;
    }
    return cost;
  }

  // Stable insertion sort of a domain by ascending key(entry).
  template <typename Key>
  void sort_domain(int8_t* dom, int len, Key key) {
    for (int i = 1; i < len; ++i) {
      int8_t v = dom[i];
      float kv = key(v);
      int j = i - 1;
      while (j >= 0 && key(dom[j]) > kv) {
        dom[j + 1] = dom[j];
        --j;
      }
      dom[j + 1] = v;
    }
  }

  bool search() {
    ++nodes;
    if (nodes > max_nodes) return false;
    int assigned = 0;
    for (int i = 0; i < n; ++i)
      if (assignment[i] >= 0) ++assigned;
    if (assigned == n) return true;
    // minimum-remaining-values over the static order
    int var = -1;
    int best_len = 0;
    for (int oi = 0; oi < n; ++oi) {
      int i = order[oi];
      if (assignment[i] >= 0) continue;
      int len = domain_len[i];
      if (var < 0 || len < best_len) {
        var = i;
        best_len = len;
      }
    }
    // Order the domain in place: interval-weighted score when weights are
    // given, home pitch first in both cases. No per-frame arrays, so the
    // recursion uses a small fixed stack (embedded target). Forward
    // checking is deliberately omitted: it prunes only to fail branches
    // faster, never changes which assignment is found first, and its
    // per-frame snapshots blew the Teensy stack (1 KB per recursion
    // level).
    int len = domain_len[var];
    if (weights != nullptr) {
      sort_domain(domains[var], len, [this, var](int8_t p) {
        return weighted_score(var, p);
      });
    }
    int hp = home[var];
    if (hp >= 0) {
      int hi2 = index_in_i8(domains[var], len, hp);
      if (hi2 > 0) {
        int8_t t = domains[var][hi2];
        for (int q = hi2; q > 0; --q) domains[var][q] = domains[var][q - 1];
        domains[var][0] = t;
      }
    }
    for (int d = 0; d < len; ++d) {
      int p = domains[var][d];
      if (!feasible(var, p)) continue;
      assignment[var] = static_cast<int8_t>(p);
      if (search()) return true;
      assignment[var] = -1;
    }
    return false;
  }
};

// Constraint edges: positions of note pairs that must harmonize.
int constraint_edges(const Round& r, int voices, uint8_t* edge_a,
                     uint8_t* edge_b) {
  int count = 0;
  if (r.n == 0) return 0;
  const float total = r.total_beats();
  const float offset = r.offset_beats;
  float starts[RG_MAX_NOTES];
  float pos = 0.0f;
  for (int i = 0; i < r.n; ++i) {
    starts[i] = pos;
    pos += r.notes[i].beats;
  }
  for (int i = 0; i < r.n; ++i) {
    if (r.notes[i].is_rest()) continue;
    for (int j = i + 1; j < r.n; ++j) {
      if (r.notes[j].is_rest()) continue;
      for (int k = 1; k < voices; ++k) {
        float shift = k * offset;
        int soct = static_cast<int>(shift / total);
        shift -= static_cast<float>(soct) * total;
        float t;
        bool found = overlap_beat(starts[i], r.notes[i].beats,
                                  starts[j] + shift, r.notes[j].beats,
                                  total, &t);
        if (!found)
          found = overlap_beat(starts[j], r.notes[j].beats,
                               starts[i] + shift, r.notes[i].beats,
                               total, &t);
        if (found && count < RG_MAX_EDGES) {
          edge_a[count] = static_cast<uint8_t>(i);
          edge_b[count] = static_cast<uint8_t>(j);
          ++count;
          break;
        }
      }
    }
  }
  return count;
}

// Solve over the active (non-rest) notes only; rests stay rests.
bool conform_active(const Round& r, const uint8_t* edge_a,
                    const uint8_t* edge_b, int edge_count, const int* scale,
                    int scale_len, float tolerance_cents, int max_nodes,
                    float dissonance, const int* home_pitches,
                    const float* interval_weights, int* result) {
  int n = r.n;
  for (int i = 0; i < n; ++i) result[i] = -1;
  int active[RG_MAX_NOTES];
  int active_n = 0;
  for (int i = 0; i < n; ++i)
    if (!r.notes[i].is_rest()) active[active_n++] = i;
  if (active_n == 0) return true;
  int idx_map[RG_MAX_NOTES];
  for (int i = 0; i < n; ++i) idx_map[i] = -1;
  for (int a = 0; a < active_n; ++a) idx_map[active[a]] = a;
  uint8_t remap_a[RG_MAX_EDGES], remap_b[RG_MAX_EDGES];
  int remap_n = 0;
  for (int e = 0; e < edge_count; ++e) {
    int a = idx_map[edge_a[e]];
    int b = idx_map[edge_b[e]];
    if (a >= 0 && b >= 0) {
      remap_a[remap_n] = static_cast<uint8_t>(a);
      remap_b[remap_n] = static_cast<uint8_t>(b);
      ++remap_n;
    }
  }
  int originals[RG_MAX_NOTES];
  int home_active[RG_MAX_NOTES];
  for (int a = 0; a < active_n; ++a) {
    originals[a] = r.notes[active[a]].midi;
    home_active[a] = home_pitches != nullptr ? home_pitches[active[a]] : -1;
  }
  // The solver runs only in the loop thread (never from an ISR); keeping
  // it on the stack keeps .bss small — the o_C has only ~12 KB of RAM
  // headroom and a build that grows it black-screens the display at boot.
  // The recursion itself is cheap: no per-frame arrays (forward checking
  // was removed), so depth 32 costs ~2 KB total.
  Solver solver;
  solver.build(scale, scale_len, originals, active_n, home_active,
               tolerance_cents, dissonance, max_nodes, interval_weights);
  solver.set_edges(remap_a, remap_b, remap_n);
  if (!solver.search()) return false;
  for (int a = 0; a < active_n; ++a) result[active[a]] = solver.assignment[a];
  return true;
}

}  // namespace

bool conform_round(const Round& in, int voices, float offset_beats,
                   const int* scale, int scale_len, float tolerance_cents,
                   int max_nodes, const float* interval_weights, Round* out) {
  if (in.n == 0) return false;
  const float total = in.total_beats();
  float candidates[RG_MAX_NOTES];
  int cand_n = 0;
  if (offset_beats >= 0.0f) {
    candidates[cand_n++] = offset_beats;
  } else {
    int hi = static_cast<int>(total);
    if (hi < 1) hi = 1;
    for (int o = 1; o < hi && cand_n < RG_MAX_NOTES; ++o)
      candidates[cand_n++] = static_cast<float>(o);
  }
  int original[RG_MAX_NOTES];
  for (int i = 0; i < in.n; ++i) original[i] = in.notes[i].midi;
  bool found = false;
  int best_changes = 0, best_distance = 0;
  float best_offset = 0.0f;
  int best_pitches[RG_MAX_NOTES];
  uint8_t edge_a[RG_MAX_EDGES], edge_b[RG_MAX_EDGES];
  for (int c = 0; c < cand_n; ++c) {
    float off = candidates[c];
    if (off <= 0.0f || off >= total) continue;
    Round trial = in;
    trial.offset_beats = off;
    int edge_count = constraint_edges(trial, voices, edge_a, edge_b);
    int pitches[RG_MAX_NOTES];
    if (!conform_active(in, edge_a, edge_b, edge_count, scale, scale_len,
                        tolerance_cents, max_nodes, 0.0f, nullptr,
                        interval_weights, pitches))
      continue;
    int changes = 0, distance = 0;
    for (int i = 0; i < in.n; ++i) {
      if (original[i] != pitches[i]) ++changes;
      if (original[i] >= 0 && pitches[i] >= 0) {
        int d = original[i] - pitches[i];
        if (d < 0) d = -d;
        distance += d;
      }
    }
    bool better = !found;
    if (found) {
      if (changes != best_changes) better = changes < best_changes;
      else if (distance != best_distance) better = distance < best_distance;
      else better = off < best_offset;
    }
    if (better) {
      found = true;
      best_changes = changes;
      best_distance = distance;
      best_offset = off;
      for (int i = 0; i < in.n; ++i) best_pitches[i] = pitches[i];
    }
  }
  if (!found) return false;
  out->n = in.n;
  for (int i = 0; i < in.n; ++i) {
    out->notes[i].midi = best_pitches[i];
    out->notes[i].beats = in.notes[i].beats;
    out->notes[i].velocity = in.notes[i].velocity;
  }
  out->offset_beats = best_offset;
  return true;
}

bool generate_round(int length, int voices, float offset_beats,
                    const int* scale, int scale_len, int max_beats,
                    float rest_probability, const float* interval_weights,
                    Rng& rng, Round* out) {
  Round raw = generate_random_round(length, offset_beats, scale, scale_len,
                                    max_beats, rest_probability, rng);
  return conform_round(raw, voices, -1.0f, scale, scale_len, 40.0f, 20000,
                       interval_weights, out);
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------
namespace {

int nearest_scale_pitch(int pitch, const int* scale, int scale_len) {
  int best = 0;
  for (int i = 1; i < scale_len; ++i) {
    int da = scale[i] - pitch;
    if (da < 0) da = -da;
    int db = scale[best] - pitch;
    if (db < 0) db = -db;
    if (da < db) best = i;
  }
  return scale[best];
}

// A random scale pitch a few diatonic steps from `pitch`.
int neighbor_pitch(int pitch, const int* scale, int scale_len, int max_steps,
                   Rng& rng) {
  if (scale_len == 0) return pitch;
  int idx = index_in(scale, scale_len, pitch);
  if (idx < 0) {
    pitch = nearest_scale_pitch(pitch, scale, scale_len);
    idx = index_in(scale, scale_len, pitch);
  }
  int lo = idx - max_steps;
  if (lo < 0) lo = 0;
  int hi = idx + max_steps;
  if (hi > scale_len - 1) hi = scale_len - 1;
  int options[RG_MAX_SCALE];
  int on = 0;
  for (int i = lo; i <= hi; ++i)
    if (i != idx) options[on++] = i;
  if (on == 0) return pitch;
  return scale[rng.choice(options, on)];
}

// Pitches forming an allowed dissonant interval with a partner.
int dissonant_targets(int partner_midi, const int* scale, int scale_len,
                      float dissonance, float tolerance_cents, int* out) {
  int allowed[13];
  int an = allowed_cents(dissonance, allowed);
  // drop the consonant set, keep only the dissonant ladder
  int dissonant[5];
  int dn = 0;
  for (int i = 8; i < an; ++i) dissonant[dn++] = allowed[i];
  if (dn == 0) return 0;
  int pool[RG_MAX_POOL];
  int pool_len = scale_len > RG_MAX_POOL ? RG_MAX_POOL : scale_len;
  for (int i = 0; i < pool_len; ++i) pool[i] = scale[i];
  for (int p = scale[0]; p <= scale[scale_len - 1] && pool_len < RG_MAX_POOL; ++p)
    if (index_in(scale, scale_len, p) < 0) pool[pool_len++] = p;
  const float partner_freq = midi_to_freq(partner_midi);
  int count = 0;
  for (int i = 0; i < pool_len; ++i) {
    float cents = cents_between(partner_freq, midi_to_freq(pool[i]));
    int oct = static_cast<int>(cents / 1200.0f);
    cents -= static_cast<float>(oct) * 1200.0f;
    for (int c = 0; c < dn; ++c) {
      float diff = cents - static_cast<float>(dissonant[c]);
      if (diff < 0.0f) diff = -diff;
      if (diff <= tolerance_cents) {
        out[count++] = pool[i];
        break;
      }
    }
  }
  return count;
}

Round round_from_pitches(const int* pitches, const Round& src) {
  Round out;
  out.n = src.n;
  for (int i = 0; i < src.n; ++i) {
    out.notes[i].midi = pitches[i];
    out.notes[i].beats = src.notes[i].beats;
    out.notes[i].velocity = src.notes[i].velocity;
  }
  out.offset_beats = src.offset_beats;
  return out;
}

}  // namespace

Round mutate_round(const Round& current, int voices, Rng& rng,
                   const int* scale, int scale_len, const Round* home,
                   float pull, int max_changes, int max_steps,
                   float tolerance_cents, float dissonance,
                   float rest_probability, int max_nodes,
                   const float* interval_weights) {
  if (current.n == 0) return current;
  const int* sc = scale != nullptr ? scale : default_scale();
  const int sl = scale != nullptr ? scale_len : default_scale_len();
  const int n = current.n;
  int original[RG_MAX_NOTES], pitches[RG_MAX_NOTES];
  for (int i = 0; i < n; ++i) original[i] = pitches[i] = current.notes[i].midi;

  // rest_probability is the target rest density: each step nudges one
  // random position toward it (note -> rest with probability p, rest ->
  // note with probability 1-p). A revived note takes a random scale
  // pitch; the solver repairs its harmony below.
  int i_toggle = rng.randrange(n);
  if (original[i_toggle] < 0) {
    if (rng.next() < 1.0 - rest_probability)
      pitches[i_toggle] = rng.choice(sc, sl);
  } else if (rng.next() < rest_probability) {
    pitches[i_toggle] = -1;
  }

  Round toggled = round_from_pitches(pitches, current);
  uint8_t edge_a[RG_MAX_EDGES], edge_b[RG_MAX_EDGES];
  int edge_count = constraint_edges(toggled, voices, edge_a, edge_b);
  uint8_t partners[RG_MAX_NOTES][RG_MAX_DEGREE];
  uint8_t partners_len[RG_MAX_NOTES];
  for (int i = 0; i < n; ++i) partners_len[i] = 0;
  for (int e = 0; e < edge_count; ++e) {
    int a = edge_a[e], b = edge_b[e];
    partners[a][partners_len[a]++] = static_cast<uint8_t>(b);
    partners[b][partners_len[b]++] = static_cast<uint8_t>(a);
  }
  int active[RG_MAX_NOTES];
  int active_n = 0;
  for (int i = 0; i < n; ++i)
    if (pitches[i] >= 0) active[active_n++] = i;
  int count = rng.randint(1, max_changes);
  rng.sample(active, active_n, count < active_n ? count : active_n);
  for (int ci = 0; ci < count && ci < active_n; ++ci) {
    const int i = active[ci];
    int target = pitches[i];
    if (home != nullptr && rng.next() < pull) {
      target = home->notes[i].midi;
      if (target < 0) target = pitches[i];
    } else if (dissonance > 0.0f &&
               rng.next() < dissonance * 0.5f &&
               partners_len[i] > 0) {
      int j = partners[i][rng.randrange(partners_len[i])];
      // guard: the partner may be a revived rest in the original melody;
      // skip the dissonant aim then (the Python version would pass None)
      if (current.notes[j].midi < 0) {
        target = neighbor_pitch(pitches[i], sc, sl, max_steps, rng);
      } else {
        int opts[RG_MAX_POOL];
        int on = dissonant_targets(current.notes[j].midi, sc, sl, dissonance,
                                   tolerance_cents, opts);
        target = on > 0 ? opts[rng.randrange(on)]
                        : neighbor_pitch(pitches[i], sc, sl, max_steps, rng);
      }
    } else {
      target = neighbor_pitch(pitches[i], sc, sl, max_steps, rng);
    }
    pitches[i] = target;
  }

  bool unchanged = true;
  for (int i = 0; i < n; ++i)
    if (pitches[i] != original[i]) unchanged = false;
  if (unchanged) {
    // nothing was proposed: leave a melody that is already valid at this
    // level, but repair one that isn't (tension may have fallen).
    if (check_round(current, voices, tolerance_cents, dissonance))
      return current;
    int solved[RG_MAX_NOTES];
    if (conform_active(toggled, edge_a, edge_b, edge_count, sc, sl,
                       tolerance_cents, max_nodes, dissonance, pitches,
                       interval_weights, solved))
      return round_from_pitches(solved, current);
    return current;
  }

  Round proposed = round_from_pitches(pitches, current);
  int home_pitches[RG_MAX_NOTES];
  const int* hp_ptr = nullptr;
  if (home != nullptr) {
    for (int i = 0; i < n; ++i) home_pitches[i] = home->notes[i].midi;
    hp_ptr = home_pitches;
  }
  int solved[RG_MAX_NOTES];
  if (!conform_active(proposed, edge_a, edge_b, edge_count, sc, sl,
                      tolerance_cents, max_nodes, dissonance, hp_ptr,
                      interval_weights, solved)) {
    if (home == nullptr) {
      // retry anchored on the incoming melody so only the actual
      // violations move (mirrors the Python fallback)
      if (!conform_active(proposed, edge_a, edge_b, edge_count, sc, sl,
                          tolerance_cents, max_nodes, dissonance, original,
                          interval_weights, solved))
        return current;
    } else {
      return current;
    }
  }
  // pull toward home: snap each note back to its home pitch where it stays
  // consonant with its conformed neighbours
  if (home != nullptr && pull > 0.0f) {
    for (int i = 0; i < n; ++i) {
      if (solved[i] < 0) continue;
      int hp = home->notes[i].midi;
      if (hp < 0 || hp == solved[i]) continue;
      bool ok = true;
      for (int e = 0; e < partners_len[i]; ++e) {
        int j = partners[i][e];
        if (solved[j] >= 0 &&
            !is_consonant(midi_to_freq(hp), midi_to_freq(solved[j]),
                          tolerance_cents, dissonance)) {
          ok = false;
          break;
        }
      }
      if (ok) solved[i] = hp;
    }
  }
  return round_from_pitches(solved, current);
}

Round transpose_round(const Round& r, int semitones) {
  Round out = r;
  for (int i = 0; i < out.n; ++i)
    if (!out.notes[i].is_rest()) out.notes[i].midi += semitones;
  return out;
}

Round fold_into_range(const Round& r, const int* scale, int scale_len) {
  Round out = r;
  const int* sc = scale != nullptr ? scale : default_scale();
  const int sl = scale != nullptr ? scale_len : default_scale_len();
  const int lo = sc[0], hi = sc[sl - 1];
  for (int i = 0; i < out.n; ++i) {
    int p = out.notes[i].midi;
    if (p >= 0) {
      while (p > hi) p -= 12;
      while (p < lo) p += 12;
      out.notes[i].midi = p;
    }
  }
  return out;
}

int note_at(const Round& r, float beat) {
  if (r.n == 0) return -1;
  const float total = r.total_beats();
  float tt = beat;
  if (tt < 0.0f || tt >= total) {
    int oct = static_cast<int>(tt / total);
    tt -= static_cast<float>(oct) * total;
    if (tt < 0.0f) tt += total;
  }
  float pos = 0.0f;
  for (int i = 0; i < r.n; ++i) {
    if (tt < pos + r.notes[i].beats) return i;
    pos += r.notes[i].beats;
  }
  return r.n - 1;
}

}  // namespace roundgen
