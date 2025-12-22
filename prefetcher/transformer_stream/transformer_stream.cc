/*
 * Transformer-Aware Stream Prefetcher - Implementation
 * 
 * Extended from Enhanced Stream Prefetcher with five transformer-specific enhancements.
 * All inference is emergent from address behavior - no PCs, hints, or framework info.
 */

#include "transformer_stream.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cache.h"

using namespace transformer_config;

// =============================================================================
// Initialization
// =============================================================================

void transformer_stream::prefetcher_initialize()
{
    // Initialize training table
    for (auto& entry : training_table) {
        entry.valid = false;
        entry.miss_count = 0;
        entry.direction = StreamDirection::UNKNOWN;
        entry.stride = 1;
        entry.last_access_timestamp = 0;
        entry.pattern_confidence = 0;
    }
    
    // Initialize extended stream table
    for (auto& entry : stream_table) {
        entry.valid = false;
        entry.active = false;
        entry.stream_length = 0;
        entry.last_trigger_timestamp = 0;
        entry.stream_class = StreamClass::UNKNOWN;
        entry.reactivation_count = 0;
        entry.confidence_score = 1;
        entry.group_id = -1;
        entry.consistent_stride_count = 0;
    }
    
    // Initialize stream groups
    for (auto& group : stream_groups) {
        group.valid = false;
        group.member_count = 0;
        group.members.fill(-1);
    }
    
    // Initialize pattern history
    for (auto& pattern : pattern_history) {
        pattern.valid = false;
    }
    pattern_history_head = 0;
    
    // Initialize phase state
    phase_state = PhaseState{};
    phase_state.current_prefetch_degree = BASE_PREFETCH_DEGREE;
    
    current_timestamp = 0;
    cleanup_counter = 0;
}

// =============================================================================
// Region Computation (from base)
// =============================================================================

champsim::block_number transformer_stream::compute_region_base(champsim::block_number block) const
{
    uint64_t block_val = block.to<uint64_t>();
    uint64_t region_mask = ~(static_cast<uint64_t>(REGION_SIZE_BLOCKS) - 1);
    return champsim::block_number{block_val & region_mask};
}

// =============================================================================
// Training Table Operations (from base)
// =============================================================================

