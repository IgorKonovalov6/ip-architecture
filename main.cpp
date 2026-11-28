#include "protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct NodeConfig {
    std::uint16_t node_id = 0;
    std::string interface_ip;
    std::size_t payload_size = udp_exchange::kMinPayloadSize;
    std::uint64_t cycle_count = 1000;
};

struct SendResult {
    std::size_t bytes_sent = 0;
    std::int64_t send_time_ns = 0;
};

struct ReceiveResult {
    bool timed_out = false;
    bool valid = false;
    udp_exchange::PacketHeader header;
    std::int64_t receive_time_ns = 0;
};

struct CycleMetrics {
    TimePoint started = Clock::now();
    std::int64_t send_time_ns = 0;
    std::int64_t processing_time_ns = 0;
    std::int64_t receive_time_ns = 0;
    std::uint16_t seen_nodes_mask = 0;
    std::uint32_t duplicate_packets = 0;
    std::uint32_t retries = 0;
    bool sent = false;
};

struct CycleRecord {
    std::uint64_t cycle_id = 0;
    std::int64_t total_cycle_ns = 0;
    std::int64_t send_time_ns = 0;
    std::int64_t processing_time_ns = 0;
    std::int64_t receive_time_ns = 0;
    std::uint32_t unique_packets = 0;
    std::uint32_t duplicate_packets = 0;
    std::uint32_t retries = 0;
    bool complete = false;
};

struct MetricSummary {
    std::int64_t minimum_ns = 0;
    std::int64_t maximum_ns = 0;
    long double average_ns = 0.0;
};

volatile std::uint64_t g_processing_sink = 0;

NodeConfig parse_arguments(int argc, char* argv[])
{
    if (argc < 3 || argc > 5) {
        throw std::runtime_error(
            "Использование: udp_node NODE_ID INTERFACE_IP "
            "[PAYLOAD_SIZE] [CYCLE_COUNT]");
    }

    NodeConfig config;
    const int node_id = std::stoi(argv[1]);
    if (node_id < 1 || node_id > udp_exchange::kNodeCount) {
        throw std::runtime_error("Номер узла должен быть от 1 до 10");
    }
    config.node_id = static_cast<std::uint16_t>(node_id);
    config.interface_ip = argv[2];

    if (argc >= 4) {
        config.payload_size = static_cast<std::size_t>(std::stoull(argv[3]));
    }
    if (config.payload_size < udp_exchange::kMinPayloadSize ||
        config.payload_size > udp_exchange::kMaxPayloadSize) {
        throw std::runtime_error("Размер массива должен быть от 1024 до 51200 байт");
    }

    if (argc >= 5) {
        config.cycle_count = std::stoull(argv[4]);
    }
    if (config.cycle_count == 0) {
        throw std::runtime_error("Количество циклов должно быть больше нуля");
    }

    return config;
}

int create_multicast_socket(
    const NodeConfig& config,
    sockaddr_in& multicast_address)
{
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }

    const int enabled = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        close(socket_fd);
        throw std::runtime_error(std::string("SO_REUSEADDR: ") + std::strerror(errno));
    }
#ifdef SO_REUSEPORT
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) < 0) {
        close(socket_fd);
        throw std::runtime_error(std::string("SO_REUSEPORT: ") + std::strerror(errno));
    }
