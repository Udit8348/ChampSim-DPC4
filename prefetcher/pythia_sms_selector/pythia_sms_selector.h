//=======================================================================================//
// File             : pythia_sms_selector/pythia_sms_selector.h
// Description      : Set-dueling based prefetcher selector between Pythia and SMS
//                    Uses sampler sets to track performance and dynamically choose
//                    the best prefetcher for each cache set
//=======================================================================================//

#ifndef __PYTHIA_SMS_SELECTOR_H__
#define __PYTHIA_SMS_SELECTOR_H__

#include <cstdint>
#include <vector>

#include "champsim.h"
#include "modules.h"
#include "util/bits.h"

// Forward declarations - we'll include the actual headers in .cc
struct pythia;
struct sms;
class CACHE;

struct pythia_sms_selector : public champsim::modules::prefetcher {
private:
  // Actual prefetcher instances
  pythia* pref_pythia;
  sms* pref_sms;
  
  long NUM_SET;
  long NUM_WAY;
  
  // Metadata encoding for prefetch source tracking
  // We use bits in metadata to identify which prefetcher issued a prefetch
  static constexpr uint32_t METADATA_PYTHIA_BIT = (1u << 30);
  static constexpr uint32_t METADATA_SMS_BIT = (1u << 31);
  static constexpr uint32_t METADATA_SOURCE_MASK = (METADATA_PYTHIA_BIT | METADATA_SMS_BIT);
  static constexpr uint32_t METADATA_PRESERVE_MASK = ~METADATA_SOURCE_MASK;
  
  // Sampler set approach: track performance metrics
  struct sampler_entry {
    // Pythia metrics
    uint64_t pythia_useful = 0;
    uint64_t pythia_issued = 0;
    uint64_t pythia_late = 0;
    uint64_t pythia_early = 0;
    
    // SMS metrics
    uint64_t sms_useful = 0;
    uint64_t sms_issued = 0;
    uint64_t sms_late = 0;
    uint64_t sms_early = 0;
  };
  
  std::vector<sampler_entry> samplers;
  
  // Track prefetch requests in dedicated sets for performance monitoring
  struct dedicated_set_stats {
    uint64_t pythia_useful = 0;
    uint64_t pythia_issued = 0;
    uint64_t sms_useful = 0;
    uint64_t sms_issued = 0;
  } dedicated_stats;
  
  // Global policy selector counters (saturating counters)
  // Higher value favors Pythia, lower favors SMS
  static constexpr int POLICY_MAX = 1024;
  static constexpr int POLICY_MIN = -1024;
  int policy_selector = 0;
  
  // Statistics
  uint64_t pythia_selected_count = 0;
  uint64_t sms_selected_count = 0;
  uint64_t sampler_pythia_wins = 0;
  uint64_t sampler_sms_wins = 0;
  
  // Sampler set helper functions (copied from modules.cc)
  long get_set_sample_rate() const {
    long set_sample_rate = 32; // 1 in 32
    if(NUM_SET < 1024 && NUM_SET >= 256) {
      set_sample_rate = 16;
    } else if(NUM_SET >= 64) {
      set_sample_rate = 8;
    } else if(NUM_SET >= 8) {
      set_sample_rate = 4;
    }
    return set_sample_rate;
  }
  
  long get_set_sample_category(long set) const {
    auto set_sample_rate = get_set_sample_rate();
    auto mask = set_sample_rate - 1;
    auto shift = champsim::lg2(set_sample_rate);
    auto low_slice = set & mask;
    auto high_slice = (set >> shift) & mask;
    return (set_sample_rate + low_slice - high_slice) & mask;
  }
  
  long get_num_sampled_sets() const {
    return NUM_SET / get_set_sample_rate();
  }
  
  // Set categorization
  // Category 0: Sampler sets (both prefetchers track, but don't issue prefetches)
  // Category 1: Pythia-dedicated sets
  // Category 2: SMS-dedicated sets  
  // Category 3+: Follow the global policy selector
  
  bool is_sampler_set(long set) const {
    return get_set_sample_category(set) == 0;
  }
  
  bool is_pythia_dedicated_set(long set) const {
    return get_set_sample_category(set) == 1;
  }
  
  bool is_sms_dedicated_set(long set) const {
    return get_set_sample_category(set) == 2;
  }
  
  bool use_pythia_for_set(long set) const {
    if (is_pythia_dedicated_set(set))
      return true;
    if (is_sms_dedicated_set(set))
      return false;
    if (is_sampler_set(set))
      return true; // In samplers, use Pythia (we track both via metadata)
    
    // For other sets, use global policy selector (>= 0 means Pythia to give fair start)
    return policy_selector >= 0;
  }
  
  bool use_sms_for_set(long set) const {
    if (is_sms_dedicated_set(set))
      return true;
    if (is_pythia_dedicated_set(set))
      return false;
    if (is_sampler_set(set))
      return false; // In samplers, we use Pythia primarily
    
    // For other sets, use global policy selector
    return policy_selector < 0;
  }
  
  // Helper to tag metadata with prefetcher source
  uint32_t tag_metadata_pythia(uint32_t metadata) const {
    return (metadata & METADATA_PRESERVE_MASK) | METADATA_PYTHIA_BIT;
  }
  
  uint32_t tag_metadata_sms(uint32_t metadata) const {
    return (metadata & METADATA_PRESERVE_MASK) | METADATA_SMS_BIT;
  }
  
  // Helper to check metadata source
  bool is_pythia_prefetch(uint32_t metadata) const {
    return (metadata & METADATA_PYTHIA_BIT) != 0;
  }
  
  bool is_sms_prefetch(uint32_t metadata) const {
    return (metadata & METADATA_SMS_BIT) != 0;
  }
  
  // Update policy selector based on sampler performance
  void update_policy_selector();
  
  // Calculate performance score for a sampler
  double calculate_score(uint64_t useful, uint64_t issued, uint64_t late, uint64_t early) const;

public:
  using champsim::modules::prefetcher::prefetcher;
  
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif /* __PYTHIA_SMS_SELECTOR_H__ */
