// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_UTILS_FNV_HASH_H
#define DOSBOX_UTILS_FNV_HASH_H

#include <cstddef>
#include <cstdint>
#include <string_view>

// FNV-1a, 64-bit. For cheap "did this change" signals (screen text and
// frame ETags) - not a cryptographic hash, and not collision-resistant
// against an adversarial input.
constexpr uint64_t FnvOffsetBasis64 = 0xcbf29ce484222325ULL;
constexpr uint64_t FnvPrime64       = 0x100000001b3ULL;

constexpr uint64_t Fnv1aHash(const uint8_t* data, const size_t len,
                             const uint64_t seed = FnvOffsetBasis64)
{
	uint64_t hash = seed;
	for (size_t i = 0; i < len; ++i) {
		hash ^= data[i];
		hash *= FnvPrime64;
	}
	return hash;
}

inline uint64_t Fnv1aHash(const std::string_view data)
{
	return Fnv1aHash(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

#endif // DOSBOX_UTILS_FNV_HASH_H
