//=======================================================================================//
// File             : transformer_pythia_selector_bw/transformer_pythia_selector_bw.cc
// Description      : Bandwidth-aware prefetcher selector
//
// DEPENDENCIES     : Requires ../transformer_stream/ and ../pythia/ directories
//=======================================================================================//

#include "transformer_pythia_selector_bw.h"

#include <algorithm>
#include <cmath>
#include <iostream>

#include "cache.h"
#include "dpc_api.h"
#include "../transformer_stream/transformer_stream.h"
#include "../pythia/pythia.h"

void transformer_pythia_selector_bw::prefetcher_initialize()
{
  std::cout << "Initialize BW-AWARE TRANSFORMER-PYTHIA SELECTOR" << std::endl;
  
  NUM_SET = intern_->NUM_SET;
  NUM_WAY = intern_->NUM_WAY;
  
  std::cout << "  Sets: " << NUM_SET << ", Ways: " << NUM_WAY << std::endl;
  std::cout << "  BW threshold: " << (BW_UTIL_THRESHOLD * 100) << "%" << std::endl;
  
  samplers.resize(get_num_sampled_sets());
  
  pref_transformer = new transformer_stream(intern_);
  pref_pythia = new pythia(intern_);
  
  pref_transformer->prefetcher_initialize();
  pref_pythia->prefetcher_initialize();
  
  std::cout << "  Self-contained prefetchers initialized (tps_bw namespace)" << std::endl;
}

double transformer_pythia_selector_bw::get_bandwidth_utilization() const {
  return static_cast<double>(get_dram_bw()) / 16.0;
}

double transformer_pythia_selector_bw::get_prefetch_accuracy() const {
  uint64_t useful = dedicated_stats.transformer_useful + dedicated_stats.pythia_useful;
  uint64_t issued = dedicated_stats.transformer_issued + dedicated_stats.pythia_issued;
  for (const auto& s : samplers) {
    useful += s.transformer_useful + s.pythia_useful;
    issued += s.transformer_issued + s.pythia_issued;
  }
  return issued ? static_cast<double>(useful) / issued : 1.0;
}

bool transformer_pythia_selector_bw::should_allow_prefetch() {
  double bw = get_bandwidth_utilization(), acc = get_prefetch_accuracy();
  bool bw_ok = bw < BW_UTIL_THRESHOLD, acc_ok = acc > bw || acc > MIN_ACCURACY_THRESHOLD;
  if (!bw_ok) high_bw_events++;
  if (!acc_ok) low_accuracy_events++;
  bool allow = bw_ok && acc_ok;
  if (allow) prefetch_allowed_count++; else prefetch_throttled_count++;
  return allow;
}

uint32_t transformer_pythia_selector_bw::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
    uint8_t cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in)
{
  long set = (addr.to<uint64_t>() >> LOG2_BLOCK_SIZE) & (NUM_SET - 1);
  
  if (useful_prefetch && cache_hit) {
    if (is_sampler_set(set)) {
      long idx = set / get_set_sample_rate();
      if (idx < (long)samplers.size()) samplers[idx].transformer_useful++;
    } else if (is_transformer_dedicated_set(set)) dedicated_stats.transformer_useful++;
    else if (is_pythia_dedicated_set(set)) dedicated_stats.pythia_useful++;
    else { if (policy_selector >= 0) dedicated_stats.transformer_useful++; else dedicated_stats.pythia_useful++; }
  }
  
  if (!should_allow_prefetch()) return metadata_in;
  
  if (is_sampler_set(set) || use_transformer_for_set(set)) {
    transformer_selected_count++;
    return tag_metadata_transformer(pref_transformer->prefetcher_cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata_in));
  } else {
    pythia_selected_count++;
    return tag_metadata_pythia(pref_pythia->prefetcher_cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata_in));
  }
}

uint32_t transformer_pythia_selector_bw::prefetcher_cache_fill(champsim::address addr, long set, long way,
    uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  if (prefetch) {
    if (is_sampler_set(set)) {
      long idx = set / get_set_sample_rate();
      if (idx < (long)samplers.size()) samplers[idx].transformer_issued++;
    } else if (is_transformer_dedicated_set(set)) dedicated_stats.transformer_issued++;
    else if (is_pythia_dedicated_set(set)) dedicated_stats.pythia_issued++;
    else { if (policy_selector >= 0) dedicated_stats.transformer_issued++; else dedicated_stats.pythia_issued++; }
  }
  pref_transformer->prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
  pref_pythia->prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
  return metadata_in;
}

void transformer_pythia_selector_bw::prefetcher_cycle_operate() {
  static uint64_t cycles = 0;
  if (++cycles % 5000 == 0) update_policy_selector();
  pref_transformer->prefetcher_cycle_operate();
  pref_pythia->prefetcher_cycle_operate();
}

void transformer_pythia_selector_bw::update_policy_selector() {
  uint64_t t_u = dedicated_stats.transformer_useful, t_i = dedicated_stats.transformer_issued;
  uint64_t p_u = dedicated_stats.pythia_useful, p_i = dedicated_stats.pythia_issued;
  for (const auto& s : samplers) { t_u += s.transformer_useful; t_i += s.transformer_issued; p_u += s.pythia_useful; p_i += s.pythia_issued; }
  if (t_i < 100 || p_i < 100) return;
  double t_s = ((double)t_u/t_i) * (1.0 + std::log(1.0 + t_u));
  double p_s = ((double)p_u/p_i) * (1.0 + std::log(1.0 + p_u));
  if (t_s > p_s * 1.05) { policy_selector = std::min(policy_selector + 1, POLICY_MAX); sampler_transformer_wins++; }
  else if (p_s > t_s * 1.05) { policy_selector = std::max(policy_selector - 1, POLICY_MIN); sampler_pythia_wins++; }
}

void transformer_pythia_selector_bw::prefetcher_final_stats() {
  std::cout << "\n=== Self-Contained BW-Aware Transformer-Pythia Selector ===" << std::endl;
  uint64_t total = prefetch_allowed_count + prefetch_throttled_count;
  std::cout << "BW Throttling: allowed=" << prefetch_allowed_count << " throttled=" << prefetch_throttled_count;
  if (total) std::cout << " (" << (100.0 * prefetch_throttled_count / total) << "%)";
  std::cout << "\n  High BW: " << high_bw_events << ", Low acc: " << low_accuracy_events << std::endl;
  std::cout << "Selection: T=" << transformer_selected_count << " P=" << pythia_selected_count << std::endl;
  std::cout << "Policy: " << policy_selector << " (T-wins=" << sampler_transformer_wins << " P-wins=" << sampler_pythia_wins << ")\n";
  pref_transformer->prefetcher_final_stats();
  pref_pythia->prefetcher_final_stats();
}
