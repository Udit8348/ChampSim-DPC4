//=======================================================================================//
// File             : pythia_sms_selector/pythia_sms_selector.cc
// Description      : Implementation of set-dueling based prefetcher selector
//=======================================================================================//

#include "pythia_sms_selector.h"

#include <algorithm>
#include <cmath>
#include <iostream>

#include "cache.h"
#include "../sms/bitmap.h"  // Include bitmap.h first to ensure BITMAP_MAX_SIZE is defined
#include "../pythia/pythia.h"
#include "../sms/sms.h"

void pythia_sms_selector::prefetcher_initialize()
{
  std::cout << "Initialize PYTHIA-SMS SELECTOR Prefetcher" << std::endl;
  
  // Get cache configuration
  NUM_SET = intern_->NUM_SET;
  NUM_WAY = intern_->NUM_WAY;
  
  std::cout << "  Cache sets: " << NUM_SET << std::endl;
  std::cout << "  Cache ways: " << NUM_WAY << std::endl;
  std::cout << "  Set sample rate: " << get_set_sample_rate() << std::endl;
  std::cout << "  Number of sampler sets: " << get_num_sampled_sets() << std::endl;
  
  // Initialize sampler entries
  samplers.resize(get_num_sampled_sets());
  
  // Create and initialize both prefetchers
  pref_pythia = new pythia(intern_);
  pref_sms = new sms(intern_);
  
  pref_pythia->prefetcher_initialize();
  pref_sms->prefetcher_initialize();
  
  std::cout << "  Both Pythia and SMS prefetchers initialized" << std::endl;
  std::cout << "  Set categorization:" << std::endl;
  std::cout << "    Category 0: Sampler sets (tracking only)" << std::endl;
  std::cout << "    Category 1: Pythia-dedicated sets" << std::endl;
  std::cout << "    Category 2: SMS-dedicated sets" << std::endl;
  std::cout << "    Category 3+: Follow global policy" << std::endl;
}

uint32_t pythia_sms_selector::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, 
                                                        bool useful_prefetch, access_type type, uint32_t metadata_in)
{
  // Calculate which set this address maps to
  long set = (addr.to<uint64_t>() >> LOG2_BLOCK_SIZE) & (NUM_SET - 1);
  
  // Track useful prefetches from our tagged metadata
  if (useful_prefetch && cache_hit) {
    if (is_sampler_set(set)) {
      long sampler_idx = set / get_set_sample_rate();
      if (sampler_idx < (long)samplers.size()) {
        if (is_pythia_prefetch(metadata_in)) {
          samplers[sampler_idx].pythia_useful++;
        }
        if (is_sms_prefetch(metadata_in)) {
          samplers[sampler_idx].sms_useful++;
        }
      }
    } else if (is_pythia_dedicated_set(set)) {
      dedicated_stats.pythia_useful++;
    } else if (is_sms_dedicated_set(set)) {
      dedicated_stats.sms_useful++;
    }
  }
  
  // For sampler sets, we'll use the dedicated set performance instead
  // since both prefetchers issue there and we can measure accurately
  if (is_sampler_set(set)) {
    // Just operate Pythia in sampler sets for simplicity
    pythia_selected_count++;
    uint32_t metadata_out = pref_pythia->prefetcher_cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata_in);
    return tag_metadata_pythia(metadata_out);
  }
  
  // For dedicated sets or policy-controlled sets, use the appropriate prefetcher
  if (use_pythia_for_set(set)) {
    pythia_selected_count++;
    uint32_t metadata_out = pref_pythia->prefetcher_cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata_in);
    return tag_metadata_pythia(metadata_out);
  } else if (use_sms_for_set(set)) {
    sms_selected_count++;
    uint32_t metadata_out = pref_sms->prefetcher_cache_operate(addr, ip, cache_hit, useful_prefetch, type, metadata_in);
    return tag_metadata_sms(metadata_out);
  }
  
  return metadata_in;
}

