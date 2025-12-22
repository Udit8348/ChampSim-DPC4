/*
 * Enhanced Stream Prefetcher
 * 
 * Implementation based on:
 * Liu et al., "Enhancements for Accurate and Timely Streaming Prefetcher,"
 * Journal of Instruction-Level Parallelism, Vol. 13, 2011.
 *
 * This prefetcher implements four key enhancements:
 * 1. Constant-stride detection - Supports strides > 1 cache block
 * 2. Noise-tolerant training - Filters spurious accesses during training
 * 3. Early re-launch of repeated streams - Reactivates previously seen streams
 * 4. Dead stream removal - Removes short, inactive streams
 *
 * IMPORTANT CONSTRAINTS:
 * - This is a REGION-BASED prefetcher, NOT PC-based
 * - Training happens on CACHE MISSES ONLY
 * - Streams are confirmed after 3 consecutive consistent misses
 * - Prefetching is UNIDIRECTIONAL per stream
 * - Stride is measured in cache blocks (≥1)
 */

#ifndef ENHANCED_STREAM_H
#define ENHANCED_STREAM_H

#include <array>
#include <cstdint>

#include "address.h"
#include "champsim.h"
#include "modules.h"

namespace enhanced_stream_config {

// =============================================================================
// Table Size Configuration
// =============================================================================
constexpr uint32_t TRAINING_TABLE_SIZE = 32;  // Number of training entries
constexpr uint32_t STREAM_TABLE_SIZE = 16;    // Number of active/inactive streams

// =============================================================================
// Region Configuration (Paper Section: Training Table)
// A region groups multiple cache blocks for training purposes
// =============================================================================
constexpr uint32_t REGION_SIZE_BLOCKS = 4;    // Cache blocks per region

// =============================================================================
// Training Thresholds (Paper Section: Stream Training Logic)
// =============================================================================
constexpr uint32_t CONFIRMATION_THRESHOLD = 3;  // Misses needed to confirm stream

// =============================================================================
// Dead Stream Removal Thresholds (Paper Section: Dead Stream Removal)
// =============================================================================
constexpr uint64_t DEAD_STREAM_THRESHOLD = 1000;   // Timestamp age before dead
constexpr uint32_t SHORT_STREAM_THRESHOLD = 4;     // Min prefetches for valid stream

// =============================================================================
// Prefetch Parameters (Paper Section: Prefetch Generation Logic)
// Conservative settings to avoid pollution
// =============================================================================
constexpr uint32_t PREFETCH_DEGREE = 2;  // Lines to prefetch ahead

// =============================================================================
// Dead Stream Cleanup Interval
// =============================================================================
constexpr uint64_t CLEANUP_INTERVAL = 256;  // Check for dead streams every N misses

}  // namespace enhanced_stream_config

// =============================================================================
// Direction Enumeration
// UNKNOWN: Initial state before direction is determined
// POSITIVE: Stream progresses to higher addresses
// NEGATIVE: Stream progresses to lower addresses
// =============================================================================
enum class StreamDirection : int8_t {
    UNKNOWN = 0,
    POSITIVE = 1,
    NEGATIVE = -1
};

// =============================================================================
// Training Table Entry (Paper Section 1.1: Training Table)
// Used to detect potential streams before they are confirmed.
// =============================================================================
struct TrainingEntry {
    bool valid = false;
    
    // Region-aligned base address (identifies the training region)
    champsim::block_number region_base{};
    
    // Miss history for direction and stride detection
    champsim::block_number last_miss_block{};        // Most recent miss (A_n)
    champsim::block_number second_last_miss_block{}; // Second most recent (A_n-1)
    champsim::block_number third_last_miss_block{};  // Third most recent (A_n-2)
    
    // Training state
    uint32_t miss_count = 0;                         // Progress toward confirmation (max 3)
    StreamDirection direction = StreamDirection::UNKNOWN;
    int32_t stride = 1;                              // Stride in cache blocks (≥1)
    
    // Timestamp for LRU replacement
    uint64_t last_access_timestamp = 0;
};

// =============================================================================
// Stream Table Entry (Paper Section 1.2: Stream Table)
// Tracks active and inactive (dormant) streams for prefetching and re-launch.
// =============================================================================
struct StreamEntry {
    bool valid = false;
    bool active = true;         // Active streams generate prefetches
    
    // Stream boundaries (in block numbers)
    champsim::block_number stream_start_block{};
    champsim::block_number stream_end_block{};
    
    // Current prefetch position
    champsim::block_number current_prefetch_block{};
    
