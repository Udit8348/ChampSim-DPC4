# Bandwidth-Aware Transformer-Pythia Selector

A bandwidth-aware set-dueling prefetcher selector that dynamically chooses between **Transformer Stream** and **Pythia** based on runtime performance, with throttling based on DRAM bandwidth utilization.

## Throttling Logic

```
BW_util = get_dram_bw() / 16.0   // 0.0 to 1.0  
PF_acc  = useful / issued
ALLOW_PREFETCH = (BW_util < 0.9) AND (PF_acc > BW_util)
```

## Dependencies

This prefetcher requires:
- `../transformer_stream/` - Transformer Stream prefetcher
- `../pythia/` - Pythia prefetcher

**Note**: Making this folder truly "self-contained" by copying pythia/transformer_stream files causes duplicate symbol errors during linking. The relative include approach is the clean solution for ChampSim's architecture.

## Files

- `transformer_pythia_selector_bw.h` - Header with class definition
- `transformer_pythia_selector_bw.cc` - Implementation
- `README.md` - This file

## Usage

Set `"prefetcher": "transformer_pythia_selector_bw"` in your L2C configuration.
