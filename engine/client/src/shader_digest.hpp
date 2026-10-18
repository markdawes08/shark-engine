#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace shark::client::detail {

// SHA-256 as specified by FIPS 180-4. The bounded bundle loader owns its input;
// hashing allocates no storage proportional to payload size.
inline std::array<std::byte, 32> shader_sha256(std::span<const std::byte> input) noexcept {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U};
    std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    const auto transform = [&](const std::array<std::byte, 64>& block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                words[index] =
                    (words[index] << 8U) | std::to_integer<std::uint32_t>(block[index * 4 + byte]);
            }
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto x = words[index - 15];
            const auto y = words[index - 2];
            const auto a = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3U);
            const auto b = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10U);
            words[index] = words[index - 16] + a + words[index - 7] + b;
        }
        auto work = hash;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sigma1 =
                std::rotr(work[4], 6) ^ std::rotr(work[4], 11) ^ std::rotr(work[4], 25);
            const auto choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
            const auto first = work[7] + sigma1 + choose + constants[index] + words[index];
            const auto sigma0 =
                std::rotr(work[0], 2) ^ std::rotr(work[0], 13) ^ std::rotr(work[0], 22);
            const auto majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
            work = {first + sigma0 + majority, work[0], work[1], work[2],
                    work[3] + first,           work[4], work[5], work[6]};
        }
        for (std::size_t index = 0; index < hash.size(); ++index) {
            hash[index] += work[index];
        }
    };
    std::array<std::byte, 64> block{};
    std::size_t used = 0;
    for (const auto byte : input) {
        block[used++] = byte;
        if (used == block.size()) {
            transform(block);
            block.fill(std::byte{0});
            used = 0;
        }
    }
    block[used++] = std::byte{0x80};
    if (used > 56) {
        transform(block);
        block.fill(std::byte{0});
    }
    const auto bits = static_cast<std::uint64_t>(input.size()) * 8U;
    for (std::uint32_t index = 0; index < 8; ++index) {
        block[63 - index] = static_cast<std::byte>((bits >> (index * 8U)) & 0xffU);
    }
    transform(block);
    std::array<std::byte, 32> result{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
        for (std::uint32_t byte = 0; byte < 4; ++byte) {
            result[index * 4 + byte] =
                static_cast<std::byte>((hash[index] >> ((3U - byte) * 8U)) & 0xffU);
        }
    }
    return result;
}

inline std::string shader_digest_text(const std::array<std::byte, 32>& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned int>(byte);
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 15U]);
    }
    return text;
}

} // namespace shark::client::detail
