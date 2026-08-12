#pragma once
// How many CPUs this process may actually use.
// License: MIT

namespace jdoc { namespace util {

// std::thread::hardware_concurrency() reports the machine's processors, not
// the ones this process is allowed to run on: it sees neither CPU affinity
// masks (taskset, cpuset, Windows job objects) nor container CPU quotas
// (docker --cpus). Sizing a worker pool from it oversubscribes inside a
// constrained container and — the costlier half — scales per-worker peak
// memory with the host's core count instead of the container's budget.
//
// Returns the smallest of {hardware_concurrency, affinity mask, cgroup quota},
// never less than 1. Computed once per process and cached.
unsigned available_cpus();

}} // namespace jdoc::util
