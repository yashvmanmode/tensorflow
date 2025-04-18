/* Copyright 2025 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_SERVICE_GPU_AUTOTUNING_DOT_SEARCH_SPACE_H_
#define XLA_SERVICE_GPU_AUTOTUNING_DOT_SEARCH_SPACE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/gpu/matmul_utils.h"
#include "xla/stream_executor/device_description.h"

namespace xla::gpu {

// Generates the space of promising Triton configs for a given dot fusion
// and hardware.
//
// Takes into account the properties of the problem (e.g., operand and result
// shapes, fused instructions), and the hardware (e.g., number of cores,
// available registers and memory per core).
//
// Internal doc with rationale: go/xla-gpu-dot-search
class TritonDotFusionSearchSpace {
 public:
  TritonDotFusionSearchSpace(const se::DeviceDescription& device_description,
                             const HloDotInstruction* dot);

  // Generates the list of promising configs in the search space for the
  // autotuner to try. If `force_contracting_split` is set, the search space
  // will be restricted to only include configs with the given split_k factor.
  std::vector<TritonGemmConfig> GenerateConfigs(
      std::optional<int64_t> force_contracting_split = std::nullopt);

  // Serializes the search space to a human-readable string.
  std::string Serialize();

 private:
  // Groups together the tiling of the dot's output dimensions: the parallel
  // dimensions of the left and right hand sides. We assume that any batch
  // dimensions are tiled by a factor of 1.
  struct OutputTile {
    int lhs_dim = 0;  // LHS tiling (aka. block_m).
    int rhs_dim = 0;  // RHS tiling (aka. block_n).
  };

  // Adds notes to configs, which carry additional information we need to
  // consider while generating the search space.
  struct ConfigWithNotes {
    TritonGemmConfig config;
    // This config has a larger than expected split_k, but we do not want to
    // discard it.
    bool keep_large_split = false;
    // This config does not have enough tiles for all cores to be occupied.
    bool not_enough_tiles = false;

    std::string ToString() const { return config.ToString(); }
  };

  // Callback type for `ExtendConfigs`. The method should append zero or more
  // extensions of `config` to the `updated_configs` vector.
  using ExtendConfigCallback = void (TritonDotFusionSearchSpace::*)(
      const ConfigWithNotes& config,
      std::vector<ConfigWithNotes>& updated_configs);

  // Extends Triton gemm configs by repeatedly calling `*extend_config()` on
  // each config in `configs`. Expects that after all calls to `extend_config`,
  // the updated list of configs is non-empty.
  void ExtendConfigs(std::vector<ConfigWithNotes>& configs,
                     ExtendConfigCallback extend_config);

  // Computes the maximum sensible size of the output tile (block_m, block_n)
  // based on the dot shape and element type, and the available registers on
  // the core.
  OutputTile GetMaxOutputTile() const;

  // Computes the number of result tiles we would have without
  // splitting the contracting dimension for a given output tile.
  int64_t GetNumResultTiles(OutputTile output_tile) const;

  // Computes the maximum sensible split in the contracting dimension
  // (split_k) to sufficiently occupy all available cores when using the given
  // output tile.
  int GetMaxContractingSplit(OutputTile output_tile) const;

  // Finds all promising values for splitting the contracting dimension to
  // achieve sufficient occupancy (split_k).
  std::vector<ConfigWithNotes> GenerateContractingSplitFactors();

  // Finds all promising output shape tilings (block_m, block_n), based on
  // `config` with already determined contracting split value and appends them
  // to `updated_configs`. Each config in the input list might yield zero or
  // more configs in the output.
  void AddOutputTilings(const ConfigWithNotes& config,
                        std::vector<ConfigWithNotes>& updated_configs);

  // Removes configs that are marked with `not_enough_tiles` from the list. If
  // this results in an empty list, adds a config that should be the most
  // optimal one even though it does not occupy all cores.
  void EliminateLowOccupancyConfigs(std::vector<ConfigWithNotes>& configs);

  se::DeviceDescription device_description_;
  int64_t contracting_size_;
  int64_t batch_size_;
  int64_t lhs_parallel_size_;
  int64_t rhs_parallel_size_;
  int compute_bitwidth_;
  int desired_total_warps_;
  OutputTile max_out_tile_;
  OutputTile min_out_tile_;
  int min_warps_per_cta_;
  int min_contracting_tile_size_;
  int max_contracting_split_;
};

}  // namespace xla::gpu

#endif  // XLA_SERVICE_GPU_AUTOTUNING_DOT_SEARCH_SPACE_H_
