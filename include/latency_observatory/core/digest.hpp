// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace latobs::core {

/// FNV-1a 64: the deterministic content-addressing function used by the
/// identity vocabulary. It is not a cryptographic hash and is never used for
/// integrity checking.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view data) noexcept;

/// CRC-32C (Castagnoli), reflected, used for record integrity in the store.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline std::uint32_t crc32c(std::string_view data) noexcept {
  return crc32c(data.data(), data.size());
}

/// Streaming SHA-256, used for content digests and segment integrity.
class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;

  Sha256() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view data) noexcept { update(data.data(), data.size()); }
  /// Finalizes the digest. The object must not be updated afterwards.
  void finish(std::uint8_t out[kDigestBytes]) noexcept;
  [[nodiscard]] std::string hex();

 private:
  void transform(const std::uint8_t block[64]) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_count_;
  std::uint8_t buffer_[64];
  std::size_t buffer_size_;
};

/// Convenience: hex encoded SHA-256 of a buffer.
[[nodiscard]] std::string sha256_hex(std::string_view data);

/// Lowercase hex encoding of an arbitrary buffer.
[[nodiscard]] std::string to_hex(const std::uint8_t* data, std::size_t size);
[[nodiscard]] inline std::string to_hex(std::string_view data) {
  return to_hex(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

}  // namespace latobs::core