    // Stream characteristics (fixed at creation)
    StreamDirection direction = StreamDirection::POSITIVE;
    int32_t stride = 1;         // Stride in blocks
    
    // Timing for dead stream detection
    uint64_t last_trigger_timestamp = 0;
    
    // Statistics for dead stream removal
    uint32_t stream_length = 0; // Number of blocks prefetched so far
};

// =============================================================================
// Enhanced Stream Prefetcher Class
// =============================================================================
struct enhanced_stream : public champsim::modules::prefetcher {
private:
    // Training table: detects potential streams from miss sequences
    std::array<TrainingEntry, enhanced_stream_config::TRAINING_TABLE_SIZE> training_table{};
    
    // Stream table: tracks active and inactive streams
    std::array<StreamEntry, enhanced_stream_config::STREAM_TABLE_SIZE> stream_table{};
    
    // Monotonic timestamp counter (incremented on each miss)
    // Paper Section 6: Use monotonic timestamp, NOT wall-clock time
    uint64_t current_timestamp = 0;
    
    // Counter for periodic dead stream cleanup
    uint64_t cleanup_counter = 0;
    
    // =========================================================================
    // Region Computation
    // =========================================================================
    
    // Compute the region base block number for a given address
    champsim::block_number compute_region_base(champsim::block_number block) const;
    
    // =========================================================================
    // Training Table Operations
    // =========================================================================
    
    // Find existing training entry for a region (-1 if not found)
    int find_training_entry(champsim::block_number region_base) const;
    
    // Allocate a new training entry (evicts LRU if full)
    int allocate_training_entry(champsim::block_number region_base);
    
    // Update training entry with new miss information
    void update_training_entry(int idx, champsim::block_number miss_block);
    
    // =========================================================================
    // Direction and Stride Detection
    // Paper Sections: Direction Detection, Constant-Stride Detection
    // =========================================================================
    
    // Detect direction with noise filtering
    // Returns UNKNOWN if noisy or inconsistent
    StreamDirection detect_direction(int64_t gap1, int64_t gap2) const;
    
    // Detect stride from two gaps
    // Returns stride in blocks (≥1), or 0 if inconsistent
    int32_t detect_stride(int64_t gap1, int64_t gap2) const;
    
    // Check if a gap is noise (small deviation in opposite direction)
    bool is_noise(int64_t gap1, int64_t gap2) const;
    
    // =========================================================================
    // Stream Table Operations
    // =========================================================================
    
    // Find stream that covers the given block address (-1 if not found)
    int find_stream_for_block(champsim::block_number block) const;
    
    // Find inactive stream matching direction/stride near the region
    // For early re-launch (Enhancement 3)
    int find_matching_inactive_stream(StreamDirection dir, int32_t stride,
                                      champsim::block_number region_base) const;
    
    // Allocate new stream entry (evicts LRU if full)
    int allocate_stream_entry();
    
    // Create new stream from confirmed training entry
    void create_stream(const TrainingEntry& trained_entry);
    
    // Reactivate dormant stream (early re-launch)
    void reactivate_stream(int idx, champsim::block_number trigger_block);
    
    // =========================================================================
    // Prefetch Generation (Paper Section 3)
    // =========================================================================
    
    // Generate prefetches for an active stream
    void generate_prefetches(int stream_idx);
    
    // =========================================================================
    // Dead Stream Removal (Paper Section 5)
    // =========================================================================
    
    // Remove dead streams from the table
    void remove_dead_streams();
    
    // =========================================================================
    // Early Re-launch (Paper Section 4)
    // =========================================================================
    
    // Try to re-launch a matching inactive stream
    // Returns true if re-launched, false if new stream should be created
    bool try_relaunch_stream(champsim::block_number miss_block,
                            StreamDirection dir, int32_t stride);

public:
    // Use parent class constructor
    using champsim::modules::prefetcher::prefetcher;
    
    // =========================================================================
    // ChampSim Prefetcher Interface
    // =========================================================================
    
    // Called once at initialization
    void prefetcher_initialize();
    
    // Called on every cache access (hit or miss)
    // Paper: Training happens on MISSES ONLY
    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                      uint8_t cache_hit, bool useful_prefetch,
                                      access_type type, uint32_t metadata_in);
    
    // Called when a line fills the cache
    uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
                                   uint8_t prefetch, champsim::address evicted_addr,
                                   uint32_t metadata_in);
    
    // Called every cycle for background operations
    void prefetcher_cycle_operate();
    
    // Called at end of simulation for statistics
    void prefetcher_final_stats();
};

#endif  // ENHANCED_STREAM_H
