//=======================================================================================//
// File             : transformer_pythia_selector_bw/transformer_pythia_selector_bw.h
// Description      : Bandwidth-aware prefetcher selector (uses transformer_stream & pythia)
//
// DEPENDENCIES     : Requires ../transformer_stream/ and ../pythia/ directories
//=======================================================================================//

#ifndef __TRANSFORMER_PYTHIA_SELECTOR_BW_H__
#define __TRANSFORMER_PYTHIA_SELECTOR_BW_H__

#include <cstdint>
#include <vector>

#include "champsim.h"
#include "modules.h"
#include "util/bits.h"

// Forward declarations (from original prefetcher folders)
struct transformer_stream;
struct pythia;

struct transformer_pythia_selector_bw : public champsim::modules::prefetcher {
private:
  // Prefetcher instances (from ../transformer_stream/ and ../pythia/)
  transformer_stream* pref_transformer;
  pythia* pref_pythia;
  
  long NUM_SET;
  long NUM_WAY;
  
  // Bandwidth throttling parameters
  static constexpr double BW_UTIL_THRESHOLD = 0.9;
  static constexpr double MIN_ACCURACY_THRESHOLD = 0.1;
  
  // Bandwidth stats
  uint64_t prefetch_allowed_count = 0;
  uint64_t prefetch_throttled_count = 0;
  uint64_t high_bw_events = 0;
  uint64_t low_accuracy_events = 0;
  
  // Metadata encoding
  static constexpr uint32_t METADATA_TRANSFORMER_BIT = (1u << 30);
  static constexpr uint32_t METADATA_PYTHIA_BIT = (1u << 31);
  static constexpr uint32_t METADATA_SOURCE_MASK = (METADATA_TRANSFORMER_BIT | METADATA_PYTHIA_BIT);
  static constexpr uint32_t METADATA_PRESERVE_MASK = ~METADATA_SOURCE_MASK;
  
  struct sampler_entry {
    uint64_t transformer_useful = 0, transformer_issued = 0;
    uint64_t pythia_useful = 0, pythia_issued = 0;
  };
  std::vector<sampler_entry> samplers;
  
  struct dedicated_set_stats {
    uint64_t transformer_useful = 0, transformer_issued = 0;
    uint64_t pythia_useful = 0, pythia_issued = 0;
  } dedicated_stats;
  
  static constexpr int POLICY_MAX = 1024, POLICY_MIN = -1024;
  int policy_selector = 0;
  
  uint64_t transformer_selected_count = 0, pythia_selected_count = 0;
  uint64_t sampler_transformer_wins = 0, sampler_pythia_wins = 0;
  
  long get_set_sample_rate() const {
    if(NUM_SET < 1024 && NUM_SET >= 256) return 16;
    if(NUM_SET >= 64) return 8;
    if(NUM_SET >= 8) return 4;
    return 32;
  }
  long get_set_sample_category(long set) const {
    auto r = get_set_sample_rate(), m = r - 1;
    return (r + (set & m) - ((set >> champsim::lg2(r)) & m)) & m;
  }
  long get_num_sampled_sets() const { return NUM_SET / get_set_sample_rate(); }
  
  bool is_sampler_set(long s) const { return get_set_sample_category(s) == 0; }
  bool is_transformer_dedicated_set(long s) const { return get_set_sample_category(s) == 1; }
  bool is_pythia_dedicated_set(long s) const { return get_set_sample_category(s) == 2; }
  
  bool use_transformer_for_set(long s) const {
    if (is_transformer_dedicated_set(s)) return true;
    if (is_pythia_dedicated_set(s) || is_sampler_set(s)) return get_set_sample_category(s) != 2;
    return policy_selector >= 0;
  }
  bool use_pythia_for_set(long s) const { return !use_transformer_for_set(s); }
  
  uint32_t tag_metadata_transformer(uint32_t m) const { return (m & METADATA_PRESERVE_MASK) | METADATA_TRANSFORMER_BIT; }
  uint32_t tag_metadata_pythia(uint32_t m) const { return (m & METADATA_PRESERVE_MASK) | METADATA_PYTHIA_BIT; }
  
  bool should_allow_prefetch();
  double get_prefetch_accuracy() const;
  double get_bandwidth_utilization() const;
  void update_policy_selector();

public:
  using champsim::modules::prefetcher::prefetcher;
  
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif
