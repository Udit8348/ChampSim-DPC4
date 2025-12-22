/*
 * Enhanced Stream Prefetcher - Implementation
 * 
 * Based on Liu et al., "Enhancements for Accurate and Timely Streaming Prefetcher,"
 * Journal of Instruction-Level Parallelism, Vol. 13, 2011.
 *
 * This file implements all four enhancements:
 * 1. Constant-stride detection
 * 2. Noise-tolerant training
 * 3. Early re-launch of repeated streams
 * 4. Dead stream removal
 */

#include "enhanced_stream.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cache.h"

using namespace enhanced_stream_config;

// =============================================================================
// Initialization
// =============================================================================

void enhanced_stream::prefetcher_initialize()
{
    // Initialize all training table entries as invalid
    for (auto& entry : training_table) {
        entry.valid = false;
        entry.miss_count = 0;
        entry.direction = StreamDirection::UNKNOWN;
        entry.stride = 1;
        entry.last_access_timestamp = 0;
    }
    
    // Initialize all stream table entries as invalid
    for (auto& entry : stream_table) {
        entry.valid = false;
        entry.active = false;
        entry.stream_length = 0;
        entry.last_trigger_timestamp = 0;
    }
    
    current_timestamp = 0;
    cleanup_counter = 0;
}

// =============================================================================
// Region Computation
// Paper Section 1.1: A region groups multiple cache blocks
// =============================================================================

champsim::block_number enhanced_stream::compute_region_base(champsim::block_number block) const
{
    // Region base is the block number rounded down to region boundary
    // region_base = block & ~(REGION_SIZE_BLOCKS - 1)
    uint64_t block_val = block.to<uint64_t>();
    uint64_t region_mask = ~(static_cast<uint64_t>(REGION_SIZE_BLOCKS) - 1);
    return champsim::block_number{block_val & region_mask};
}

// =============================================================================
// Training Table Operations
// Paper Section 1.1: Training Table
// =============================================================================

int enhanced_stream::find_training_entry(champsim::block_number region_base) const
{
    for (std::size_t i = 0; i < training_table.size(); ++i) {
        if (training_table[i].valid && training_table[i].region_base == region_base) {
            return static_cast<int>(i);
        }
    }
    return -1;  // Not found
}

int enhanced_stream::allocate_training_entry(champsim::block_number region_base)
{
    // First, try to find an invalid entry
    for (std::size_t i = 0; i < training_table.size(); ++i) {
        if (!training_table[i].valid) {
            training_table[i].valid = true;
            training_table[i].region_base = region_base;
            training_table[i].miss_count = 0;
            training_table[i].direction = StreamDirection::UNKNOWN;
            training_table[i].stride = 1;
            training_table[i].last_access_timestamp = current_timestamp;
            return static_cast<int>(i);
        }
    }
    
    // All entries valid, evict LRU (oldest timestamp)
    std::size_t lru_idx = 0;
    uint64_t oldest_time = std::numeric_limits<uint64_t>::max();
    
    for (std::size_t i = 0; i < training_table.size(); ++i) {
        if (training_table[i].last_access_timestamp < oldest_time) {
            oldest_time = training_table[i].last_access_timestamp;
            lru_idx = i;
        }
    }
    
    // Reset the entry
    training_table[lru_idx].valid = true;
    training_table[lru_idx].region_base = region_base;
    training_table[lru_idx].miss_count = 0;
    training_table[lru_idx].direction = StreamDirection::UNKNOWN;
    training_table[lru_idx].stride = 1;
    training_table[lru_idx].last_access_timestamp = current_timestamp;
    
    return static_cast<int>(lru_idx);
}

