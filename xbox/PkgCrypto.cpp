// SPDX-License-Identifier: GPL-2.0-or-later
#include "PkgCrypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <stdexcept>
#include <cstring>

namespace Lab::PkgCrypto {
namespace {
void Check(NTSTATUS result) {
    if (result < 0) throw std::runtime_error("Operacao criptografica falhou: chave ou dados invalidos.");
}
struct Algorithm {
    BCRYPT_ALG_HANDLE handle{};
    Algorithm(LPCWSTR name, ULONG flags = 0) { Check(BCryptOpenAlgorithmProvider(&handle, name, nullptr, flags)); }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    Algorithm(const Algorithm&) = delete;
};
struct Key {
    BCRYPT_KEY_HANDLE handle{};
    ~Key() { if (handle) BCryptDestroyKey(handle); }
};
auto Mutable(std::span<const unsigned char> data) { return const_cast<PUCHAR>(data.data()); }
struct Cipher {
    Algorithm algorithm{BCRYPT_AES_ALGORITHM};
    Key key;
    Cipher(std::span<const unsigned char> secret, bool cbc) {
        auto mode = cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
        Check(BCryptSetProperty(algorithm.handle, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
            static_cast<ULONG>((wcslen(mode) + 1) * sizeof(wchar_t)), 0));
        Check(BCryptGenerateSymmetricKey(algorithm.handle, &key.handle, nullptr, 0,
            Mutable(secret), static_cast<ULONG>(secret.size()), 0));
    }
    Bytes Run(std::span<const unsigned char> data, std::span<const unsigned char> iv, bool encrypt) {
        if (data.empty() || data.size() % 16 || data.size() > UINT32_MAX)
            throw std::runtime_error("Comprimento AES invalido.");
        Bytes output(data.size()), localIv(iv.begin(), iv.end());
        ULONG count{};
        auto operation = encrypt ? BCryptEncrypt : BCryptDecrypt;
        Check(operation(key.handle, Mutable(data), static_cast<ULONG>(data.size()), nullptr,
            localIv.empty() ? nullptr : localIv.data(), static_cast<ULONG>(localIv.size()),
            output.data(), static_cast<ULONG>(output.size()), &count, 0));
        if (count != output.size()) throw std::runtime_error("Saida AES incompleta.");
        return output;
    }
};
}

Bytes Rsa(std::span<const unsigned char> input, const RsaFields& fields) {
    auto field = [&](const char* name, size_t size) -> const Bytes& {
        auto found = fields.find(name);
        if (found == fields.end() || found->second.size() != size)
            throw std::runtime_error("Chaves locais ausentes ou incompletas. Importe keys.json.");
        return found->second;
    };
    const auto& e = field("PublicExponent", 4);
    const auto& n = field("Modulus", 256);
    const auto& p = field("Prime1", 128);
    const auto& q = field("Prime2", 128);
    const auto& dp = field("Exponent1", 128);
    const auto& dq = field("Exponent2", 128);
    const auto& iq = field("Coefficient", 128);
    const auto& d = field("PrivateExponent", 256);
    if (input.size() != 256) throw std::runtime_error("Bloco RSA invalido.");
    BCRYPT_RSAKEY_BLOB header{BCRYPT_RSAPRIVATE_MAGIC, 2048, 4, 256, 128, 128};
    Bytes blob(sizeof(header));
    memcpy(blob.data(), &header, sizeof(header));
    for (const auto* part : {&e, &n, &p, &q, &dp, &dq, &iq, &d}) blob.insert(blob.end(), part->begin(), part->end());
    Algorithm algorithm{BCRYPT_RSA_ALGORITHM};
    Key key;
    auto imported = BCryptImportKeyPair(algorithm.handle, nullptr, BCRYPT_RSAPRIVATE_BLOB,
        &key.handle, blob.data(), static_cast<ULONG>(blob.size()), 0);
    SecureZeroMemory(blob.data(), blob.size());
    Check(imported);
    Bytes result(256);
    ULONG count{};
    Check(BCryptDecrypt(key.handle, Mutable(input), 256, nullptr, nullptr, 0,
        result.data(), 256, &count, BCRYPT_PAD_PKCS1));
    if (count != 32) throw std::runtime_error("Chave PKG decifrada tem tamanho inesperado.");
    result.resize(count);
    return result;
}
Bytes Hash(std::span<const unsigned char> input, std::span<const unsigned char> key) {
    Algorithm algorithm{BCRYPT_SHA256_ALGORITHM, key.empty() ? 0u : BCRYPT_ALG_HANDLE_HMAC_FLAG};
    Bytes result(32);
    Check(BCryptHash(algorithm.handle, Mutable(key), static_cast<ULONG>(key.size()),
        Mutable(input), static_cast<ULONG>(input.size()), result.data(), 32));
    return result;
}
Bytes Aes(std::span<const unsigned char> input, std::span<const unsigned char> key,
          std::span<const unsigned char> iv, bool encrypt) {
    if (key.size() != 16 || (!iv.empty() && iv.size() != 16)) throw std::runtime_error("Chave AES invalida.");
    Cipher cipher(key, !iv.empty());
    return cipher.Run(input, iv, encrypt);
}
Bytes Xts(std::span<const unsigned char> input, std::span<const unsigned char> keys, uint64_t sector) {
    if (keys.size() != 32 || input.size() % 4096) throw std::runtime_error("Bloco PFS invalido.");
    Cipher tweakCipher(keys.first(16), false), dataCipher(keys.subspan(16), false);
    Bytes output(input.size());
    for (size_t offset = 0; offset < input.size(); offset += 4096, ++sector) {
        std::array<unsigned char, 16> number{};
        for (unsigned i = 0; i < 8; ++i) number[i] = static_cast<unsigned char>(sector >> (i * 8));
        auto tweak = tweakCipher.Run(number, {}, true);
        Bytes masks(4096), whitened(4096);
        for (size_t block = 0; block < 4096; block += 16) {
            for (size_t i = 0; i < 16; ++i) {
                masks[block + i] = tweak[i];
                whitened[block + i] = input[offset + block + i] ^ tweak[i];
            }
            unsigned carry = 0;
            for (auto& byte : tweak) { auto next = byte >> 7; byte = static_cast<unsigned char>((byte << 1) | carry); carry = next; }
            if (carry) tweak[0] ^= 0x87;
        }
        auto plain = dataCipher.Run(whitened, {}, false);
        for (size_t i = 0; i < 4096; ++i) output[offset + i] = plain[i] ^ masks[i];
    }
    return output;
}
}

void Lab::ValidatePackageKeys(const PackageKeys& keys) {
    for (const auto* set : {&keys.derived, &keys.fake}) {
        for (auto [name, size] : {std::pair{"PublicExponent", 4u}, {"Modulus", 256u}, {"Prime1", 128u}, {"Prime2", 128u}, {"Exponent1", 128u}, {"Exponent2", 128u}, {"Coefficient", 128u}, {"PrivateExponent", 256u}}) {
            auto found = set->find(name);
            if (found == set->end() || found->second.size() != size)
                throw std::runtime_error("keys.json deve conter PkgDerivedKey3Keyset e FakeKeyset RSA-2048 completos.");
        }
    }
}
