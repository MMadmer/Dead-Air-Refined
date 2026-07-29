#pragma once

#include <parallel_hashmap/phmap.h>

#include "xr_allocator.h"

template <typename K, class V, class Hasher = phmap::Hash<K>, class Equal = std::equal_to<K>,
    typename Allocator = xr_allocator<std::pair<const K, V>>>
using xr_flat_hash_map = phmap::flat_hash_map<K, V, Hasher, Equal, Allocator>;

template <typename K, class V, class Hasher = phmap::Hash<K>, class Equal = std::equal_to<K>,
    typename Allocator = xr_allocator<std::pair<const K, V>>>
using xr_node_hash_map = phmap::node_hash_map<K, V, Hasher, Equal, Allocator>;