void enhanced_stream::update_training_entry(int idx, champsim::block_number miss_block)
{
    TrainingEntry& entry = training_table[idx];
    entry.last_access_timestamp = current_timestamp;
    
    if (entry.miss_count == 0) {
        // First miss in this training sequence
        entry.last_miss_block = miss_block;
        entry.miss_count = 1;
        return;
    }
    
    if (entry.miss_count == 1) {
        // Second miss - shift history and record
        entry.second_last_miss_block = entry.last_miss_block;
        entry.last_miss_block = miss_block;
        entry.miss_count = 2;
        return;
    }
    
    // Third or subsequent miss - compute gaps and detect direction/stride
    // Shift the history
    entry.third_last_miss_block = entry.second_last_miss_block;
    entry.second_last_miss_block = entry.last_miss_block;
    entry.last_miss_block = miss_block;
    
    // Compute gaps between consecutive misses
    // gap1 = A(n-1) - A(n-2)
    // gap2 = A(n) - A(n-1)
    int64_t gap1 = champsim::offset(entry.third_last_miss_block, entry.second_last_miss_block);
    int64_t gap2 = champsim::offset(entry.second_last_miss_block, entry.last_miss_block);
    
    // Paper Section: Noise-Tolerant Training
    // Check if this is noise - if so, don't reset, just continue
    if (is_noise(gap1, gap2)) {
        // Noise detected - do NOT reset training, continue accumulating
        // The noise will be filtered out
        return;
    }
    
    // Paper Section: Direction Detection
    StreamDirection detected_dir = detect_direction(gap1, gap2);
    
    if (detected_dir == StreamDirection::UNKNOWN) {
        // Large inconsistent jumps - reset training
        entry.miss_count = 1;
        entry.direction = StreamDirection::UNKNOWN;
        entry.stride = 1;
        return;
    }
    
    // Paper Section: Constant-Stride Detection
    int32_t detected_stride = detect_stride(gap1, gap2);
    
    if (detected_stride <= 0) {
        // Inconsistent strides - reset training
        entry.miss_count = 1;
        entry.direction = StreamDirection::UNKNOWN;
        entry.stride = 1;
        return;
    }
    
    // Valid direction and stride detected!
    entry.direction = detected_dir;
    entry.stride = detected_stride;
    entry.miss_count = 3;  // Ready for stream confirmation
}

// =============================================================================
// Direction and Stride Detection
// Paper Sections: Direction Detection, Constant-Stride Detection, Noise Filtering
// =============================================================================

bool enhanced_stream::is_noise(int64_t gap1, int64_t gap2) const
{
    // Paper Section: Noise-Tolerant Training
    // Noise: ±1 block deviation in opposite direction
    // Example: gap1 = +3, gap2 = -1 → might be noise
    // Example: gap1 = -1, gap2 = +3 → might be noise
    
    // Check if one gap is ±1 and the other has opposite sign
    if ((gap1 == 1 && gap2 < 0) || (gap1 == -1 && gap2 > 0) ||
        (gap2 == 1 && gap1 < 0) || (gap2 == -1 && gap1 > 0)) {
        // But the large gap should be > 1 for this to be noise filtering
        if (std::abs(gap1) <= 1 || std::abs(gap2) <= 1) {
            return true;  // This is noise
        }
    }
    
    return false;
}

StreamDirection enhanced_stream::detect_direction(int64_t gap1, int64_t gap2) const
{
    // Paper Section: Direction Detection
    // If both gaps have the same sign → direction is valid
    
    if (gap1 > 0 && gap2 > 0) {
        return StreamDirection::POSITIVE;
    }
    
    if (gap1 < 0 && gap2 < 0) {
        return StreamDirection::NEGATIVE;
    }
    
    // Mixed signs (not noise) → unknown/inconsistent
    return StreamDirection::UNKNOWN;
}

int32_t enhanced_stream::detect_stride(int64_t gap1, int64_t gap2) const
{
    // Paper Section: Constant-Stride Detection
    // stride = |gap| (in cache blocks)
    // Must be consistent for 2 consecutive gaps
    
    int64_t abs_gap1 = std::abs(gap1);
    int64_t abs_gap2 = std::abs(gap2);
    
    // Check if strides are consistent (equal magnitudes)
    if (abs_gap1 != abs_gap2) {
        return 0;  // Inconsistent strides
    }
    
    // Stride must be at least 1
    if (abs_gap1 < 1) {
        return 0;  // Invalid stride
    }
    
    return static_cast<int32_t>(abs_gap1);
}