#endif

    const int socket_buffer_size = 4 * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF,
            &socket_buffer_size, sizeof(socket_buffer_size)) < 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF,
            &socket_buffer_size, sizeof(socket_buffer_size)) < 0) {
        close(socket_fd);
        throw std::runtime_error(std::string("Размер буфера сокета: ") + std::strerror(errno));
    }

    sockaddr_in local_address{};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(udp_exchange::kDefaultPort);
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<sockaddr*>(&local_address),
            sizeof(local_address)) < 0) {
        close(socket_fd);
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    }

    ip_mreq membership{};
    if (inet_pton(AF_INET, udp_exchange::kDefaultMulticastGroup,
            &membership.imr_multiaddr) != 1 ||
        inet_pton(AF_INET, config.interface_ip.c_str(),
            &membership.imr_interface) != 1) {
        close(socket_fd);
        throw std::runtime_error("Некорректный multicast-адрес или IP интерфейса");
    }
    if (setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
            &membership, sizeof(membership)) < 0 ||
        setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF,
            &membership.imr_interface, sizeof(membership.imr_interface)) < 0) {
        close(socket_fd);
        throw std::runtime_error(std::string("Multicast: ") + std::strerror(errno));
    }

    const unsigned char ttl = 1;
    const unsigned char loopback = 1;
    if (setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0 ||
        setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_LOOP,
            &loopback, sizeof(loopback)) < 0) {
        close(socket_fd);
        throw std::runtime_error(std::string("Параметры multicast: ") + std::strerror(errno));
    }

    multicast_address = {};
    multicast_address.sin_family = AF_INET;
    multicast_address.sin_port = htons(udp_exchange::kDefaultPort);
    if (inet_pton(AF_INET, udp_exchange::kDefaultMulticastGroup,
            &multicast_address.sin_addr) != 1) {
        close(socket_fd);
        throw std::runtime_error("Не удалось сформировать адрес multicast-группы");
    }
    return socket_fd;
}

std::vector<std::uint8_t> create_send_buffer(const NodeConfig& config)
{
    std::vector<std::uint8_t> buffer(
        udp_exchange::kHeaderWireSize + config.payload_size);
    for (std::size_t i = 0; i < config.payload_size; ++i) {
        buffer[udp_exchange::kHeaderWireSize + i] =
            static_cast<std::uint8_t>((config.node_id * 31U + i) & 0xFFU);
    }
    return buffer;
}

SendResult send_packet(
    int socket_fd,
    const sockaddr_in& destination,
    const NodeConfig& config,
    std::vector<std::uint8_t>& send_buffer,
    udp_exchange::PacketType type,
    std::uint64_t cycle_id,
    std::uint32_t sequence_number)
{
    udp_exchange::PacketHeader header;
    header.type = type;
    header.node_id = config.node_id;
    header.cycle_id = cycle_id;
    header.payload_size = type == udp_exchange::PacketType::Data
        ? static_cast<std::uint32_t>(config.payload_size)
        : 0;
    header.sequence_number = sequence_number;

    const auto wire_header = udp_exchange::serialize_header(header);
    std::copy(wire_header.begin(), wire_header.end(), send_buffer.begin());
    const std::size_t packet_size =
        udp_exchange::kHeaderWireSize + header.payload_size;

    const auto started = Clock::now();
    const ssize_t sent = sendto(
        socket_fd, send_buffer.data(), packet_size, 0,
        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    const auto finished = Clock::now();

    if (sent < 0) {
        throw std::runtime_error(std::string("sendto: ") + std::strerror(errno));
    }
    if (static_cast<std::size_t>(sent) != packet_size) {
        throw std::runtime_error("UDP-пакет отправлен не полностью");
    }

    return {
        static_cast<std::size_t>(sent),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            finished - started).count()
    };
}

ReceiveResult receive_packet(
    int socket_fd,
    std::vector<std::uint8_t>& receive_buffer,
    int timeout_ms)
{
    pollfd descriptor{};
    descriptor.fd = socket_fd;
    descriptor.events = POLLIN;

    int poll_result = 0;
    do {
        poll_result = poll(&descriptor, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);

    if (poll_result < 0) {
        throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
    }
    if (poll_result == 0) {
        ReceiveResult result;
        result.timed_out = true;
        return result;
    }
    if ((descriptor.revents & POLLIN) == 0) {
        return {};
    }

    const auto started = Clock::now();
    const ssize_t received = recvfrom(
        socket_fd, receive_buffer.data(), receive_buffer.size(), 0, nullptr, nullptr);
    const auto finished = Clock::now();
    if (received < 0) {
        throw std::runtime_error(std::string("recvfrom: ") + std::strerror(errno));
    }

    ReceiveResult result;
    result.receive_time_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            finished - started).count();
    result.valid = udp_exchange::deserialize_header(
        receive_buffer.data(), static_cast<std::size_t>(received), result.header);
    return result;
}

