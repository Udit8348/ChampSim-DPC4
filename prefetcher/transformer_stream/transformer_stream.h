/*
 * Transformer-Aware Stream Prefetcher
 * 
 * Extension of Enhanced Stream Prefetcher optimized for Transformer/AI workloads.
 * 
 * Based on Liu et al., "Enhancements for Accurate and Timely Streaming Prefetcher,"
 * Journal of Instruction-Level Parallelism, Vol. 13, 2011.
 *
 * Extended with five transformer-specific enhancements:
 * 1. Multi-Stream Concurrency Awareness - Support multiple similar streams
 * 2. Stream Classification - Dense/Medium/Sparse based on stride and length
 * 3. Repetition-Aware Stream Reinforcement - Confidence boosting for patterns
 * 4. Phase-Aware Stream Throttling - Reduce aggressiveness during transitions
 * 5. Conservative Cross-Dimension Prefetching - Safe stride boundary handling
 *
 * DESIGN PHILOSOPHY:
 * - All inference is emergent from address behavior
 * - NO program counters, NO compiler hints, NO framework-specific info
 * - Backward compatible with general workloads
 * - Hardware-realistic implementation
 *
 * KEY INSIGHT:
 * Transformer memory accesses manifest as nested, repeating streams:
 * - Dense inner streams (head_dim traversal)
 * - Regular strided streams (token-to-token)
 * - Repeated patterns across layers
 * - Multiple concurrent similar streams (multi-head attention)
 */

#ifndef TRANSFORMER_STREAM_H
#define TRANSFORMER_STREAM_H

#include <array>
#include <cstdint>

#include "address.h"
#include "champsim.h"
#include "modules.h"

// =============================================================================
// Configuration Namespace - Transformer-Specific Parameters
// =============================================================================
namespace transformer_config {

// -----------------------------------------------------------------------------
// Base Stream Prefetcher Parameters (from enhanced_stream)
// -----------------------------------------------------------------------------
constexpr uint32_t TRAINING_TABLE_SIZE = 32;
constexpr uint32_t STREAM_TABLE_SIZE = 32;      // Increased for multi-head attention
constexpr uint32_t REGION_SIZE_BLOCKS = 4;
constexpr uint32_t CONFIRMATION_THRESHOLD = 3;
constexpr uint64_t DEAD_STREAM_THRESHOLD = 1000;
constexpr uint32_t SHORT_STREAM_THRESHOLD = 4;
constexpr uint32_t BASE_PREFETCH_DEGREE = 2;
constexpr uint64_t CLEANUP_INTERVAL = 256;

// -----------------------------------------------------------------------------
// Enhancement 1: Multi-Stream Grouping Parameters
// For multi-head attention, we need to track multiple similar concurrent streams
// -----------------------------------------------------------------------------
constexpr uint32_t MAX_STREAM_GROUPS = 8;        // Max distinct (stride,dir) combos
constexpr uint32_t MAX_STREAMS_PER_GROUP = 8;    // Max streams per group

// -----------------------------------------------------------------------------
// Enhancement 2: Stream Classification Thresholds
// Classify streams as dense/medium/sparse based on observed characteristics
// These thresholds are tuned for typical transformer memory patterns
// -----------------------------------------------------------------------------
constexpr int32_t DENSE_STRIDE_MAX = 2;          // stride ≤ 2 → head_dim-like
constexpr int32_t MEDIUM_STRIDE_MAX = 16;        // stride 3-16 → token-like
// stride > 16 → layer-like (sparse)

constexpr uint32_t DENSE_LENGTH_MIN = 8;         // Min prefetches for dense class
constexpr uint32_t MEDIUM_LENGTH_MIN = 4;        // Min prefetches for medium class

// Prefetch aggressiveness per class
constexpr uint32_t DENSE_PREFETCH_DEGREE = 4;    // Aggressive for dense streams
constexpr uint32_t MEDIUM_PREFETCH_DEGREE = 2;   // Moderate for medium streams
constexpr uint32_t SPARSE_PREFETCH_DEGREE = 1;   // Conservative for sparse streams

// -----------------------------------------------------------------------------
// Enhancement 3: Repetition-Aware Reinforcement Parameters
// Boost confidence for patterns that reappear within reuse window
// Models layer-to-layer repetition in transformers
// -----------------------------------------------------------------------------
constexpr uint32_t REUSE_WINDOW_SIZE = 2000;     // Timestamp window for pattern matching
constexpr uint32_t MAX_CONFIDENCE = 8;           // Maximum confidence level
constexpr uint32_t CONFIDENCE_BOOST_ON_REUSE = 2;// Confidence increment on reuse
constexpr uint32_t FAST_TRACK_CONFIDENCE = 4;    // Skip training if confidence >= this
constexpr uint32_t PATTERN_HISTORY_SIZE = 16;    // Remember last N terminated streams

// -----------------------------------------------------------------------------
// Enhancement 4: Phase-Aware Throttling Parameters
// Detect phase transitions (attention→MLP→norm) from stream behavior
// -----------------------------------------------------------------------------
constexpr uint32_t PHASE_WINDOW_SIZE = 64;       // Misses per window
constexpr uint32_t PHASE_TRANSITION_THRESHOLD = 4; // Terminations to trigger transition
constexpr uint32_t MIN_PREFETCH_DEGREE = 1;      // Minimum during transitions
constexpr uint32_t PHASE_RECOVERY_WINDOW = 32;   // Misses before recovery

// -----------------------------------------------------------------------------
// Enhancement 5: Cross-Dimension Prefetching Control
// Be conservative at stride boundaries to avoid crossing dimensions
// -----------------------------------------------------------------------------
constexpr uint32_t CONSERVATIVE_LOOKAHEAD = 1;   // Blocks ahead at boundaries
constexpr uint32_t AGGRESSIVE_LOOKAHEAD = 4;     // Blocks ahead in stable dense
constexpr uint32_t STRIDE_STABILITY_THRESHOLD = 3; // Consistent gaps needed

}  // namespace transformer_config

