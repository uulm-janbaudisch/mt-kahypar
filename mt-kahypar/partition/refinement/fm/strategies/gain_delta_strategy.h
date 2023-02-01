/*******************************************************************************
 * MIT License
 *
 * This file is part of Mt-KaHyPar.
 *
 * Copyright (C) 2020 Lars Gottesbüren <lars.gottesbueren@kit.edu>
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

#pragma once

#include "km1_gains.h"
#include "mt-kahypar/partition/refinement/fm/fm_commons.h"


namespace mt_kahypar {
  class GainDeltaStrategy {
  public:

    using VertexPriorityQueue = ds::MaxHeap<Gain, HypernodeID>;

    static constexpr bool uses_gain_cache = false;
    static constexpr bool maintain_gain_cache_between_rounds = false;

    GainDeltaStrategy(const Context& context,
                          HypernodeID numNodes,
                          FMSharedData& sharedData,
                          FMStats& runStats) :
            context(context),
            runStats(runStats),
            sharedData(sharedData),
            vertexPQs(),
            gc(context, sharedData.original_k)
    {
      vertexPQs.reserve(sharedData.original_k);
      for (PartitionID i = 0; i < sharedData.original_k; ++i) {
        vertexPQs.emplace_back(sharedData.vertexPQHandles.data() + (i * numNodes), numNodes);
      }
    }

    template<typename PHG>
    MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE
    void insertIntoPQ(const PHG& phg, const HypernodeID v, const SearchID ) {
      gc.computeGains(phg, v);
      for (PartitionID i = 0; i < context.partition.k; ++i) {
        if (i != phg.partID(v)) {
          vertexPQs[i].insert(v, gc.gains[i]);
          ASSERT(vertexPQs[i].contains(v));
        }
      }
      runStats.pushes++;
    }

    template<typename PHG>
    MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE
    void updateGain(const PHG& /*phg*/, const HypernodeID /*v*/, const Move& /*move*/) {
      // do nothing
    }

    template<typename PHG>
    MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE
    bool findNextMove(const PHG& phg, Move& m) {
      const PartitionID target = findBestTargetBlock(phg);
      if (target == kInvalidPartition) {
        return false;
      }
      const HypernodeID u = vertexPQs[target].top();
      const Gain estimated_gain = vertexPQs[target].topKey();
      vertexPQs[target].deleteTop();
      m.node = u; m.to = target; m.from = phg.partID(u);
      m.gain = estimated_gain;
      runStats.extractions++;
      for (PartitionID i = 0; i < context.partition.k; ++i) {
        if (i != m.from && i != target) {
          vertexPQs[i].remove(u);
        }
      }
      return true;
    }

    void clearPQs(const size_t /* bestImprovementIndex */ ) {
      // release all nodes that were not moved
      const bool release = sharedData.release_nodes
                           && runStats.moves > 0;

      if (release) {
        // Release all nodes contained in the search

        for (PosT j = 0; j < vertexPQs[1].size(); ++j) {
          const HypernodeID v = vertexPQs[1].at(j);
          // we're not storing nodes in pqs for the block they're currently in --> have to check two pqs and deduplicate
          if (!vertexPQs[0].contains(v)) {
            sharedData.nodeTracker.releaseNode(v);
          }
        }
        for (PosT j = 0; j < vertexPQs[0].size(); ++j) {
          sharedData.nodeTracker.releaseNode(vertexPQs[0].at(j));
        }

      }

      for (PartitionID i = 0; i < context.partition.k; ++i) {
        vertexPQs[i].clear();
      }
    }

    // ! perform delta gain updates for vertices that are in our search. uses the PQs as gain store
    template<typename PHG>
    MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE
    void deltaGainUpdates(PHG& phg, const HyperedgeID he, const HyperedgeWeight edge_weight,
                          const PartitionID from, const HypernodeID pin_count_in_from_part_after,
                          const PartitionID to, const HypernodeID pin_count_in_to_part_after) {

      auto some_other_block = [&](const PartitionID i) {
        return context.partition.k - 1 - i;
      };

      auto in_search = [&](const HypernodeID u) {
        return vertexPQs[some_other_block(phg.partID(u))].contains(u);
      };

      auto increase = [&](const HypernodeID u, const PartitionID i) {
        vertexPQs[i].increaseKey(u, vertexPQs[i].getKey(u) + edge_weight);
      };

      auto decrease = [&](const HypernodeID u, const PartitionID i) {
        vertexPQs[i].decreaseKey(u, vertexPQs[i].getKey(u) - edge_weight);
      };

      // gain = moveToBenefit - moveFromPenalty

      if (pin_count_in_from_part_after == 1) {
        for (HypernodeID u : phg.pins(he)) {
          if (phg.partID(u) == from && in_search(u)) {
            // move to benefit increased --> gain increased
            for (PartitionID i = 0; i < context.partition.k; ++i) {
              if (i != from) {
                increase(u, i);
              }
            }
          }
        }
      } else if (pin_count_in_from_part_after == 0) {
        for (HypernodeID u : phg.pins(he)) {
          // moveToBenefit increased --> gain decreased
          if (in_search(u)) {
            decrease(u, from);
          }
        }
      }

      if (pin_count_in_to_part_after == 1) {
        for (HypernodeID u : phg.pins(he)) {
          // moveToBenefit decreased --> gain increased
          if (in_search(u))  {
            increase(u, to);
          }
        }
      } else if (pin_count_in_to_part_after == 2) {
        for (HypernodeID u : phg.pins(he)) {
          if (phg.partID(u) == to && in_search(u)) {
            // move from benefit decreased --> gain decreased
            for (PartitionID i = 0; i < context.partition.k; ++i) {
              if (i != to) {
                decrease(u, i);
              }
            }
          }
        }
      }

    }

    template<typename F>
    void doParallelForAllEntries(F&& f) {
      tbb::parallel_for(0, context.partition.k, [&](PartitionID i) {
        for (size_t j = 0; j < vertexPQs[i].size(); ++j) {
          f(i, vertexPQs[i].at(j), vertexPQs[i].keyAtPos(j));
        }
      });
    }

    void memoryConsumption(utils::MemoryTreeNode *parent) const {
      size_t vertex_pq_sizes = std::accumulate(
               vertexPQs.begin(), vertexPQs.end(), 0,
                    [](size_t init, const VertexPriorityQueue& pq) { return init + pq.size_in_bytes(); }
      );
      parent->addChild("PQs", vertex_pq_sizes);
      parent->addChild("Initial Gain Comp", context.partition.k * sizeof(Gain));
    }

  private:

    template<typename PHG>
    PartitionID findBestTargetBlock(const PHG& phg) {
      PartitionID target = kInvalidPartition;
      HypernodeWeight best_gain = std::numeric_limits<Gain>::min();
      HypernodeWeight best_weight = std::numeric_limits<HypernodeWeight>::max();
      for (PartitionID i = 0; i < context.partition.k; ++i) {
        const HypernodeWeight pw = phg.partWeight(i);
        if (!vertexPQs[i].empty() && pw < context.partition.max_part_weights[i]) {
          const Gain gain = vertexPQs[i].topKey();
          if (gain > best_gain || (gain == best_gain && pw < best_weight)) {
            target = i;
            best_gain = gain;
            best_weight = pw;
          }
        }
      }
      return target;
    }


    size_t handle(HypernodeID u, PartitionID p) const {
      return size_t(u) * context.partition.k +  p;
    }

    const Context& context;

    FMStats& runStats;

    FMSharedData& sharedData;

    vec<VertexPriorityQueue> vertexPQs;

    Km1GainComputer gc;
  };


}