uint32_t pythia_sms_selector::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, 
                                                     champsim::address evicted_addr, uint32_t metadata_in)
{
  // Track prefetch issued counts based on metadata tags
  if (prefetch) {
    if (is_sampler_set(set)) {
      long sampler_idx = set / get_set_sample_rate();
      if (sampler_idx < (long)samplers.size()) {
        // Use metadata to identify which prefetcher issued this
        if (is_pythia_prefetch(metadata_in)) {
          samplers[sampler_idx].pythia_issued++;
        }
        if (is_sms_prefetch(metadata_in)) {
          samplers[sampler_idx].sms_issued++;
        }
      }
    } else if (is_pythia_dedicated_set(set)) {
      dedicated_stats.pythia_issued++;
    } else if (is_sms_dedicated_set(set)) {
      dedicated_stats.sms_issued++;
    }
  }
  
  // Forward to both prefetchers for their internal state updates
  // Both need to see all fills to maintain accurate internal state
  pref_pythia->prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
  pref_sms->prefetcher_cache_fill(addr, set, way, prefetch, evicted_addr, metadata_in);
  
  return metadata_in;
}

void pythia_sms_selector::prefetcher_cycle_operate()
{
  // Update policy selector periodically based on sampler performance
  static uint64_t cycle_count = 0;
  cycle_count++;
  
  // Update policy every 5000 cycles
  if (cycle_count % 5000 == 0) {
    update_policy_selector();
  }
  
  // Forward to both prefetchers
  pref_pythia->prefetcher_cycle_operate();
  pref_sms->prefetcher_cycle_operate();
}

void pythia_sms_selector::update_policy_selector()
{
  // Use dedicated set statistics (more accurate since both prefetchers operate exclusively)
  uint64_t total_pythia_useful = dedicated_stats.pythia_useful;
  uint64_t total_pythia_issued = dedicated_stats.pythia_issued;
  uint64_t total_sms_useful = dedicated_stats.sms_useful;
  uint64_t total_sms_issued = dedicated_stats.sms_issued;
  
  // Also add sampler set data if available
  for (const auto& s : samplers) {
    total_pythia_useful += s.pythia_useful;
    total_pythia_issued += s.pythia_issued;
    total_sms_useful += s.sms_useful;
    total_sms_issued += s.sms_issued;
  }
  
  // Need minimum data to make decisions
  if (total_pythia_issued < 100 || total_sms_issued < 100) {
    return; // Not enough data yet
  }
  
  // Calculate accuracy for both
  double pythia_accuracy = (double)total_pythia_useful / total_pythia_issued;
  double sms_accuracy = (double)total_sms_useful / total_sms_issued;
  
  // Calculate coverage (useful prefetches)
  double pythia_coverage = total_pythia_useful;
  double sms_coverage = total_sms_useful;
  
  // Combined score (accuracy weighted with coverage)
  double pythia_score = pythia_accuracy * (1.0 + std::log(1.0 + pythia_coverage));
  double sms_score = sms_accuracy * (1.0 + std::log(1.0 + sms_coverage));
  
  // Update policy selector with saturating counter
  if (pythia_score > sms_score * 1.05) { // 5% threshold to avoid thrashing
    policy_selector = std::min(policy_selector + 1, POLICY_MAX);
    sampler_pythia_wins++;
  } else if (sms_score > pythia_score * 1.05) {
    policy_selector = std::max(policy_selector - 1, POLICY_MIN);
    sampler_sms_wins++;
  }
}

double pythia_sms_selector::calculate_score(uint64_t useful, uint64_t issued, uint64_t late, uint64_t early) const
{
  if (issued == 0)
    return 0.0;
  
  double accuracy = (double)useful / issued;
  double coverage = useful;
  double timeliness = 1.0 - ((double)(late + early) / issued);
  
  // Weighted score
  return accuracy * 0.4 + (coverage / 1000.0) * 0.3 + timeliness * 0.3;
}