// =============================================================================
// Stream Table Operations
// Paper Section 1.2: Stream Table
// =============================================================================

int enhanced_stream::find_stream_for_block(champsim::block_number block) const
{
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) continue;
        
        const StreamEntry& entry = stream_table[i];
        
        // Check if block is within stream range
        // For positive direction: start <= block
        // For negative direction: block <= start
        if (entry.direction == StreamDirection::POSITIVE) {
            if (block >= entry.stream_start_block && block <= entry.current_prefetch_block) {
                return static_cast<int>(i);
            }
        } else {
            if (block <= entry.stream_start_block && block >= entry.current_prefetch_block) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;  // Not found
}

int enhanced_stream::find_matching_inactive_stream(StreamDirection dir, int32_t stride,
                                                    champsim::block_number region_base) const
{
    // Paper Section 4: Early Launch of Repeated Streams
    // Find inactive stream with same direction, same stride, overlapping region
    
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) continue;
        if (stream_table[i].active) continue;  // Only look at inactive streams
        
        const StreamEntry& entry = stream_table[i];
        
        // Check direction and stride match
        if (entry.direction != dir || entry.stride != stride) continue;
        
        // Check for overlapping address range
        // Compute region base of the stream's start
        champsim::block_number stream_region = compute_region_base(entry.stream_start_block);
        
        // Check if regions overlap or are adjacent
        int64_t region_diff = std::abs(champsim::offset(region_base, stream_region));
        if (region_diff <= REGION_SIZE_BLOCKS * 2) {
            return static_cast<int>(i);  // Found matching inactive stream
        }
    }
    return -1;  // No match found
}

int enhanced_stream::allocate_stream_entry()
{
    // First, try to find an invalid entry
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) {
            return static_cast<int>(i);
        }
    }
    
    // Try to evict a dead stream first
    remove_dead_streams();
    
    // Check again for invalid entry
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) {
            return static_cast<int>(i);
        }
    }
    
    // All entries valid, evict LRU inactive stream
    std::size_t lru_idx = 0;
    uint64_t oldest_time = std::numeric_limits<uint64_t>::max();
    bool found_inactive = false;
    
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].active) {
            if (stream_table[i].last_trigger_timestamp < oldest_time) {
                oldest_time = stream_table[i].last_trigger_timestamp;
                lru_idx = i;
                found_inactive = true;
            }
        }
    }
    
    if (found_inactive) {
        stream_table[lru_idx].valid = false;
        return static_cast<int>(lru_idx);
    }
    
    // All streams are active - evict overall LRU
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (stream_table[i].last_trigger_timestamp < oldest_time) {
            oldest_time = stream_table[i].last_trigger_timestamp;
            lru_idx = i;
        }
    }
    
    stream_table[lru_idx].valid = false;
    return static_cast<int>(lru_idx);
}

void enhanced_stream::create_stream(const TrainingEntry& trained_entry)
{
    int idx = allocate_stream_entry();
    if (idx < 0) return;  // Could not allocate
    
    StreamEntry& entry = stream_table[idx];
    
    entry.valid = true;
    entry.active = true;
    entry.direction = trained_entry.direction;
    entry.stride = trained_entry.stride;
    entry.last_trigger_timestamp = current_timestamp;
    entry.stream_length = 0;
    
    // Set stream start and initial prefetch position
    entry.stream_start_block = trained_entry.last_miss_block;
    entry.current_prefetch_block = trained_entry.last_miss_block;
    
    // Set stream end boundary (use a reasonable lookahead distance)
    // We'll limit prefetching to stay within a reasonable range
    int64_t dir_val = static_cast<int64_t>(trained_entry.direction);
    int64_t end_offset = dir_val * trained_entry.stride * 64;  // Up to 64 blocks ahead
    
    if (trained_entry.direction == StreamDirection::POSITIVE) {
        entry.stream_end_block = trained_entry.last_miss_block + end_offset;
    } else {
        entry.stream_end_block = trained_entry.last_miss_block + end_offset;
    }
    
    // Immediately generate prefetches for the new stream
    // Paper Section 2: Launch prefetching immediately
    generate_prefetches(idx);
}

