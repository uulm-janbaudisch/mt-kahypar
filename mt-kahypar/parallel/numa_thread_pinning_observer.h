/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2019 Tobias Heuer <tobias.heuer@kit.edu>
 *
 * KaHyPar is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KaHyPar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KaHyPar.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#pragma once

#include <unordered_map>

#undef __TBB_ARENA_OBSERVER
#define __TBB_ARENA_OBSERVER true
#include "tbb/task_scheduler_observer.h"
#undef __TBB_ARENA_OBSERVER

#include "kahypar/macros.h"

namespace kahypar {
namespace parallel {

/**
 * Pins threads of task arena to a NUMA node. Each time a thread
 * enters a task arena on_scheduler_entry(...) is called. Each time
 * a thread leaves a task arena on_scheduler_exit(...) is called.
 */
template< typename HwTopology >
class NumaThreadPinningObserver : public tbb::task_scheduler_observer {
  using Base = tbb::task_scheduler_observer;

  static constexpr bool debug = true;

 public:
  explicit NumaThreadPinningObserver(tbb::task_arena& arena,
                                     int numa_node) :
    Base(arena),
    _arena(arena),
    _topology(HwTopology::instance()),
    _numa_node(numa_node),
    _last_cpu() {
    observe(true);
  }

  NumaThreadPinningObserver(const NumaThreadPinningObserver&) = delete;
  NumaThreadPinningObserver& operator= (const NumaThreadPinningObserver&) = delete;

  NumaThreadPinningObserver(NumaThreadPinningObserver&& other) = default;
  NumaThreadPinningObserver& operator= (NumaThreadPinningObserver&&) = delete;

  ~NumaThreadPinningObserver() {
    observe(false);
  }

  void on_scheduler_entry(bool) override {
    _last_cpu[std::this_thread::get_id()] = sched_getcpu();
    _topology.pin_thread_to_numa_node(_numa_node);
  }

  void on_scheduler_exit(bool) override {
    _topology.unpin_thread_from_numa_node(_numa_node);
    if (_last_cpu.find(std::this_thread::get_id()) != _last_cpu.end()) {
      _topology.pin_thread_to_cpu(_last_cpu[std::this_thread::get_id()]);
      DBG << "Assign thread" << std::this_thread::get_id() 
          << "to its last cpu" << _last_cpu[std::this_thread::get_id()];
      _last_cpu.erase(std::this_thread::get_id());
    }
  }

 private:
  tbb::task_arena& _arena;
  HwTopology& _topology;
  int _numa_node;
  std::unordered_map<std::thread::id, int> _last_cpu;
};

} // namespace parallel
} // namespace kahypar