void pythia_sms_selector::prefetcher_final_stats()
{
  std::cout << std::endl << "=== Pythia-SMS Selector Statistics ===" << std::endl;
  std::cout << "Pythia selected (operates): " << pythia_selected_count << std::endl;
  std::cout << "SMS selected (operates): " << sms_selected_count << std::endl;
  std::cout << "Policy selector value: " << policy_selector << std::endl;
  std::cout << "Sampler Pythia wins: " << sampler_pythia_wins << std::endl;
  std::cout << "Sampler SMS wins: " << sampler_sms_wins << std::endl;
  
  // Aggregate sampler statistics
  uint64_t total_pythia_useful = 0;
  uint64_t total_pythia_issued = 0;
  uint64_t total_sms_useful = 0;
  uint64_t total_sms_issued = 0;
  
  for (const auto& s : samplers) {
    total_pythia_useful += s.pythia_useful;
    total_pythia_issued += s.pythia_issued;
    total_sms_useful += s.sms_useful;
    total_sms_issued += s.sms_issued;
  }
  
  std::cout << std::endl << "Sampler Set Performance:" << std::endl;
  std::cout << "  Pythia - Useful: " << total_pythia_useful 
            << ", Issued: " << total_pythia_issued;
  if (total_pythia_issued > 0)
    std::cout << ", Accuracy: " << (100.0 * total_pythia_useful / total_pythia_issued) << "%";
  std::cout << std::endl;
  
  std::cout << "  SMS - Useful: " << total_sms_useful 
            << ", Issued: " << total_sms_issued;
  if (total_sms_issued > 0)
    std::cout << ", Accuracy: " << (100.0 * total_sms_useful / total_sms_issued) << "%";
  std::cout << std::endl;
  
  // Show dedicated set performance
  std::cout << std::endl << "Dedicated Set Performance:" << std::endl;
  std::cout << "  Pythia - Useful: " << dedicated_stats.pythia_useful
            << ", Issued: " << dedicated_stats.pythia_issued;
  if (dedicated_stats.pythia_issued > 0) {
    double pythia_acc = 100.0 * dedicated_stats.pythia_useful / dedicated_stats.pythia_issued;
    double pythia_score = (dedicated_stats.pythia_useful / (double)dedicated_stats.pythia_issued) * 
                          (1.0 + std::log(1.0 + dedicated_stats.pythia_useful));
    std::cout << ", Accuracy: " << pythia_acc << "%";
    std::cout << ", Score: " << pythia_score;
  }
  std::cout << std::endl;
  
  std::cout << "  SMS - Useful: " << dedicated_stats.sms_useful
            << ", Issued: " << dedicated_stats.sms_issued;
  if (dedicated_stats.sms_issued > 0) {
    double sms_acc = 100.0 * dedicated_stats.sms_useful / dedicated_stats.sms_issued;
    double sms_score = (dedicated_stats.sms_useful / (double)dedicated_stats.sms_issued) * 
                       (1.0 + std::log(1.0 + dedicated_stats.sms_useful));
    std::cout << ", Accuracy: " << sms_acc << "%";
    std::cout << ", Score: " << sms_score;
  }
  std::cout << std::endl;
  
  // Show who should be winning based on scores
  if (dedicated_stats.pythia_issued > 0 && dedicated_stats.sms_issued > 0) {
    double pythia_score = (dedicated_stats.pythia_useful / (double)dedicated_stats.pythia_issued) * 
                          (1.0 + std::log(1.0 + dedicated_stats.pythia_useful));
    double sms_score = (dedicated_stats.sms_useful / (double)dedicated_stats.sms_issued) * 
                       (1.0 + std::log(1.0 + dedicated_stats.sms_useful));
    std::cout << "  Winner: " << (sms_score > pythia_score * 1.05 ? "SMS" : 
                                  pythia_score > sms_score * 1.05 ? "Pythia" : "Tie")
              << " (SMS/Pythia score ratio: " << (sms_score / pythia_score) << ")" << std::endl;
  }
  
  // Forward to individual prefetchers
  std::cout << std::endl << "=== Pythia Statistics ===" << std::endl;
  pref_pythia->prefetcher_final_stats();
  
  std::cout << std::endl << "=== SMS Statistics ===" << std::endl;
  // SMS doesn't have final_stats, so we skip it
  
  std::cout << std::endl;
}
