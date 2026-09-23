// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "latency_observatory/core/digest.hpp"

#include <array>
#include <cstring>

namespace latobs::core {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

constexpr std::uint32_t kCrc32cTable[256] = {
    0x00000000u, 0xF26B8303u, 0xE13B70F7u, 0x1350F3F4u, 0xC79A971Fu, 0x35F1141Cu, 0x26A1E7E8u,
    0xD4CA64EBu, 0x8AD958CFu, 0x78B2DBCCu, 0x6BE22838u, 0x9989AB3Bu, 0x4D43CFD0u, 0xBF284CD3u,
    0xAC78BF27u, 0x5E133C24u, 0x105EC76Fu, 0xE235446Cu, 0xF165B798u, 0x030E349Bu, 0xD7C45070u,
    0x25AFD373u, 0x36FF2087u, 0xC494A384u, 0x9A879FA0u, 0x68EC1CA3u, 0x7BBCEF57u, 0x89D76C54u,
    0x5D1D08BFu, 0xAF768BBCu, 0xBC267848u, 0x4E4DFB4Bu, 0x20BD8EDEu, 0xD2D60DDDu, 0xC186FE29u,
    0x33ED7D2Au, 0xE72719C1u, 0x154C9AC2u, 0x061C6936u, 0xF477EA35u, 0xAA64D611u, 0x580F5512u,
    0x4B5FA6E6u, 0xB93425E5u, 0x6DFE410Eu, 0x9F95C20Du, 0x8CC531F9u, 0x7EAEB2FAu, 0x30E349B1u,
    0xC288CAB2u, 0xD1D83946u, 0x23B3BA45u, 0xF779DEAEu, 0x05125DADu, 0x1642AE59u, 0xE4292D5Au,
    0xBA3A117Eu, 0x4851927Du, 0x5B016189u, 0xA96AE28Au, 0x7DA08661u, 0x8FCB0562u, 0x9C9BF696u,
    0x6EF07595u, 0x417B1DBCu, 0xB3109EBFu, 0xA0406D4Bu, 0x522BEE48u, 0x86E18AA3u, 0x748A09A0u,
    0x67DAFA54u, 0x95B17957u, 0xCBA24573u, 0x39C9C670u, 0x2A993584u, 0xD8F2B687u, 0x0C38D26Cu,
    0xFE53516Fu, 0xED03A29Bu, 0x1F682198u, 0x5125DAD3u, 0xA34E59D0u, 0xB01EAA24u, 0x42752927u,
    0x96BF4DCCu, 0x64D4CECFu, 0x77843D3Bu, 0x85EFBE38u, 0xDBFC821Cu, 0x2997011Fu, 0x3AC7F2EBu,
    0xC8AC71E8u, 0x1C661503u, 0xEE0D9600u, 0xFD5D65F4u, 0x0F36E6F7u, 0x61C69362u, 0x93AD1061u,
    0x80FDE395u, 0x72966096u, 0xA65C047Du, 0x5437877Eu, 0x4767748Au, 0xB50CF789u, 0xEB1FCBADu,
    0x197448AEu, 0x0A24BB5Au, 0xF84F3859u, 0x2C855CB2u, 0xDEEEDFB1u, 0xCDBE2C45u, 0x3FD5AF46u,
    0x7198540Du, 0x83F3D70Eu, 0x90A324FAu, 0x62C8A7F9u, 0xB602C312u, 0x44694011u, 0x5739B3E5u,
    0xA55230E6u, 0xFB410CC2u, 0x092A8FC1u, 0x1A7A7C35u, 0xE811FF36u, 0x3CDB9BDDu, 0xCEB018DEu,
    0xDDE0EB2Au, 0x2F8B6829u, 0x82F63B78u, 0x709DB87Bu, 0x63CD4B8Fu, 0x91A6C88Cu, 0x456CAC67u,
    0xB7072F64u, 0xA457DC90u, 0x563C5F93u, 0x082F63B7u, 0xFA44E0B4u, 0xE9141340u, 0x1B7F9043u,
    0xCFB5F4A8u, 0x3DDE77ABu, 0x2E8E845Fu, 0xDCE5075Cu, 0x92A8FC17u, 0x60C37F14u, 0x73938CE0u,
    0x81F80FE3u, 0x55326B08u, 0xA759E80Bu, 0xB4091BFFu, 0x466298FCu, 0x1871A4D8u, 0xEA1A27DBu,
    0xF94AD42Fu, 0x0B21572Cu, 0xDFEB33C7u, 0x2D80B0C4u, 0x3ED04330u, 0xCCBBC033u, 0xA24BB5A6u,
    0x502036A5u, 0x4370C551u, 0xB11B4652u, 0x65D122B9u, 0x97BAA1BAu, 0x84EA524Eu, 0x7681D14Du,
    0x2892ED69u, 0xDAF96E6Au, 0xC9A99D9Eu, 0x3BC21E9Du, 0xEF087A76u, 0x1D63F975u, 0x0E330A81u,
    0xFC588982u, 0xB21572C9u, 0x407EF1CAu, 0x532E023Eu, 0xA145813Du, 0x758FE5D6u, 0x87E466D5u,
    0x94B49521u, 0x66DF1622u, 0x38CC2A06u, 0xCAA7A905u, 0xD9F75AF1u, 0x2B9CD9F2u, 0xFF56BD19u,
    0x0D3D3E1Au, 0x1E6DCDEEu, 0xEC064EEDu, 0xC38D26C4u, 0x31E6A5C7u, 0x22B65633u, 0xD0DDD530u,
    0x0417B1DBu, 0xF67C32D8u, 0xE52CC12Cu, 0x1747422Fu, 0x49547E0Bu, 0xBB3FFD08u, 0xA86F0EFCu,
    0x5A048DFFu, 0x8ECEE914u, 0x7CA56A17u, 0x6FF599E3u, 0x9D9E1AE0u, 0xD3D3E1ABu, 0x21B862A8u,
    0x32E8915Cu, 0xC083125Fu, 0x144976B4u, 0xE622F5B7u, 0xF5720643u, 0x07198540u, 0x590AB964u,
    0xAB613A67u, 0xB831C993u, 0x4A5A4A90u, 0x9E902E7Bu, 0x6CFBAD78u, 0x7FAB5E8Cu, 0x8DC0DD8Fu,
    0xE330A81Au, 0x115B2B19u, 0x020BD8EDu, 0xF0605BEEu, 0x24AA3F05u, 0xD6C1BC06u, 0xC5914FF2u,
    0x37FACCF1u, 0x69E9F0D5u, 0x9B8273D6u, 0x88D28022u, 0x7AB90321u, 0xAE7367CAu, 0x5C18E4C9u,
    0x4F48173Du, 0xBD23943Eu, 0xF36E6F75u, 0x0105EC76u, 0x12551F82u, 0xE03E9C81u, 0x34F4F86Au,
    0xC69F7B69u, 0xD5CF889Du, 0x27A40B9Eu, 0x79B737BAu, 0x8BDCB4B9u, 0x988C474Du, 0x6AE7C44Eu,
    0xBE2DA0A5u, 0x4C4623A6u, 0x5F16D052u, 0xAD7D5351u};

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

}  // namespace