// =============================================================================
// Stream Direction (from base prefetcher)
// =============================================================================
enum class StreamDirection : int8_t {
    UNKNOWN = 0,
    POSITIVE = 1,
    NEGATIVE = -1
};

// =============================================================================
// Enhancement 2: Stream Classification
// Inferred from stride magnitude, stream length, and access density
// =============================================================================
enum class StreamClass : uint8_t {
    UNKNOWN = 0,
    DENSE,      // Small stride (≤2), frequent accesses → head_dim-like
    MEDIUM,     // Medium stride (3-16), regular gaps → token-like  
    SPARSE      // Large stride (>16), long reuse → layer-like
};

// =============================================================================
// Training Table Entry (extended from base)
// =============================================================================
struct TrainingEntry {
    bool valid = false;
    
    champsim::block_number region_base{};
    champsim::block_number last_miss_block{};
    champsim::block_number second_last_miss_block{};
    champsim::block_number third_last_miss_block{};
    
    uint32_t miss_count = 0;
    StreamDirection direction = StreamDirection::UNKNOWN;
    int32_t stride = 1;
    uint64_t last_access_timestamp = 0;
    
    // Enhancement 3: Pattern matching for fast-track
    uint32_t pattern_confidence = 0;
};

// =============================================================================
// Enhancement 1 & 2: Extended Stream Entry with Classification and Grouping
// =============================================================================
struct TransformerStreamEntry {
    // Base stream fields
    bool valid = false;
    bool active = true;
    
    champsim::block_number stream_start_block{};
    champsim::block_number stream_end_block{};
    champsim::block_number current_prefetch_block{};
    
    StreamDirection direction = StreamDirection::POSITIVE;
    int32_t stride = 1;
    
    uint64_t last_trigger_timestamp = 0;
    uint32_t stream_length = 0;
    
    // Enhancement 2: Stream Classification
    StreamClass stream_class = StreamClass::UNKNOWN;
    
    // Enhancement 3: Repetition tracking
    uint32_t reactivation_count = 0;     // Times this pattern reappeared
    uint32_t confidence_score = 1;       // Boosted on repetition (1-MAX_CONFIDENCE)
    
