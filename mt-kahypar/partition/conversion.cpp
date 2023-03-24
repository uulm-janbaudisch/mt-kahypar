/*******************************************************************************
 * MIT License
 *
 * This file is part of Mt-KaHyPar.
 *
 * Copyright (C) 2023 Tobias Heuer <tobias.heuer@kit.edu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/


#include "mt-kahypar/partition/conversion.h"

#include "mt-kahypar/macros.h"

namespace mt_kahypar {

mt_kahypar_hypergraph_type_t to_hypergraph_c_type(const PresetType preset,
                                                  const InstanceType instance) {
  if ( instance == InstanceType::hypergraph ) {
    switch ( preset ) {
      case PresetType::deterministic:
      case PresetType::large_k:
      case PresetType::default_preset:
      case PresetType::default_flows: return STATIC_HYPERGRAPH;
      case PresetType::quality_preset:
      case PresetType::quality_flows: return DYNAMIC_HYPERGRAPH;
      case PresetType::UNDEFINED: ERR("Unknown preset type!");
    }
  }
  #ifdef KAHYPAR_ENABLE_GRAPH_PARTITIONING_FEATURES
  else if ( instance == InstanceType::graph ) {
    switch ( preset ) {
      case PresetType::deterministic:
      case PresetType::large_k:
      case PresetType::default_preset:
      case PresetType::default_flows: return STATIC_GRAPH;
      case PresetType::quality_preset:
      case PresetType::quality_flows: return DYNAMIC_GRAPH;
      case PresetType::UNDEFINED: ERR("Unknown preset type!");
    }
  }
  #endif
  else {
    ERR("Unknown instance type. Should be either graph or hypergraph");
  }
  return NULLPTR_HYPERGRAPH;
}

mt_kahypar_partition_type_t to_partition_c_type(const PresetType preset,
                                                const InstanceType instance) {
  #ifdef KAHYPAR_ENABLE_GRAPH_PARTITIONING_FEATURES
  if ( instance == InstanceType::graph ) {
    if ( preset == PresetType::default_preset ||
         preset == PresetType::default_flows ||
         preset == PresetType::large_k ||
         preset == PresetType::deterministic ) {
      return MULTILEVEL_GRAPH_PARTITIONING;
    } else if ( preset == PresetType::quality_preset ||
                preset == PresetType::quality_flows ) {
      return N_LEVEL_HYPERGRAPH_PARTITIONING;
    }
  } else
  #endif
  if ( instance == InstanceType::hypergraph ) {
    if ( preset == PresetType::default_preset ||
         preset == PresetType::default_flows ||
         preset == PresetType::deterministic ) {
      return MULTILEVEL_HYPERGRAPH_PARTITIONING;
    } else if ( preset == PresetType::quality_preset ||
                preset == PresetType::quality_flows ) {
      return N_LEVEL_HYPERGRAPH_PARTITIONING;
    } else if ( preset == PresetType::large_k ) {
      return LARGE_K_PARTITIONING;
    }
  }
  return NULLPTR_PARTITION;
}

PresetType to_preset_type(const Mode mode,
                          const PartitionID k,
                          const CoarseningAlgorithm coarsening_algo,
                          const FlowAlgorithm flow_algo) {
  if ( coarsening_algo == CoarseningAlgorithm::deterministic_multilevel_coarsener ) {
    return PresetType::deterministic;
  } else if ( mode == Mode::deep_multilevel && k >= 1024 ) {
    return PresetType::large_k;
  } else if ( coarsening_algo == CoarseningAlgorithm::multilevel_coarsener ) {
    if ( flow_algo == FlowAlgorithm::flow_cutter ) {
      return PresetType::default_flows;
    } else {
      return PresetType::default_preset;
    }
  } else if ( coarsening_algo == CoarseningAlgorithm::nlevel_coarsener ) {
    if ( flow_algo == FlowAlgorithm::flow_cutter ) {
      return PresetType::quality_flows;
    } else {
      return PresetType::quality_preset;
    }
  }
  return PresetType::UNDEFINED;
}

InstanceType to_instance_type(const FileFormat format) {
  if ( format == FileFormat::Metis ) {
    #ifdef KAHYPAR_ENABLE_GRAPH_PARTITIONING_FEATURES
    return InstanceType::graph;
    #else
    return InstanceType::hypergraph;
    #endif
  } else if ( format == FileFormat::hMetis ) {
    return InstanceType::hypergraph;
  }
  return InstanceType::UNDEFINED;
}

}  // namespace mt_kahypar
