// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_MAP_H_
#define NINJA_MAP_H_

#include <algorithm>
#include <string>
#include <cstring>
#include <string_view>

// MurmurHash2, by Austin Appleby
static inline
unsigned int MurmurHash2(const void* key, size_t len) {
  static const unsigned int seed = 0xDECAFBAD;
  const unsigned int m = 0x5bd1e995;
  const int r = 24;
  unsigned int h = seed ^ len;
  const unsigned char* data = (const unsigned char*)key;
  while (len >= 4) {
    unsigned int k;
    std::memcpy(&k, data, sizeof k);
    k *= m;
    k ^= k >> r;
    k *= m;
    h *= m;
    h ^= k;
    data += 4;
    len -= 4;
  }
  switch (len) {
  case 3: h ^= data[2] << 16;
  case 2: h ^= data[1] << 8;
  case 1: h ^= data[0];
    h *= m;
  };
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}

#if (__cplusplus >= 201103L) || (_MSC_VER >= 1900)
#include <unordered_map>

#elif defined(_MSC_VER)
#include <hash_map>

using stdext::hash_map;
using stdext::hash_compare;

struct StringViewCmp : public hash_compare<std::string_view> {
  size_t operator()(std::string_view key) const {
    return MurmurHash2(key.str_, key.len_);
  }
  bool operator()(std::string_view a, std::string_view b) const {
    int cmp = memcmp(a.data(), b.data(), min(a.size(), b.size()));
    if (cmp < 0) {
      return true;
    } else if (cmp > 0) {
      return false;
    } else {
      return a.size() < b.size();
    }
  }
};

#else
#include <ext/hash_map>

using __gnu_cxx::hash_map;

namespace __gnu_cxx {
template<>
struct hash<std::string_view> {
  size_t operator()(std::string_view key) const {
    return MurmurHash2(key.data(), key.size());
  }
};
}
#endif

/// A template for hash_maps keyed by a std::string_view whose string is
/// owned externally (typically by the values).  Use like:
/// ExternalStringHash<Foo*>::Type foos; to make foos into a hash
/// mapping std::string_view => Foo*.
template<typename V>
struct ExternalStringHashMap {
#if (__cplusplus >= 201103L) || (_MSC_VER >= 1900)
  typedef std::unordered_map<std::string_view, V> Type;
#elif defined(_MSC_VER)
  typedef hash_map<std::string_view, V, StringViewCmp> Type;
#else
  typedef hash_map<std::string_view, V> Type;
#endif
};

#endif // NINJA_MAP_H_