std::int64_t process_payload(const std::uint8_t* data, std::size_t size)
{
    const auto started = Clock::now();
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    g_processing_sink ^= hash;
    const auto finished = Clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        finished - started).count();
}

std::uint32_t unique_packet_count(std::uint16_t mask)
{
    return static_cast<std::uint32_t>(std::bitset<16>(mask).count());
}

CycleRecord make_cycle_record(
    std::uint64_t cycle_id,
    const CycleMetrics& metrics)
{
    const std::uint32_t unique = unique_packet_count(metrics.seen_nodes_mask);
    return {
        cycle_id,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - metrics.started).count(),
        metrics.send_time_ns,
        metrics.processing_time_ns,
        metrics.receive_time_ns,
        unique,
        metrics.duplicate_packets,
        metrics.retries,
        unique == udp_exchange::kNodeCount - 1,
    };
}

template <typename Getter>
MetricSummary summarize_metric(
    const std::vector<CycleRecord>& records,
    Getter getter)
{
    MetricSummary summary;
    if (records.empty()) {
        return summary;
    }

    summary.minimum_ns = getter(records.front());
    summary.maximum_ns = summary.minimum_ns;
    long double sum = 0.0;
    for (const CycleRecord& record : records) {
        const std::int64_t value = getter(record);
        summary.minimum_ns = std::min(summary.minimum_ns, value);
        summary.maximum_ns = std::max(summary.maximum_ns, value);
        sum += static_cast<long double>(value);
    }
    summary.average_ns = sum / static_cast<long double>(records.size());
    return summary;
}

void print_metric_summary(const char* name, const MetricSummary& summary)
{
    std::cout << std::fixed << std::setprecision(3)
              << name
              << ": min=" << static_cast<long double>(summary.minimum_ns) / 1000.0L
              << " us, max=" << static_cast<long double>(summary.maximum_ns) / 1000.0L
              << " us, avg=" << summary.average_ns / 1000.0L << " us\n";
}

void print_terminal_statistics(
    const NodeConfig& config,
    const std::vector<CycleRecord>& records)
{
    if (records.empty()) {
        std::cout << "Нет завершённых измерений\n";
        return;
    }

    const MetricSummary exchange = summarize_metric(
        records, [](const CycleRecord& record) { return record.total_cycle_ns; });
    const MetricSummary sending = summarize_metric(
        records, [](const CycleRecord& record) { return record.send_time_ns; });
    const MetricSummary processing = summarize_metric(
        records, [](const CycleRecord& record) { return record.processing_time_ns; });

    std::cout << "\n========================================\n"
              << "Statistics for node " << config.node_id;
    if (config.node_id == 1) {
        std::cout << " (full system cycle)";
    }
    std::cout << "\nPayload: " << config.payload_size << " bytes\n";
    print_metric_summary("Exchange", exchange);
    print_metric_summary("Send", sending);
    print_metric_summary("Processing", processing);

    struct HistogramRange {
        std::int64_t lower_us;
        std::int64_t upper_us;
    };
    constexpr std::array<HistogramRange, 16> ranges{{
        {0, 20}, {20, 50}, {50, 75}, {75, 100},
        {100, 125}, {125, 150}, {150, 175}, {175, 200},
        {200, 300}, {300, 500}, {500, 1000}, {1000, 2000},
        {2000, 5000}, {5000, 10000}, {10000, 50000},
        {50000, -1},
    }};
    std::array<std::uint64_t, ranges.size()> counts{};

    std::uint64_t measured_ok = 0;
    std::uint64_t within_200_us = 0;
    for (const CycleRecord& record : records) {
        if (record.complete) {
            ++measured_ok;
        }
        if (record.total_cycle_ns <= 200000) {
            ++within_200_us;
        }
        const std::int64_t value_us = record.total_cycle_ns / 1000;
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            if (value_us >= ranges[i].lower_us &&
                (ranges[i].upper_us < 0 || value_us < ranges[i].upper_us)) {
                ++counts[i];
                break;
            }
        }
    }

    std::cout << "\nHistogram (exchange_us):\n";
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        std::cout << "[" << std::setw(7) << ranges[i].lower_us << " ... ";
        if (ranges[i].upper_us < 0) {
            std::cout << std::setw(7) << "inf";
        } else {
            std::cout << std::setw(7) << ranges[i].upper_us;
        }
        std::cout << ") us : " << counts[i] << '\n';
    }
    std::cout << "Total rounds=" << records.size()
              << ", measured_ok=" << measured_ok
              << ", <=200us=" << within_200_us << '\n'
              << "========================================\n";

    std::ofstream histogram_file("histogram.csv", std::ios::trunc);
    if (!histogram_file) {
        throw std::runtime_error("Не удалось создать histogram.csv");
    }
    histogram_file
        << "range_start_us,range_end_us,count,percent,total_rounds,"
           "measured_ok,payload_bytes\n";
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const long double percent =
            static_cast<long double>(counts[i]) * 100.0L /
            static_cast<long double>(records.size());
        histogram_file << ranges[i].lower_us << ',';
        if (ranges[i].upper_us < 0) {
            histogram_file << "inf";
        } else {
            histogram_file << ranges[i].upper_us;
        }
        histogram_file << ',' << counts[i] << ','
                       << std::fixed << std::setprecision(3) << percent << ','
                       << records.size() << ',' << measured_ok << ','
                       << config.payload_size << '\n';
    }
    std::cout << "Histogram saved to histogram.csv\n";
}