    // Density tracking for classification
    uint32_t accesses_in_window = 0;     // Access count for density calc
    uint64_t window_start_timestamp = 0; // Window start for density
    
    // Enhancement 1: Group membership
    int group_id = -1;                   // Index into stream_groups (-1 = ungrouped)
    
    // Enhancement 5: Stride stability tracking
    uint32_t consistent_stride_count = 0; // Consecutive accesses with same stride
};

// =============================================================================
// Enhancement 1: Stream Group
// Groups multiple concurrent streams with identical characteristics
// Critical for multi-head attention where identical streams run in parallel
// =============================================================================
struct StreamGroup {
    bool valid = false;
    
    int32_t stride = 0;
    StreamDirection direction = StreamDirection::UNKNOWN;
    
    uint32_t member_count = 0;           // Active streams in this group
    uint64_t group_confidence = 0;       // Reinforced on reappearance
    uint64_t last_seen_timestamp = 0;
    
    StreamClass typical_class = StreamClass::UNKNOWN;
    
    // Member stream indices (in stream_table)
    std::array<int, transformer_config::MAX_STREAMS_PER_GROUP> members;
    
    StreamGroup() { members.fill(-1); }
};

// =============================================================================
// Enhancement 4: Phase Detection State
// Tracks runtime behavior to detect phase transitions
// =============================================================================
struct PhaseState {
    uint64_t window_start_timestamp = 0;
    uint32_t streams_terminated_in_window = 0;
    uint32_t misses_in_window = 0;
    uint32_t successful_prefetches_in_window = 0;
    
    uint32_t current_prefetch_degree = transformer_config::BASE_PREFETCH_DEGREE;
    bool in_phase_transition = false;
    
    uint32_t recovery_counter = 0;       // Counts stable behavior after transition
};

// =============================================================================
// Enhancement 3: Pattern History Entry
// Remembers terminated stream characteristics for re-launch optimization
// =============================================================================
struct PatternHistoryEntry {
    bool valid = false;
    StreamDirection direction = StreamDirection::UNKNOWN;
    int32_t stride = 0;
    champsim::block_number region_base{};
    uint64_t termination_timestamp = 0;
    uint32_t stream_length = 0;          // How long was this stream
    StreamClass stream_class = StreamClass::UNKNOWN;
};

// =============================================================================
// Transformer-Aware Stream Prefetcher Class
// =============================================================================
struct transformer_stream : public champsim::modules::prefetcher {
private:
    // Training table (same as base)
    std::array<TrainingEntry, transformer_config::TRAINING_TABLE_SIZE> training_table{};
    
    // Extended stream table
    std::array<TransformerStreamEntry, transformer_config::STREAM_TABLE_SIZE> stream_table{};
    
    // Enhancement 1: Stream groups for multi-head attention
    std::array<StreamGroup, transformer_config::MAX_STREAM_GROUPS> stream_groups{};
    
    // Enhancement 3: Pattern history for re-launch optimization
    std::array<PatternHistoryEntry, transformer_config::PATTERN_HISTORY_SIZE> pattern_history{};
    uint32_t pattern_history_head = 0;   // Circular buffer index
    
    // Enhancement 4: Phase detection state
    PhaseState phase_state{};
    
    // Monotonic timestamp
    uint64_t current_timestamp = 0;
    uint64_t cleanup_counter = 0;
    
    // =========================================================================
    // Region Computation (from base)
    // =========================================================================
    champsim::block_number compute_region_base(champsim::block_number block) const;
    
    // =========================================================================
    // Training Table Operations (from base)
    // =========================================================================
    int find_training_entry(champsim::block_number region_base) const;
    int allocate_training_entry(champsim::block_number region_base);
    void update_training_entry(int idx, champsim::block_number miss_block);
    
    // =========================================================================
    // Direction and Stride Detection (from base, with noise filtering)
    // =========================================================================
    StreamDirection detect_direction(int64_t gap1, int64_t gap2) const;
    int32_t detect_stride(int64_t gap1, int64_t gap2) const;
    bool is_noise(int64_t gap1, int64_t gap2) const;
    
    // =========================================================================
    // Enhancement 1: Stream Grouping Operations
    // =========================================================================
    