void enhanced_stream::reactivate_stream(int idx, champsim::block_number trigger_block)
{
    // Paper Section 4: Early Re-launch
    // Reactivate the stream and update its position
    
    StreamEntry& entry = stream_table[idx];
    
    entry.active = true;
    entry.last_trigger_timestamp = current_timestamp;
    
    // Update the current prefetch position to start from the trigger
    entry.current_prefetch_block = trigger_block;
    
    // Extend the stream end if necessary
    int64_t dir_val = static_cast<int64_t>(entry.direction);
    int64_t end_offset = dir_val * entry.stride * 64;
    
    if (entry.direction == StreamDirection::POSITIVE) {
        champsim::block_number new_end = trigger_block + end_offset;
        if (new_end > entry.stream_end_block) {
            entry.stream_end_block = new_end;
        }
    } else {
        champsim::block_number new_end = trigger_block + end_offset;
        if (new_end < entry.stream_end_block) {
            entry.stream_end_block = new_end;
        }
    }
    
    // Generate prefetches immediately
    generate_prefetches(idx);
}

bool enhanced_stream::try_relaunch_stream(champsim::block_number miss_block,
                                          StreamDirection dir, int32_t stride)
{
    // Paper Section 4: Early Launch of Repeated Streams
    // Search for inactive stream with matching characteristics
    
    champsim::block_number region = compute_region_base(miss_block);
    int match_idx = find_matching_inactive_stream(dir, stride, region);
    
    if (match_idx >= 0) {
        // Found a matching inactive stream - reactivate it
        reactivate_stream(match_idx, miss_block);
        return true;
    }
    
    return false;  // No match, need to create new stream
}

// =============================================================================
// Prefetch Generation
// Paper Section 3: Prefetch Generation Logic
// =============================================================================

void enhanced_stream::generate_prefetches(int stream_idx)
{
    StreamEntry& entry = stream_table[stream_idx];
    
    if (!entry.valid || !entry.active) return;
    
    int64_t dir_val = static_cast<int64_t>(entry.direction);
    
    // Generate PREFETCH_DEGREE prefetches
    for (uint32_t i = 0; i < PREFETCH_DEGREE; ++i) {
        // Compute next prefetch address
        // next_addr = current_addr + direction × stride
        champsim::block_number next_block = entry.current_prefetch_block + (dir_val * entry.stride);
        
        // Check if we've exceeded stream bounds
        if (entry.direction == StreamDirection::POSITIVE) {
            if (next_block > entry.stream_end_block) {
                // Reached end of stream - mark as inactive
                entry.active = false;
                return;
            }
        } else {
            if (next_block < entry.stream_end_block) {
                // Reached end of stream - mark as inactive
                entry.active = false;
                return;
            }
        }
        
        // Check MSHR occupancy before prefetching
        // Paper Section 3: Stop if MSHR/prefetch queue is full
        double mshr_ratio = intern_->get_mshr_occupancy_ratio();
        if (mshr_ratio > 0.75) {
            // MSHR is getting full, stop prefetching for now
            return;
        }
        
        // Convert block number to address and issue prefetch
        champsim::address pf_addr{next_block};
        
        // Check if we should fill this level or just lower levels
        // Use conservative policy: fill this level when MSHR is light
        bool fill_this_level = (mshr_ratio < 0.5);
        
        bool success = prefetch_line(pf_addr, fill_this_level, 0);
        
        if (success) {
            // Update prefetch position
            entry.current_prefetch_block = next_block;
            entry.stream_length++;
        } else {
            // Prefetch failed (likely queue full) - try again next cycle
            return;
        }
    }
    
    // Update timestamp after successful prefetching
    entry.last_trigger_timestamp = current_timestamp;
}

// =============================================================================
// Dead Stream Removal
// Paper Section 5: Dead Stream Removal
// =============================================================================