void run_node(
    int socket_fd,
    const sockaddr_in& multicast_address,
    const NodeConfig& config)
{
    std::vector<std::uint8_t> send_buffer = create_send_buffer(config);
    std::vector<std::uint8_t> receive_buffer(
        udp_exchange::kHeaderWireSize + udp_exchange::kMaxPayloadSize);
    std::unordered_map<std::uint64_t, CycleMetrics> cycles;
    std::vector<CycleRecord> records;
    records.reserve(static_cast<std::size_t>(config.cycle_count));
    std::optional<std::uint64_t> last_sent_cycle;
    std::optional<std::uint64_t> last_completed_cycle;
    std::uint64_t active_coordinator_cycle = 0;
    std::uint32_t sequence_number = 0;
    int coordinator_timeouts = 0;
    TimePoint last_activity = Clock::now();

    auto send_data = [&](std::uint64_t cycle_id, bool retry) {
        auto [it, inserted] = cycles.try_emplace(cycle_id);
        CycleMetrics& metrics = it->second;
        if (inserted) {
            metrics.started = Clock::now();
        }
        const SendResult result = send_packet(
            socket_fd, multicast_address, config, send_buffer,
            udp_exchange::PacketType::Data, cycle_id, sequence_number++);
        metrics.send_time_ns += result.send_time_ns;
        metrics.sent = true;
        if (retry) {
            ++metrics.retries;
        }
        last_sent_cycle = cycle_id;
    };

    auto resend_completed_cycle = [&](std::uint64_t cycle_id) {
        const SendResult ignored = send_packet(
            socket_fd, multicast_address, config, send_buffer,
            udp_exchange::PacketType::Data, cycle_id, sequence_number++);
        (void)ignored;
    };

    auto send_stop = [&](std::uint64_t cycle_id) {
        for (int i = 0; i < 5; ++i) {
            send_packet(socket_fd, multicast_address, config, send_buffer,
                udp_exchange::PacketType::Stop, cycle_id, sequence_number++);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    if (config.node_id == 1) {
        std::cout << "Ожидание запуска остальных узлов: 2 секунды\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        send_data(0, false);
    }

    while (true) {
        const ReceiveResult received = receive_packet(socket_fd, receive_buffer, 250);

        if (received.timed_out) {
            if (config.node_id == 1 && active_coordinator_cycle < config.cycle_count) {
                if (++coordinator_timeouts > 20) {
                    throw std::runtime_error(
                        "Цикл остановился: после 20 повторов нет ответа от узла 10");
                }
                send_data(active_coordinator_cycle, true);
            } else if (Clock::now() - last_activity > std::chrono::seconds(30)) {
                throw std::runtime_error("Нет сетевой активности более 30 секунд");
            }
            continue;
        }
        if (!received.valid) {
            continue;
        }
        last_activity = Clock::now();
        const auto& header = received.header;

        if (header.type == udp_exchange::PacketType::Stop) {
            if (config.node_id != 1) {
                std::cout << "Получена команда завершения\n";
            }
            break;
        }
        if (header.type != udp_exchange::PacketType::Data ||
            header.payload_size != config.payload_size ||
            header.node_id == config.node_id) {
            continue;
        }

        const std::uint16_t predecessor =
            static_cast<std::uint16_t>(config.node_id - 1);

        if (last_completed_cycle && header.cycle_id <= *last_completed_cycle) {
            if (config.node_id > 1 && header.node_id == predecessor &&
                last_sent_cycle && header.cycle_id == *last_sent_cycle) {
                resend_completed_cycle(header.cycle_id);
            }
            continue;
        }

        auto [it, inserted] = cycles.try_emplace(header.cycle_id);
        CycleMetrics& metrics = it->second;
        if (inserted) {
            metrics.started = Clock::now();
        }
        metrics.receive_time_ns += received.receive_time_ns;

        const std::uint16_t sender_bit = static_cast<std::uint16_t>(
            1U << (header.node_id - 1));
        const bool duplicate = (metrics.seen_nodes_mask & sender_bit) != 0;
        if (duplicate) {
            ++metrics.duplicate_packets;
        } else {
            metrics.seen_nodes_mask |= sender_bit;
            metrics.processing_time_ns += process_payload(
                receive_buffer.data() + udp_exchange::kHeaderWireSize,
                header.payload_size);
        }

        if (config.node_id > 1 && header.node_id == predecessor) {
            send_data(header.cycle_id, metrics.sent);
        }

        const bool cycle_finished =
            (config.node_id == udp_exchange::kNodeCount && metrics.sent) ||
            (config.node_id != udp_exchange::kNodeCount &&
                header.node_id == udp_exchange::kNodeCount);
        if (!cycle_finished) {
            continue;
        }

        records.push_back(make_cycle_record(header.cycle_id, metrics));
        last_completed_cycle = header.cycle_id;
        cycles.erase(header.cycle_id);

        if (config.node_id == 1) {
            coordinator_timeouts = 0;
            const std::uint64_t next_cycle = header.cycle_id + 1;
            if (next_cycle >= config.cycle_count) {
                send_stop(header.cycle_id);
                break;
            }
            active_coordinator_cycle = next_cycle;
            send_data(next_cycle, false);
        }
    }

    if (config.node_id == 1) {
        print_terminal_statistics(config, records);
    }
    std::cout << "Узел " << config.node_id << " завершил работу\n";
}

} // namespace

int main(int argc, char* argv[])
{
    int socket_fd = -1;
    try {
        const NodeConfig config = parse_arguments(argc, argv);
        sockaddr_in multicast_address{};
        socket_fd = create_multicast_socket(config, multicast_address);

        std::cout << "Узел: " << config.node_id << '/' << udp_exchange::kNodeCount
                  << "\nИнтерфейс: " << config.interface_ip
                  << "\nГруппа: " << udp_exchange::kDefaultMulticastGroup
                  << ':' << udp_exchange::kDefaultPort
                  << "\nМассив: " << config.payload_size << " байт"
                  << "\nЦиклы: " << config.cycle_count << "\n";

        run_node(socket_fd, multicast_address, config);
        close(socket_fd);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        if (socket_fd >= 0) {
            close(socket_fd);
        }
        std::cerr << "Ошибка: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
