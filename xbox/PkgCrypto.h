// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "PkgExtractor.h"
#include <span>
namespace Lab::PkgCrypto {
Bytes Rsa(std::span<const unsigned char> input, const RsaFields& fields);
Bytes Hash(std::span<const unsigned char> input, std::span<const unsigned char> key = {});
Bytes Aes(std::span<const unsigned char> input, std::span<const unsigned char> key,
          std::span<const unsigned char> iv = {}, bool encrypt = false);
Bytes Xts(std::span<const unsigned char> input, std::span<const unsigned char> keys, uint64_t sector);
}
