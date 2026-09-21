// SPDX-License-Identifier: GPL-2.0-or-later
#include "CoreProbe.h"

// These are original shadPS4 headers, compiled by the UWP target rather than copied.
#include "common/endian.h"
#include "core/file_format/psf.h"

#include <bit>
#include <cstring>

namespace Lab {

CoreProbeResult ProbeUpstreamCoreTypes() {
    static_assert(sizeof(void*) == 8);
    static_assert(sizeof(PSFHeader) == 0x14);
    static_assert(sizeof(PSFRawEntry) == 0x10);
    static_assert(std::endian::native == std::endian::little);

    PSFHeader header{};
    header.magic = PSF_MAGIC;
    header.version = PSF_VERSION_1_1;
    header.index_table_entries = 1;

    std::uint32_t raw_magic{};
    std::memcpy(&raw_magic, &header.magic, sizeof(raw_magic));

    CoreProbeResult result;
    result.psf_header_size = sizeof(PSFHeader);
    result.psf_entry_size = sizeof(PSFRawEntry);
    result.decoded_magic = static_cast<u32>(header.magic);
    result.stored_magic = raw_magic;
    PSF source;
    source.AddString("TITLE_ID", "HB000000001");
    source.AddInteger("APP_VER", 42);
    const auto encoded = source.Encode();
    PSF decoded;
    const bool decoded_ok = decoded.Open(encoded);
    const auto title = decoded.GetString("TITLE_ID");
    const auto number = decoded.GetInteger("APP_VER");
    result.encoded_size = static_cast<std::uint32_t>(encoded.size());
    result.decoded_integer = number.value_or(-1);
    result.passed = result.decoded_magic == PSF_MAGIC &&
                    result.psf_header_size == 0x14 && result.psf_entry_size == 0x10 &&
                    raw_magic == std::byteswap(PSF_MAGIC) && decoded_ok &&
                    title == "HB000000001" && number == 42;
    return result;
}

} // namespace Lab
