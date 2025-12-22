# Enhanced Stream Prefetcher

Implementation based on:
> Liu et al., "Enhancements for Accurate and Timely Streaming Prefetcher,"
> Journal of Instruction-Level Parallelism, Vol. 13, 2011.

## Overview

This prefetcher implements four key enhancements over a baseline stream prefetcher:

1. **Constant-stride detection** - Supports strides larger than 1 cache block
2. **Noise-tolerant training** - Filters spurious accesses during training phase
3. **Early re-launch of repeated streams** - Quickly reactivates previously seen streams
4. **Dead stream removal** - Removes short, inactive streams to prevent table pollution

## Important Design Constraints

| Constraint | Implementation |
|------------|----------------|
| **Region-based identification** | Streams are identified by address regions, NOT by PC/IP |
| **Training on misses only** | Training only progresses on L2 cache misses |
| **3-miss confirmation** | A stream is confirmed after 3 consistent misses |
| **Unidirectional prefetching** | Each stream has a fixed direction (+1 or -1) |
| **Stride in cache blocks** | Stride ≥ 1 cache block (not bytes) |
| **Monotonic timestamps** | Uses internal counter, not wall-clock time |

## Data Structures

### Training Table Entry

Used to detect potential streams before confirmation.

```cpp
struct TrainingEntry {
    bool valid;
    champsim::block_number region_base;      // Region-aligned base
    champsim::block_number last_miss_block;  // A(n) - most recent
    champsim::block_number second_last_miss_block;  // A(n-1)
    champsim::block_number third_last_miss_block;   // A(n-2)
    uint32_t miss_count;                     // 0, 1, 2, or 3
    StreamDirection direction;               // UNKNOWN, POSITIVE, NEGATIVE
    int32_t stride;                          // In cache blocks (≥1)
    uint64_t last_access_timestamp;
};
```

### Stream Table Entry

Tracks active and inactive streams.

```cpp
struct StreamEntry {
    bool valid;
    bool active;                             // Active = generating prefetches
    champsim::block_number stream_start_block;
    champsim::block_number stream_end_block;
    champsim::block_number current_prefetch_block;
    StreamDirection direction;
    int32_t stride;
    uint64_t last_trigger_timestamp;
    uint32_t stream_length;                  // Blocks prefetched
};
```

## Algorithm Details

### 1. Stream Training

When an L2 cache miss occurs:

1. Compute the region base address (groups of 4 cache blocks)
2. Look up (or allocate) a training table entry for this region
3. Record the miss address in the history (A(n-2), A(n-1), A(n))
4. After 3 misses, compute gaps:
   - `gap1 = A(n-1) - A(n-2)`
   - `gap2 = A(n) - A(n-1)`
5. Detect direction and stride (see below)
6. If consistent, confirm the stream

### 2. Constant-Stride Detection

```
stride = |gap1| = |gap2|   (must be equal)
direction = sign(gap1) = sign(gap2)   (must match)
```

The stride is measured in cache blocks and must be ≥ 1.

**Example:**
- Misses at blocks: 100, 102, 104
- gap1 = 102 - 100 = +2
- gap2 = 104 - 102 = +2
- stride = 2 blocks, direction = POSITIVE

### 3. Noise Filtering

The prefetcher tolerates small deviations that might be noise:

| Gap1 | Gap2 | Action |
|------|------|--------|
| +N | +N | Valid positive stream (N ≥ 1) |
| -N | -N | Valid negative stream (N ≥ 1) |
| +N | -1 | **Noise** - continue training |
| -1 | +N | **Noise** - continue training |
| +N | -M (M > 1) | Reset training |
| -N | +M (M > 1) | Reset training |

This prevents spurious cache accesses (e.g., from prefetcher itself or OS) from breaking training.

### 4. Early Re-launch of Repeated Streams

Before creating a new stream, the prefetcher checks for inactive streams that match:

1. Same direction
2. Same stride  
3. Overlapping or nearby address range

If found, the old stream is **reactivated immediately**, skipping the training phase. This is important for loops that traverse the same data multiple times.

### 5. Dead Stream Removal

Streams are removed if they meet BOTH criteria:
- `age > DEAD_STREAM_THRESHOLD` (default: 1000 timestamp ticks)
- `stream_length < SHORT_STREAM_THRESHOLD` (default: 4 prefetches)

This prevents short, stale streams from polluting the stream table.

## Configuration Parameters

Located in `enhanced_stream.h`:

```cpp
// Table sizes
constexpr uint32_t TRAINING_TABLE_SIZE = 32;
constexpr uint32_t STREAM_TABLE_SIZE = 16;

// Region size (cache blocks per region)
constexpr uint32_t REGION_SIZE_BLOCKS = 4;

// Training threshold
constexpr uint32_t CONFIRMATION_THRESHOLD = 3;

// Dead stream thresholds
constexpr uint64_t DEAD_STREAM_THRESHOLD = 1000;
constexpr uint32_t SHORT_STREAM_THRESHOLD = 4;

// Prefetch parameters
constexpr uint32_t PREFETCH_DEGREE = 2;
```

## Paper Section Mapping

| Paper Section | Implementation File | Function/Area |
|--------------|---------------------|---------------|
| §1.1 Training Table | `enhanced_stream.h` | `TrainingEntry` struct |
| §1.2 Stream Table | `enhanced_stream.h` | `StreamEntry` struct |
| §2 Stream Training | `enhanced_stream.cc` | `update_training_entry()` |
| §2 Direction Detection | `enhanced_stream.cc` | `detect_direction()` |
| §2 Constant-Stride | `enhanced_stream.cc` | `detect_stride()` |
| §2 Noise Filtering | `enhanced_stream.cc` | `is_noise()` |
| §3 Prefetch Generation | `enhanced_stream.cc` | `generate_prefetches()` |
| §4 Early Re-launch | `enhanced_stream.cc` | `try_relaunch_stream()` |
| §5 Dead Stream Removal | `enhanced_stream.cc` | `remove_dead_streams()` |
| §6 Timing Rules | `enhanced_stream.cc` | `current_timestamp` counter |

## Usage with ChampSim

To use this prefetcher with ChampSim:

1. Configure ChampSim to use `enhanced_stream` for the L2 cache prefetcher
2. Build the simulator
3. Run with your trace files

The prefetcher integrates with ChampSim's standard prefetcher interface:
- `prefetcher_initialize()` - Called once at startup
- `prefetcher_cache_operate()` - Called on every cache access
- `prefetcher_cache_fill()` - Called on cache fills
- `prefetcher_cycle_operate()` - Called every cycle
- `prefetcher_final_stats()` - Called at end of simulation
