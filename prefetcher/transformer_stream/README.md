# Transformer-Aware Stream Prefetcher

Extension of the Enhanced Stream Prefetcher optimized for **Transformer/AI workloads**.

Based on Liu et al., "Enhancements for Accurate and Timely Streaming Prefetcher," JILP 2011,
with additional transformer-specific enhancements.

## Design Philosophy

All inference is **emergent from address behavior alone**:
- NO program counters
- NO compiler hints
- NO framework-specific information (PyTorch, TensorFlow, etc.)
- Hardware-realistic implementation
- Backward compatible with general workloads

## Key Insight

Transformer memory accesses manifest as **nested, repeating streams**:

```
base + layer_offset + head_offset + token_offset + head_dim_offset
```

At runtime, this appears as:
- Dense inner streams (head_dim traversal)
- Regular strided streams (token-to-token)
- Repeated patterns across layers
- Multiple concurrent similar streams (multi-head attention)

## Five Transformer-Specific Enhancements

### 1. Multi-Stream Concurrency Awareness

**Problem:** Multi-head attention creates multiple identical concurrent streams.

**Solution:**
- Group streams with identical (stride, direction) into `StreamGroup`
- Track member count per group
- Protect streams in large groups from eviction
- Prevents premature eviction during attention phases

```cpp
struct StreamGroup {
    int32_t stride;
    StreamDirection direction;
    uint32_t member_count;           // Streams in this group
    // ...
};
```

**Benefit:** Avoids stream table thrashing during multi-head attention.

---

### 2. Stream Classification

**Problem:** Different tensor dimensions require different prefetch aggressiveness.

**Solution:** Classify streams based on stride magnitude and length:

| Stride | Length | Class | Prefetch Degree |
|--------|--------|-------|-----------------|
| ≤2 | ≥8 | DENSE | 4 (aggressive) |
| 3-16 | ≥4 | MEDIUM | 2 (moderate) |
| >16 | any | SPARSE | 1 (conservative) |

**Benefit:** head_dim gets aggressive prefetching; layer traversal is conservative.

---

### 3. Repetition-Aware Stream Reinforcement

**Problem:** Transformers repeat identical patterns across layers.

**Solution:**
- Maintain pattern history of terminated streams
- When new stream matches recent pattern: boost confidence
- High-confidence patterns can skip training phase (fast-track)
- Reuse window prevents stale pattern matching

```cpp
if (pattern_confidence >= FAST_TRACK_CONFIDENCE) {
    // Skip 3-miss training, create stream immediately
}
```

**Benefit:** Faster stream creation for repeated layer patterns.

---

### 4. Phase-Aware Stream Throttling

**Problem:** Transformer phases (attention → MLP → norm) have different characteristics.

**Solution:**
- Track stream terminations per window
- When many streams terminate suddenly → phase transition detected
- Temporarily reduce prefetch degree
- Recover after stable behavior resumes

```cpp
if (streams_terminated_in_window > PHASE_TRANSITION_THRESHOLD) {
    in_phase_transition = true;
    current_prefetch_degree = MIN_PREFETCH_DEGREE;
}
```

**Benefit:** Avoids useless prefetches during phase transitions.

---

### 5. Conservative Cross-Dimension Prefetching

**Problem:** Prefetching into wrong tensor dimension wastes bandwidth.

**Solution:**
- Track stride consistency per stream
- Aggressive prefetching only after stable stride observed
- Conservative lookahead near stream boundaries
- Never speculate beyond observed stride stability

```cpp
if (consistent_stride_count >= STRIDE_STABILITY_THRESHOLD) {
    lookahead = AGGRESSIVE_LOOKAHEAD;  // 4 blocks
} else {
    lookahead = CONSERVATIVE_LOOKAHEAD;  // 1 block
}
```

**Benefit:** Prevents crossing from head_dim into token dimension incorrectly.

---

## Modified Eviction Policy

Priority order (highest = evict last):

```cpp
priority = base_priority(stream_class)    // DENSE=30, MEDIUM=20, SPARSE=10
         + confidence_score * 2           // Repetition boost
         + group_member_count * 3         // Multi-head protection
         + (active ? 10 : 0)              // Active stream boost
         - age_penalty                    // Older = less priority
```

**Eviction order:** Invalid → Dead → Sparse ungrouped → Medium → Dense with high confidence

---

## Configuration Parameters

```cpp
// Stream table (increased for multi-head)
constexpr uint32_t STREAM_TABLE_SIZE = 32;

// Multi-stream grouping
constexpr uint32_t MAX_STREAM_GROUPS = 8;
constexpr uint32_t MAX_STREAMS_PER_GROUP = 8;

// Classification thresholds
constexpr int32_t DENSE_STRIDE_MAX = 2;    // ≤2 blocks
constexpr int32_t MEDIUM_STRIDE_MAX = 16;  // 3-16 blocks

// Repetition parameters
constexpr uint32_t REUSE_WINDOW_SIZE = 2000;
constexpr uint32_t FAST_TRACK_CONFIDENCE = 4;

// Phase detection
constexpr uint32_t PHASE_TRANSITION_THRESHOLD = 4;
constexpr uint32_t PHASE_RECOVERY_WINDOW = 32;
```

---

## Paper Section Mapping

| Component | Base Paper | Transformer Extension |
|-----------|------------|----------------------|
| Training Table | §1.1 | + pattern confidence |
| Stream Table | §1.2 | + class, group, confidence |
| Direction Detection | §2 | Unchanged |
| Noise Filtering | §2 | Unchanged |
| Stream Confirmation | §2 | + fast-track |
| Prefetch Generation | §3 | + class/phase awareness |
| Re-launch | §4 | + confidence boost |
| Dead Stream Removal | §5 | + group protection |

---

## Separation of Generic vs AI-Specific Logic

| Component | Generic (Inherited) | AI-Specific (New) |
|-----------|---------------------|-------------------|
| Training | 3-miss confirmation | Fast-track for patterns |
| Streams | Single creation | Group-aware creation |
| Eviction | LRU | Class + group + confidence |
| Prefetch degree | Fixed | Dynamic per-class + phase |
| Dead removal | Age + length | + group membership |
| Re-launch | Stride/dir match | + confidence boost |

---

## Usage

### Build

```bash
# Create test configuration
# (Change L2C prefetcher to "transformer_stream")

./config.sh config/transformer_stream_test.json
make -j4
```

### Run

```bash
./bin/champsim_transformer_stream --warmup-instructions 10000000 \
    --simulation-instructions 50000000 traces/your_trace.champsim
```

### Expected Benefits vs Base Enhanced Stream

| Metric | Improvement |
|--------|-------------|
| Useful prefetches | ↑ 10-30% (AI workloads) |
| Useless prefetches | ↓ 20-40% (AI workloads) |
| Stream table thrashing | ↓ Significantly |
| General workloads | ≈ Same or slightly better |