std::uint64_t fnv1a64(std::string_view data) noexcept {
  std::uint64_t hash = kFnvOffset;
  for (const char c : data) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;
  }
  return hash;
}

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t index = 0; index < size; ++index) {
    crc = kCrc32cTable[(crc ^ bytes[index]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu,
             0x1f83d9abu, 0x5be0cd19u},
      bit_count_(0),
      buffer_{},
      buffer_size_(0) {}

void Sha256::transform(const std::uint8_t block[64]) noexcept {
  std::uint32_t w[64];
  for (int index = 0; index < 16; ++index) {
    const std::size_t offset = static_cast<std::size_t>(index) * 4u;
    w[index] = (static_cast<std::uint32_t>(block[offset]) << 24) |
               (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
               (static_cast<std::uint32_t>(block[offset + 3]));
  }
  for (int index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(w[index - 15], 7) ^ rotr(w[index - 15], 18) ^ (w[index - 15] >> 3);
    const std::uint32_t s1 = rotr(w[index - 2], 17) ^ rotr(w[index - 2], 19) ^ (w[index - 2] >> 10);
    w[index] = w[index - 16] + s0 + w[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (int index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256K[index] + w[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  bit_count_ += static_cast<std::uint64_t>(size) * 8ULL;
  std::size_t index = 0;
  if (buffer_size_ != 0) {
    while (buffer_size_ < 64 && index < size) {
      buffer_[buffer_size_++] = bytes[index++];
    }
    if (buffer_size_ == 64) {
      transform(buffer_);
      buffer_size_ = 0;
    }
  }
  while (size - index >= 64) {
    transform(bytes + index);
    index += 64;
  }
  while (index < size) {
    buffer_[buffer_size_++] = bytes[index++];
  }
}

void Sha256::finish(std::uint8_t out[kDigestBytes]) noexcept {
  const std::uint64_t bit_count = bit_count_;
  const std::uint8_t padding = 0x80;
  update(&padding, 1);
  const std::uint8_t zero = 0x00;
  while (buffer_size_ != 56) {
    update(&zero, 1);
  }
  std::uint8_t length_bytes[8];
  for (int index = 0; index < 8; ++index) {
    length_bytes[index] = static_cast<std::uint8_t>((bit_count >> (56 - 8 * index)) & 0xFFULL);
  }
  // Writing the length directly avoids re-counting the padding bits.
  for (int index = 0; index < 8; ++index) {
    buffer_[buffer_size_++] = length_bytes[index];
  }
  transform(buffer_);
  buffer_size_ = 0;
  for (int index = 0; index < 8; ++index) {
    out[index * 4 + 0] = static_cast<std::uint8_t>((state_[static_cast<std::size_t>(index)] >> 24) & 0xFFu);
    out[index * 4 + 1] = static_cast<std::uint8_t>((state_[static_cast<std::size_t>(index)] >> 16) & 0xFFu);
    out[index * 4 + 2] = static_cast<std::uint8_t>((state_[static_cast<std::size_t>(index)] >> 8) & 0xFFu);
    out[index * 4 + 3] = static_cast<std::uint8_t>(state_[static_cast<std::size_t>(index)] & 0xFFu);
  }
}

std::string Sha256::hex() {
  std::uint8_t digest[kDigestBytes];
  finish(digest);
  return to_hex(digest, kDigestBytes);
}

std::string sha256_hex(std::string_view data) {
  Sha256 hash;
  hash.update(data);
  return hash.hex();
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    out[index * 2] = kDigits[(data[index] >> 4) & 0x0Fu];
    out[index * 2 + 1] = kDigits[data[index] & 0x0Fu];
  }
  return out;
}

}  // namespace latobs::core