void enhanced_stream::remove_dead_streams()
{
    // Paper Section 5: Dead Stream Removal
    // Remove streams if:
    // - age > DEAD_THRESHOLD
    // - AND stream_length < SHORT_STREAM_THRESHOLD
    
    for (auto& entry : stream_table) {
        if (!entry.valid) continue;
        
        // Compute stream age
        uint64_t age = current_timestamp - entry.last_trigger_timestamp;
        
        // Check dead stream criteria
        if (age > DEAD_STREAM_THRESHOLD && entry.stream_length < SHORT_STREAM_THRESHOLD) {
            // This is a dead stream - remove it
            entry.valid = false;
            entry.active = false;
        }
    }
}

// =============================================================================
// Main Prefetcher Interface
// =============================================================================

uint32_t enhanced_stream::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                                    uint8_t cache_hit, bool useful_prefetch,
                                                    access_type type, uint32_t metadata_in)
{
    // Paper: Training happens on CACHE MISSES ONLY
    // Note: cache_hit == 0 means miss, cache_hit != 0 means hit
    if (cache_hit) {
        return metadata_in;  // Do nothing on cache hits
    }
    
    // Increment monotonic timestamp on each miss
    // Paper Section 6: Use monotonic timestamp
    current_timestamp++;
    
    // Periodically remove dead streams
    cleanup_counter++;
    if (cleanup_counter >= CLEANUP_INTERVAL) {
        remove_dead_streams();
        cleanup_counter = 0;
    }
    
    // Convert address to block number
    champsim::block_number miss_block{addr};
    champsim::block_number region_base = compute_region_base(miss_block);
    
    // =========================================================================
    // Step 1: Check if this miss triggers an existing stream
    // =========================================================================
    int stream_idx = find_stream_for_block(miss_block);
    if (stream_idx >= 0) {
        // This miss is within an existing stream
        StreamEntry& entry = stream_table[stream_idx];
        entry.last_trigger_timestamp = current_timestamp;
        
        if (!entry.active) {
            // Reactivate dormant stream
            entry.active = true;
        }
        
        // Advance prefetch window
        generate_prefetches(stream_idx);
        
        return metadata_in;
    }
    
    // =========================================================================
    // Step 2: Training phase - look up or allocate training entry
    // =========================================================================
    int train_idx = find_training_entry(region_base);
    
    if (train_idx < 0) {
        // No existing training entry - allocate new one
        train_idx = allocate_training_entry(region_base);
    }
    
    // Update training with this miss
    update_training_entry(train_idx, miss_block);
    
    // =========================================================================
    // Step 3: Check if training is complete (3 consistent misses)
    // Paper Section 2: Stream Confirmation
    // =========================================================================
    if (training_table[train_idx].miss_count >= CONFIRMATION_THRESHOLD) {
        const TrainingEntry& trained = training_table[train_idx];
        
        // Only proceed if we have valid direction and stride
        if (trained.direction != StreamDirection::UNKNOWN && trained.stride >= 1) {
            // Paper Section 4: Early Re-launch
            // Try to re-launch a matching inactive stream first
            if (!try_relaunch_stream(miss_block, trained.direction, trained.stride)) {
                // No matching stream found - create new one
                create_stream(trained);
            }
        }
        
        // Remove the training entry (stream is now active)
        training_table[train_idx].valid = false;
    }
    
    return metadata_in;
}

uint32_t enhanced_stream::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                                 uint8_t prefetch, champsim::address evicted_addr,
                                                 uint32_t metadata_in)
{
    // Paper Section 6: Prefetches must not update training tables
    // This function intentionally does nothing for training
    return metadata_in;
}

void enhanced_stream::prefetcher_cycle_operate()
{
    // Issue prefetches for all active streams
    // This provides a background mechanism for continued prefetching
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (stream_table[i].valid && stream_table[i].active) {
            generate_prefetches(static_cast<int>(i));
        }
    }
}

void enhanced_stream::prefetcher_final_stats()
{
    // Could add statistics printing here if needed
    // For now, leave empty as ChampSim handles most statistics
}