    // Find existing group with matching characteristics
    int find_stream_group(StreamDirection dir, int32_t stride) const;
    
    // Create new group or return existing
    int find_or_create_stream_group(StreamDirection dir, int32_t stride);
    
    // Add stream to its matching group
    void add_stream_to_group(int stream_idx, int group_idx);
    
    // Remove stream from group (on termination)
    void remove_stream_from_group(int stream_idx);
    
    // Check if stream is protected by group membership
    bool is_group_protected(int stream_idx) const;
    
    // =========================================================================
    // Enhancement 2: Stream Classification
    // =========================================================================
    
    // Classify stream based on observed characteristics
    StreamClass classify_stream(const TransformerStreamEntry& entry) const;
    
    // Get prefetch degree for stream class
    uint32_t get_prefetch_degree_for_class(StreamClass cls) const;
    
    // Update stream classification as it evolves
    void update_stream_classification(int stream_idx);
    
    // =========================================================================
    // Enhancement 3: Repetition-Aware Reinforcement
    // =========================================================================
    
    // Record terminated stream pattern
    void record_pattern(const TransformerStreamEntry& entry);
    
    // Check if pattern exists in history
    int find_matching_pattern(StreamDirection dir, int32_t stride,
                              champsim::block_number region) const;
    
    // Get confidence boost from pattern history
    uint32_t get_pattern_confidence(StreamDirection dir, int32_t stride,
                                    champsim::block_number region) const;
    
    // Check if training can be fast-tracked
    bool can_fast_track_training(const TrainingEntry& entry) const;
    
    // Reinforce stream confidence on access
    void reinforce_stream_confidence(int stream_idx);
    
    // =========================================================================
    // Enhancement 4: Phase-Aware Throttling
    // =========================================================================
    
    // Update phase detection on each miss
    void update_phase_state(bool stream_terminated);
    
    // Check if in phase transition
    bool is_in_phase_transition() const;
    
    // Get current dynamic prefetch degree
    uint32_t get_current_prefetch_degree() const;
    
    // Try to recover from phase transition
    void try_phase_recovery();
    
    // =========================================================================
    // Enhancement 5: Cross-Dimension Prefetching Control
    // =========================================================================
    
    // Get safe lookahead distance based on stream stability
    uint32_t get_safe_lookahead(const TransformerStreamEntry& entry) const;
    
    // Check if prefetch crosses a natural boundary
    bool is_at_stride_boundary(const TransformerStreamEntry& entry) const;
    
    // =========================================================================
    // Stream Table Operations (extended from base)
    // =========================================================================
    
    int find_stream_for_block(champsim::block_number block) const;
    int find_matching_inactive_stream(StreamDirection dir, int32_t stride,
                                      champsim::block_number region_base) const;
    
    // Enhanced eviction with class/group/confidence awareness
    int allocate_stream_entry();
    int select_victim_stream() const;
    int compute_eviction_priority(int stream_idx) const;
    
    void create_stream(const TrainingEntry& trained_entry);
    void reactivate_stream(int idx, champsim::block_number trigger_block);
    bool try_relaunch_stream(champsim::block_number miss_block,
                            StreamDirection dir, int32_t stride);
    
    // =========================================================================
    // Prefetch Generation (enhanced with class/phase awareness)
    // =========================================================================
    void generate_prefetches(int stream_idx);
    
    // =========================================================================
    // Dead Stream Removal (enhanced with group awareness)
    // =========================================================================
    void remove_dead_streams();
    void terminate_stream(int stream_idx);

public:
    using champsim::modules::prefetcher::prefetcher;
    
    // =========================================================================
    // ChampSim Prefetcher Interface
    // =========================================================================
    void prefetcher_initialize();
    
    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                      uint8_t cache_hit, bool useful_prefetch,
                                      access_type type, uint32_t metadata_in);
    
    uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
                                   uint8_t prefetch, champsim::address evicted_addr,
                                   uint32_t metadata_in);
    
    void prefetcher_cycle_operate();
    
    void prefetcher_final_stats();
};

#endif  // TRANSFORMER_STREAM_H
