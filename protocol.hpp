#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace udp_exchange {

inline constexpr std::uint32_t kMagic = 0x55445058; // "UDPX"
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint16_t kNodeCount = 10;
inline constexpr std::uint16_t kDefaultPort = 5000;
inline constexpr const char* kDefaultMulticastGroup = "239.255.0.1";
inline constexpr std::size_t kMinPayloadSize = 1 * 1024;
inline constexpr std::size_t kMaxPayloadSize = 50 * 1024;
inline constexpr std::size_t kHeaderWireSize = 28;

enum class PacketType : std::uint16_t {
    Data = 1,
    Stop = 2,
};

struct PacketHeader {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kProtocolVersion;
    PacketType type = PacketType::Data;
    std::uint16_t node_id = 0;
    std::uint16_t node_count = kNodeCount;
    std::uint64_t cycle_id = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t sequence_number = 0;
};

inline void write_u16(
    std::array<std::uint8_t, kHeaderWireSize>& buffer,
    std::size_t offset,
    std::uint16_t value)
{
    buffer[offset] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    buffer[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
}

inline void write_u32(
    std::array<std::uint8_t, kHeaderWireSize>& buffer,
    std::size_t offset,
    std::uint32_t value)
{
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t shift = (3 - i) * 8;
        buffer[offset + i] =
            static_cast<std::uint8_t>((value >> shift) & 0xFF);
    }
}

inline void write_u64(
    std::array<std::uint8_t, kHeaderWireSize>& buffer,
    std::size_t offset,
    std::uint64_t value)
{
    for (std::size_t i = 0; i < 8; ++i) {
        const std::size_t shift = (7 - i) * 8;
        buffer[offset + i] =
            static_cast<std::uint8_t>((value >> shift) & 0xFF);
    }
}

inline std::uint16_t read_u16(const std::uint8_t* buffer, std::size_t offset)
{
    return (static_cast<std::uint16_t>(buffer[offset]) << 8) |
        static_cast<std::uint16_t>(buffer[offset + 1]);
}

inline std::uint32_t read_u32(const std::uint8_t* buffer, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | buffer[offset + i];
    }
    return value;
}

inline std::uint64_t read_u64(const std::uint8_t* buffer, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | buffer[offset + i];
    }
    return value;
}

inline std::array<std::uint8_t, kHeaderWireSize>
serialize_header(const PacketHeader& header)
{
    std::array<std::uint8_t, kHeaderWireSize> buffer{};
    write_u32(buffer, 0, header.magic);
    write_u16(buffer, 4, header.version);
    write_u16(buffer, 6, static_cast<std::uint16_t>(header.type));
    write_u16(buffer, 8, header.node_id);
    write_u16(buffer, 10, header.node_count);
    write_u64(buffer, 12, header.cycle_id);
    write_u32(buffer, 20, header.payload_size);
    write_u32(buffer, 24, header.sequence_number);
    return buffer;
}

inline bool is_valid_header(const PacketHeader& header)
{
    if (header.magic != kMagic || header.version != kProtocolVersion) {
        return false;
    }
    if (header.node_count != kNodeCount || header.node_id < 1 ||
        header.node_id > kNodeCount) {
        return false;
    }
    if (header.type == PacketType::Data) {
        return header.payload_size >= kMinPayloadSize &&
            header.payload_size <= kMaxPayloadSize;
    }
    if (header.type == PacketType::Stop) {
        return header.payload_size == 0;
    }
    return false;
}

inline bool deserialize_header(
    const std::uint8_t* data,
    std::size_t data_size,
    PacketHeader& header)
{
    if (data == nullptr || data_size < kHeaderWireSize) {
        return false;
    }
    header.magic = read_u32(data, 0);
    header.version = read_u16(data, 4);
    header.type = static_cast<PacketType>(read_u16(data, 6));
    header.node_id = read_u16(data, 8);
    header.node_count = read_u16(data, 10);
    header.cycle_id = read_u64(data, 12);
    header.payload_size = read_u32(data, 20);
    header.sequence_number = read_u32(data, 24);
    return is_valid_header(header) &&
        data_size == kHeaderWireSize + header.payload_size;
}

} // namespace udp_exchange