int transformer_stream::find_training_entry(champsim::block_number region_base) const
{
    for (std::size_t i = 0; i < training_table.size(); ++i) {
        if (training_table[i].valid && training_table[i].region_base == region_base) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int transformer_stream::allocate_training_entry(champsim::block_number region_base)
{
    // Find invalid entry first
    for (std::size_t i = 0; i < training_table.size(); ++i) {
        if (!training_table[i].valid) {
            training_table[i].valid = true;
            training_table[i].region_base = region_base;
            training_table[i].miss_count = 0;
            training_table[i].direction = StreamDirection::UNKNOWN;
            training_table[i].stride = 1;
            training_table[i].last_access_timestamp = current_timestamp;
            training_table[i].pattern_confidence = 0;
            return static_cast<int>(i);
        }
    }
    
    // Evict LRU
    std::size_t lru_idx = 0;
    uint64_t oldest_time = std::numeric_limits<uint64_t>::max();
    
    for (std::size_t i = 0; i < training_table.size(); ++i) {
        if (training_table[i].last_access_timestamp < oldest_time) {
            oldest_time = training_table[i].last_access_timestamp;
            lru_idx = i;
        }
    }
    
    training_table[lru_idx].valid = true;
    training_table[lru_idx].region_base = region_base;
    training_table[lru_idx].miss_count = 0;
    training_table[lru_idx].direction = StreamDirection::UNKNOWN;
    training_table[lru_idx].stride = 1;
    training_table[lru_idx].last_access_timestamp = current_timestamp;
    training_table[lru_idx].pattern_confidence = 0;
    
    return static_cast<int>(lru_idx);
}

void transformer_stream::update_training_entry(int idx, champsim::block_number miss_block)
{
    TrainingEntry& entry = training_table[idx];
    entry.last_access_timestamp = current_timestamp;
    
    if (entry.miss_count == 0) {
        entry.last_miss_block = miss_block;
        entry.miss_count = 1;
        
        // Enhancement 3: Check pattern history for confidence
        entry.pattern_confidence = get_pattern_confidence(
            StreamDirection::UNKNOWN, 0, entry.region_base);
        return;
    }
    
    if (entry.miss_count == 1) {
        entry.second_last_miss_block = entry.last_miss_block;
        entry.last_miss_block = miss_block;
        entry.miss_count = 2;
        return;
    }
    
    // Shift history
    entry.third_last_miss_block = entry.second_last_miss_block;
    entry.second_last_miss_block = entry.last_miss_block;
    entry.last_miss_block = miss_block;
    
    // Compute gaps
    int64_t gap1 = champsim::offset(entry.third_last_miss_block, entry.second_last_miss_block);
    int64_t gap2 = champsim::offset(entry.second_last_miss_block, entry.last_miss_block);
    
    // Noise filtering
    if (is_noise(gap1, gap2)) {
        return;  // Continue training, don't reset
    }
    
    StreamDirection detected_dir = detect_direction(gap1, gap2);
    if (detected_dir == StreamDirection::UNKNOWN) {
        entry.miss_count = 1;
        entry.direction = StreamDirection::UNKNOWN;
        entry.stride = 1;
        return;
    }
    
    int32_t detected_stride = detect_stride(gap1, gap2);
    if (detected_stride <= 0) {
        entry.miss_count = 1;
        entry.direction = StreamDirection::UNKNOWN;
        entry.stride = 1;
        return;
    }
    
    entry.direction = detected_dir;
    entry.stride = detected_stride;
    entry.miss_count = 3;
    
    // Enhancement 3: Update pattern confidence now that we have dir/stride
    entry.pattern_confidence = get_pattern_confidence(
        detected_dir, detected_stride, entry.region_base);
}

// =============================================================================
// Direction and Stride Detection (from base)
// =============================================================================

bool transformer_stream::is_noise(int64_t gap1, int64_t gap2) const
{
    if ((gap1 == 1 && gap2 < 0) || (gap1 == -1 && gap2 > 0) ||
        (gap2 == 1 && gap1 < 0) || (gap2 == -1 && gap1 > 0)) {
        if (std::abs(gap1) <= 1 || std::abs(gap2) <= 1) {
            return true;
        }
    }
    return false;
}

StreamDirection transformer_stream::detect_direction(int64_t gap1, int64_t gap2) const
{
    if (gap1 > 0 && gap2 > 0) return StreamDirection::POSITIVE;
    if (gap1 < 0 && gap2 < 0) return StreamDirection::NEGATIVE;
    return StreamDirection::UNKNOWN;
}

int32_t transformer_stream::detect_stride(int64_t gap1, int64_t gap2) const
{
    int64_t abs_gap1 = std::abs(gap1);
    int64_t abs_gap2 = std::abs(gap2);
    
    if (abs_gap1 != abs_gap2) return 0;
    if (abs_gap1 < 1) return 0;
    
    return static_cast<int32_t>(abs_gap1);
}

// =============================================================================
// Enhancement 1: Stream Grouping Operations
// For multi-head attention where multiple identical streams run concurrently
// =============================================================================

int transformer_stream::find_stream_group(StreamDirection dir, int32_t stride) const
{
    for (std::size_t i = 0; i < stream_groups.size(); ++i) {
        if (stream_groups[i].valid &&
            stream_groups[i].direction == dir &&
            stream_groups[i].stride == stride) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int transformer_stream::find_or_create_stream_group(StreamDirection dir, int32_t stride)
{
    // Try to find existing group
    int existing = find_stream_group(dir, stride);
    if (existing >= 0) {
        stream_groups[existing].last_seen_timestamp = current_timestamp;
        return existing;
    }
    
    // Create new group
    for (std::size_t i = 0; i < stream_groups.size(); ++i) {
        if (!stream_groups[i].valid) {
            stream_groups[i].valid = true;
            stream_groups[i].direction = dir;
            stream_groups[i].stride = stride;
            stream_groups[i].member_count = 0;
            stream_groups[i].group_confidence = 0;
            stream_groups[i].last_seen_timestamp = current_timestamp;
            stream_groups[i].members.fill(-1);
            
            // Classify group based on stride
            if (stride <= DENSE_STRIDE_MAX) {
                stream_groups[i].typical_class = StreamClass::DENSE;
            } else if (stride <= MEDIUM_STRIDE_MAX) {
                stream_groups[i].typical_class = StreamClass::MEDIUM;
            } else {
                stream_groups[i].typical_class = StreamClass::SPARSE;
            }
            
            return static_cast<int>(i);
        }
    }
    
    // All groups full - evict oldest
    std::size_t oldest_idx = 0;
    uint64_t oldest_time = std::numeric_limits<uint64_t>::max();
    
    for (std::size_t i = 0; i < stream_groups.size(); ++i) {
        // Prefer evicting groups with fewer members
        if (stream_groups[i].member_count == 0 ||
            stream_groups[i].last_seen_timestamp < oldest_time) {
            oldest_time = stream_groups[i].last_seen_timestamp;
            oldest_idx = i;
        }
    }
    
    // Clear old group membership
    for (int member_idx : stream_groups[oldest_idx].members) {
        if (member_idx >= 0 && member_idx < static_cast<int>(stream_table.size())) {
            stream_table[member_idx].group_id = -1;
        }
    }
    
    stream_groups[oldest_idx].valid = true;
    stream_groups[oldest_idx].direction = dir;
    stream_groups[oldest_idx].stride = stride;
    stream_groups[oldest_idx].member_count = 0;
    stream_groups[oldest_idx].group_confidence = 0;
    stream_groups[oldest_idx].last_seen_timestamp = current_timestamp;
    stream_groups[oldest_idx].members.fill(-1);
    
    if (stride <= DENSE_STRIDE_MAX) {
        stream_groups[oldest_idx].typical_class = StreamClass::DENSE;
    } else if (stride <= MEDIUM_STRIDE_MAX) {
        stream_groups[oldest_idx].typical_class = StreamClass::MEDIUM;
    } else {
        stream_groups[oldest_idx].typical_class = StreamClass::SPARSE;
    }
    
    return static_cast<int>(oldest_idx);
}

void transformer_stream::add_stream_to_group(int stream_idx, int group_idx)
{
    if (group_idx < 0 || group_idx >= static_cast<int>(stream_groups.size())) return;
    if (stream_idx < 0 || stream_idx >= static_cast<int>(stream_table.size())) return;
    
    StreamGroup& group = stream_groups[group_idx];
    
    // Find empty slot in group
    for (std::size_t i = 0; i < group.members.size(); ++i) {
        if (group.members[i] < 0) {
            group.members[i] = stream_idx;
            group.member_count++;
            stream_table[stream_idx].group_id = group_idx;
            
            // Inherit group's typical class
            stream_table[stream_idx].stream_class = group.typical_class;
            return;
        }
    }
    
    // Group full - don't add but still track relationship
    stream_table[stream_idx].group_id = group_idx;
}

void transformer_stream::remove_stream_from_group(int stream_idx)
{
    if (stream_idx < 0 || stream_idx >= static_cast<int>(stream_table.size())) return;
    
    int group_idx = stream_table[stream_idx].group_id;
    if (group_idx < 0 || group_idx >= static_cast<int>(stream_groups.size())) {
        stream_table[stream_idx].group_id = -1;
        return;
    }
    
    StreamGroup& group = stream_groups[group_idx];
    
    for (std::size_t i = 0; i < group.members.size(); ++i) {
        if (group.members[i] == stream_idx) {
            group.members[i] = -1;
            if (group.member_count > 0) {
                group.member_count--;
            }
            break;
        }
    }
    
    stream_table[stream_idx].group_id = -1;
    
    // Invalidate empty groups
    if (group.member_count == 0) {
        group.valid = false;
    }
}

bool transformer_stream::is_group_protected(int stream_idx) const
{
    if (stream_idx < 0 || stream_idx >= static_cast<int>(stream_table.size())) return false;
    
    int group_idx = stream_table[stream_idx].group_id;
    if (group_idx < 0 || group_idx >= static_cast<int>(stream_groups.size())) return false;
    
    // Stream is protected if its group has multiple active members
    // This prevents evicting streams during multi-head attention
    return stream_groups[group_idx].member_count >= 2;
}

// =============================================================================
// Enhancement 2: Stream Classification
// Classify based on stride magnitude, length, and access density
// =============================================================================

StreamClass transformer_stream::classify_stream(const TransformerStreamEntry& entry) const
{
    // Primary classification based on stride
    if (entry.stride <= DENSE_STRIDE_MAX) {
        // Additional check: must have enough accesses for dense
        if (entry.stream_length >= DENSE_LENGTH_MIN) {
            return StreamClass::DENSE;
        }
        return StreamClass::MEDIUM;  // Short dense → treat as medium
    }
    
    if (entry.stride <= MEDIUM_STRIDE_MAX) {
        if (entry.stream_length >= MEDIUM_LENGTH_MIN) {
            return StreamClass::MEDIUM;
        }
        return StreamClass::SPARSE;  // Short medium → treat as sparse
    }
    
    return StreamClass::SPARSE;
}

uint32_t transformer_stream::get_prefetch_degree_for_class(StreamClass cls) const
{
    switch (cls) {
        case StreamClass::DENSE:
            return DENSE_PREFETCH_DEGREE;
        case StreamClass::MEDIUM:
            return MEDIUM_PREFETCH_DEGREE;
        case StreamClass::SPARSE:
            return SPARSE_PREFETCH_DEGREE;
        default:
            return BASE_PREFETCH_DEGREE;
    }
}

void transformer_stream::update_stream_classification(int stream_idx)
{
    if (stream_idx < 0 || stream_idx >= static_cast<int>(stream_table.size())) return;
    
    TransformerStreamEntry& entry = stream_table[stream_idx];
    if (!entry.valid) return;
    
    entry.stream_class = classify_stream(entry);
    
    // Update group's typical class if this stream is representative
    if (entry.group_id >= 0 && entry.group_id < static_cast<int>(stream_groups.size())) {
        stream_groups[entry.group_id].typical_class = entry.stream_class;
    }
}

// =============================================================================
// Enhancement 3: Repetition-Aware Reinforcement
// Model layer-to-layer repetition in transformers
// =============================================================================

void transformer_stream::record_pattern(const TransformerStreamEntry& entry)
{
    // Record in circular buffer
    PatternHistoryEntry& pattern = pattern_history[pattern_history_head];
    
    pattern.valid = true;
    pattern.direction = entry.direction;
    pattern.stride = entry.stride;
    pattern.region_base = entry.stream_start_block;
    pattern.termination_timestamp = current_timestamp;
    pattern.stream_length = entry.stream_length;
    pattern.stream_class = entry.stream_class;
    
    pattern_history_head = (pattern_history_head + 1) % PATTERN_HISTORY_SIZE;
}

int transformer_stream::find_matching_pattern(StreamDirection dir, int32_t stride,
                                               champsim::block_number region) const
{
    champsim::block_number region_base = compute_region_base(region);
    
    for (std::size_t i = 0; i < pattern_history.size(); ++i) {
        const PatternHistoryEntry& pattern = pattern_history[i];
        if (!pattern.valid) continue;
        
        // Check if within reuse window
        if (current_timestamp - pattern.termination_timestamp > REUSE_WINDOW_SIZE) continue;
        
        // Match on direction and stride
        if (pattern.direction == dir && pattern.stride == stride) {
            // Check region proximity
            champsim::block_number pattern_region = compute_region_base(pattern.region_base);
            int64_t region_diff = std::abs(champsim::offset(region_base, pattern_region));
            
            if (region_diff <= static_cast<int64_t>(REGION_SIZE_BLOCKS * 4)) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

uint32_t transformer_stream::get_pattern_confidence(StreamDirection dir, int32_t stride,
                                                     champsim::block_number region) const
{
    int pattern_idx = find_matching_pattern(dir, stride, region);
    if (pattern_idx < 0) return 0;
    
    const PatternHistoryEntry& pattern = pattern_history[pattern_idx];
    
    // Confidence based on stream length and recency
    uint32_t base_confidence = 1;
    
    if (pattern.stream_length >= DENSE_LENGTH_MIN) {
        base_confidence += 2;
    }
    
    // More recent patterns get higher confidence
    uint64_t age = current_timestamp - pattern.termination_timestamp;
    if (age < REUSE_WINDOW_SIZE / 4) {
        base_confidence += 2;
    } else if (age < REUSE_WINDOW_SIZE / 2) {
        base_confidence += 1;
    }
    
    return std::min(base_confidence, MAX_CONFIDENCE / 2);
}

bool transformer_stream::can_fast_track_training(const TrainingEntry& entry) const
{
    // Fast-track if pattern confidence is high enough
    return entry.pattern_confidence >= FAST_TRACK_CONFIDENCE;
}

void transformer_stream::reinforce_stream_confidence(int stream_idx)
{
    if (stream_idx < 0 || stream_idx >= static_cast<int>(stream_table.size())) return;
    
    TransformerStreamEntry& entry = stream_table[stream_idx];
    if (!entry.valid) return;
    
    // Boost confidence (capped)
    entry.confidence_score = std::min(entry.confidence_score + 1, MAX_CONFIDENCE);
    
    // Also boost group confidence
    if (entry.group_id >= 0 && entry.group_id < static_cast<int>(stream_groups.size())) {
        stream_groups[entry.group_id].group_confidence++;
    }
}

// =============================================================================
// Enhancement 4: Phase-Aware Throttling
// Detect phase transitions from runtime behavior
// =============================================================================

void transformer_stream::update_phase_state(bool stream_terminated)
{
    phase_state.misses_in_window++;
    
    if (stream_terminated) {
        phase_state.streams_terminated_in_window++;
    }
    
    // Check for phase transition
    if (phase_state.misses_in_window >= PHASE_WINDOW_SIZE) {
        if (phase_state.streams_terminated_in_window >= PHASE_TRANSITION_THRESHOLD) {
            // Many streams died → phase transition detected
            phase_state.in_phase_transition = true;
            phase_state.current_prefetch_degree = MIN_PREFETCH_DEGREE;
            phase_state.recovery_counter = 0;
        }
        
        // Reset window
        phase_state.window_start_timestamp = current_timestamp;
        phase_state.streams_terminated_in_window = 0;
        phase_state.misses_in_window = 0;
    }
    
    // Try to recover from phase transition
    if (phase_state.in_phase_transition) {
        try_phase_recovery();
    }
}

bool transformer_stream::is_in_phase_transition() const
{
    return phase_state.in_phase_transition;
}

uint32_t transformer_stream::get_current_prefetch_degree() const
{
    return phase_state.current_prefetch_degree;
}

void transformer_stream::try_phase_recovery()
{
    phase_state.recovery_counter++;
    
    if (phase_state.recovery_counter >= PHASE_RECOVERY_WINDOW) {
        // Behavior is stable again → recover
        phase_state.in_phase_transition = false;
        phase_state.current_prefetch_degree = BASE_PREFETCH_DEGREE;
        phase_state.recovery_counter = 0;
    }
}

// =============================================================================
// Enhancement 5: Cross-Dimension Prefetching Control
// Conservative at stride boundaries
// =============================================================================

uint32_t transformer_stream::get_safe_lookahead(const TransformerStreamEntry& entry) const
{
    // If stride is stable for enough accesses, allow aggressive prefetching
    if (entry.consistent_stride_count >= STRIDE_STABILITY_THRESHOLD) {
        if (entry.stream_class == StreamClass::DENSE) {
            return AGGRESSIVE_LOOKAHEAD;
        }
        return BASE_PREFETCH_DEGREE;
    }
    
    // Otherwise be conservative
    return CONSERVATIVE_LOOKAHEAD;
}

bool transformer_stream::is_at_stride_boundary(const TransformerStreamEntry& entry) const
{
    // Check if remaining distance to stream end is less than one stride
    if (entry.direction == StreamDirection::POSITIVE) {
        int64_t remaining = champsim::offset(entry.current_prefetch_block, entry.stream_end_block);
        return remaining <= entry.stride;
    } else {
        int64_t remaining = champsim::offset(entry.stream_end_block, entry.current_prefetch_block);
        return remaining <= entry.stride;
    }
}

// =============================================================================
// Stream Table Operations (enhanced from base)
// =============================================================================

int transformer_stream::find_stream_for_block(champsim::block_number block) const
{
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) continue;
        
        const TransformerStreamEntry& entry = stream_table[i];
        
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
    return -1;
}

int transformer_stream::find_matching_inactive_stream(StreamDirection dir, int32_t stride,
                                                       champsim::block_number region_base) const
{
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) continue;
        if (stream_table[i].active) continue;
        
        const TransformerStreamEntry& entry = stream_table[i];
        
        if (entry.direction != dir || entry.stride != stride) continue;
        
        champsim::block_number stream_region = compute_region_base(entry.stream_start_block);
        int64_t region_diff = std::abs(champsim::offset(region_base, stream_region));
        
        if (region_diff <= static_cast<int64_t>(REGION_SIZE_BLOCKS * 2)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int transformer_stream::compute_eviction_priority(int stream_idx) const
{
    if (stream_idx < 0 || stream_idx >= static_cast<int>(stream_table.size())) {
        return 0;
    }
    
    const TransformerStreamEntry& entry = stream_table[stream_idx];
    if (!entry.valid) return std::numeric_limits<int>::max();  // Invalid = highest priority to evict
    
    int priority = 0;
    
    // Base priority by class (DENSE=30, MEDIUM=20, SPARSE=10)
    switch (entry.stream_class) {
        case StreamClass::DENSE:  priority = 30; break;
        case StreamClass::MEDIUM: priority = 20; break;
        case StreamClass::SPARSE: priority = 10; break;
        default:                  priority = 15; break;
    }
    
    // Confidence boost
    priority += static_cast<int>(entry.confidence_score * 2);
    
    // Group membership protection
    if (entry.group_id >= 0 && entry.group_id < static_cast<int>(stream_groups.size())) {
        priority += static_cast<int>(stream_groups[entry.group_id].member_count * 3);
    }
    
    // Active streams are protected
    if (entry.active) {
        priority += 10;
    }
    
    // Age penalty (older = lower priority)
    uint64_t age = current_timestamp - entry.last_trigger_timestamp;
    if (age > DEAD_STREAM_THRESHOLD / 2) {
        priority -= 5;
    }
    if (age > DEAD_STREAM_THRESHOLD) {
        priority -= 10;
    }
    
    return priority;
}

int transformer_stream::select_victim_stream() const
{
    int victim_idx = -1;
    int lowest_priority = std::numeric_limits<int>::max();
    
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) {
            return static_cast<int>(i);  // Invalid entry is best victim
        }
        
        int priority = compute_eviction_priority(static_cast<int>(i));
        
        if (priority < lowest_priority) {
            lowest_priority = priority;
            victim_idx = static_cast<int>(i);
        }
    }
    
    return victim_idx;
}

int transformer_stream::allocate_stream_entry()
{
    // First, try to find an invalid entry
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) {
            return static_cast<int>(i);
        }
    }
    
    // Try to remove dead streams first
    remove_dead_streams();
    
    // Check again
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (!stream_table[i].valid) {
            return static_cast<int>(i);
        }
    }
    
    // Use smart victim selection
    int victim = select_victim_stream();
    if (victim >= 0) {
        terminate_stream(victim);
    }
    
    return victim;
}

void transformer_stream::create_stream(const TrainingEntry& trained_entry)
{
    int idx = allocate_stream_entry();
    if (idx < 0) return;
    
    TransformerStreamEntry& entry = stream_table[idx];
    
    entry.valid = true;
    entry.active = true;
    entry.direction = trained_entry.direction;
    entry.stride = trained_entry.stride;
    entry.last_trigger_timestamp = current_timestamp;
    entry.stream_length = 0;
    entry.reactivation_count = 0;
    entry.consistent_stride_count = 0;
    
    // Enhancement 3: Inherit confidence from pattern history
    entry.confidence_score = std::max(1u, trained_entry.pattern_confidence);
    
    entry.stream_start_block = trained_entry.last_miss_block;
    entry.current_prefetch_block = trained_entry.last_miss_block;
    
    int64_t dir_val = static_cast<int64_t>(trained_entry.direction);
    int64_t end_offset = dir_val * trained_entry.stride * 64;
    entry.stream_end_block = trained_entry.last_miss_block + end_offset;
    
    // Enhancement 2: Initial classification
    entry.stream_class = classify_stream(entry);
    
    // Enhancement 1: Add to stream group
    int group_idx = find_or_create_stream_group(entry.direction, entry.stride);
    add_stream_to_group(idx, group_idx);
    
    // Generate initial prefetches
    generate_prefetches(idx);
}

void transformer_stream::reactivate_stream(int idx, champsim::block_number trigger_block)
{
    TransformerStreamEntry& entry = stream_table[idx];
    
    entry.active = true;
    entry.last_trigger_timestamp = current_timestamp;
    entry.reactivation_count++;
    entry.current_prefetch_block = trigger_block;
    
    // Enhancement 3: Boost confidence on reactivation
    entry.confidence_score = std::min(entry.confidence_score + CONFIDENCE_BOOST_ON_REUSE, MAX_CONFIDENCE);
    
    // Extend stream end
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
    
    // Re-add to group if needed
    if (entry.group_id < 0) {
        int group_idx = find_or_create_stream_group(entry.direction, entry.stride);
        add_stream_to_group(idx, group_idx);
    }
    
    generate_prefetches(idx);
}

bool transformer_stream::try_relaunch_stream(champsim::block_number miss_block,
                                              StreamDirection dir, int32_t stride)
{
    champsim::block_number region = compute_region_base(miss_block);
    int match_idx = find_matching_inactive_stream(dir, stride, region);
    
    if (match_idx >= 0) {
        reactivate_stream(match_idx, miss_block);
        return true;
    }
    
    return false;
}

// =============================================================================
// Prefetch Generation (enhanced with all awareness)
// =============================================================================

void transformer_stream::generate_prefetches(int stream_idx)
{
    TransformerStreamEntry& entry = stream_table[stream_idx];
    
    if (!entry.valid || !entry.active) return;
    
    // Enhancement 4: Get phase-aware degree
    uint32_t phase_degree = get_current_prefetch_degree();
    
    // Enhancement 2: Get class-aware degree
    uint32_t class_degree = get_prefetch_degree_for_class(entry.stream_class);
    
    // Enhancement 5: Get safe lookahead
    uint32_t safe_lookahead = get_safe_lookahead(entry);
    
    // Use minimum of all constraints
    uint32_t actual_degree = std::min({phase_degree, class_degree, safe_lookahead});
    
    // Enhancement 4: Further reduce during phase transitions
    if (is_in_phase_transition()) {
        actual_degree = std::min(actual_degree, MIN_PREFETCH_DEGREE);
    }
    
    int64_t dir_val = static_cast<int64_t>(entry.direction);
    
    for (uint32_t i = 0; i < actual_degree; ++i) {
        champsim::block_number next_block = entry.current_prefetch_block + (dir_val * entry.stride);
        
        // Check bounds
        if (entry.direction == StreamDirection::POSITIVE) {
            if (next_block > entry.stream_end_block) {
                entry.active = false;
                return;
            }
        } else {
            if (next_block < entry.stream_end_block) {
                entry.active = false;
                return;
            }
        }
        
        // Enhancement 5: Check if at boundary - be extra conservative
        if (is_at_stride_boundary(entry) && i > 0) {
            break;  // Stop prefetching near boundary
        }
        
        // Check MSHR
        double mshr_ratio = intern_->get_mshr_occupancy_ratio();
        if (mshr_ratio > 0.75) {
            return;
        }
        
        champsim::address pf_addr{next_block};
        bool fill_this_level = (mshr_ratio < 0.5);
        
        bool success = prefetch_line(pf_addr, fill_this_level, 0);
        
        if (success) {
            entry.current_prefetch_block = next_block;
            entry.stream_length++;
            entry.consistent_stride_count++;
            
            // Update classification periodically
            if (entry.stream_length % 8 == 0) {
                update_stream_classification(stream_idx);
            }
        } else {
            return;
        }
    }
    
    entry.last_trigger_timestamp = current_timestamp;
}

// =============================================================================
// Dead Stream Removal (enhanced with group awareness)
// =============================================================================

void transformer_stream::terminate_stream(int stream_idx)
{
    if (stream_idx < 0 || stream_idx >= static_cast<int>(stream_table.size())) return;
    
    TransformerStreamEntry& entry = stream_table[stream_idx];
    if (!entry.valid) return;
    
    // Enhancement 3: Record pattern before termination
    record_pattern(entry);
    
    // Enhancement 1: Remove from group
    remove_stream_from_group(stream_idx);
    
    // Enhancement 4: Update phase state
    update_phase_state(true);
    
    entry.valid = false;
    entry.active = false;
}

void transformer_stream::remove_dead_streams()
{
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        TransformerStreamEntry& entry = stream_table[i];
        if (!entry.valid) continue;
        
        uint64_t age = current_timestamp - entry.last_trigger_timestamp;
        
        // Check dead stream criteria
        bool is_dead = (age > DEAD_STREAM_THRESHOLD) && 
                       (entry.stream_length < SHORT_STREAM_THRESHOLD);
        
        // Enhancement 1: Don't kill if group-protected with high confidence
        if (is_dead && is_group_protected(static_cast<int>(i))) {
            if (entry.confidence_score >= FAST_TRACK_CONFIDENCE) {
                is_dead = false;  // Keep high-confidence grouped streams
            }
        }
        
        if (is_dead) {
            terminate_stream(static_cast<int>(i));
        }
    }
}

// =============================================================================
// Main Prefetcher Interface
// =============================================================================

uint32_t transformer_stream::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                                       uint8_t cache_hit, bool useful_prefetch,
                                                       access_type type, uint32_t metadata_in)
{
    // Training on CACHE MISSES ONLY
    if (cache_hit) {
        return metadata_in;
    }
    
    current_timestamp++;
    
    // Enhancement 4: Update phase state
    update_phase_state(false);
    
    // Periodic cleanup
    cleanup_counter++;
    if (cleanup_counter >= CLEANUP_INTERVAL) {
        remove_dead_streams();
        cleanup_counter = 0;
    }
    
    champsim::block_number miss_block{addr};
    champsim::block_number region_base = compute_region_base(miss_block);
    
    // Step 1: Check if triggers existing stream
    int stream_idx = find_stream_for_block(miss_block);
    if (stream_idx >= 0) {
        TransformerStreamEntry& entry = stream_table[stream_idx];
        entry.last_trigger_timestamp = current_timestamp;
        entry.accesses_in_window++;
        
        if (!entry.active) {
            entry.active = true;
            entry.reactivation_count++;
        }
        
        // Enhancement 3: Reinforce confidence
        reinforce_stream_confidence(stream_idx);
        
        generate_prefetches(stream_idx);
        return metadata_in;
    }
    
    // Step 2: Training
    int train_idx = find_training_entry(region_base);
    
    if (train_idx < 0) {
        train_idx = allocate_training_entry(region_base);
    }
    
    update_training_entry(train_idx, miss_block);
    
    // Step 3: Check confirmation
    TrainingEntry& trained = training_table[train_idx];
    
    // Enhancement 3: Fast-track for high-confidence patterns
    bool ready = (trained.miss_count >= CONFIRMATION_THRESHOLD) ||
                 (trained.miss_count >= 2 && can_fast_track_training(trained));
    
    if (ready && trained.direction != StreamDirection::UNKNOWN && trained.stride >= 1) {
        if (!try_relaunch_stream(miss_block, trained.direction, trained.stride)) {
            create_stream(trained);
        }
        
        training_table[train_idx].valid = false;
    }
    
    return metadata_in;
}

uint32_t transformer_stream::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                                    uint8_t prefetch, champsim::address evicted_addr,
                                                    uint32_t metadata_in)
{
    return metadata_in;
}

void transformer_stream::prefetcher_cycle_operate()
{
    // Background prefetching for active streams
    for (std::size_t i = 0; i < stream_table.size(); ++i) {
        if (stream_table[i].valid && stream_table[i].active) {
            generate_prefetches(static_cast<int>(i));
        }
    }
}

void transformer_stream::prefetcher_final_stats()
{
    // Statistics could be added here
}
