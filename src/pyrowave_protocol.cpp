#include "pyrowave_protocol.h"

#include <algorithm>
#include <limits>
#include <map>

namespace pyrowave::protocol {
  namespace {
    void put_le(std::vector<std::uint8_t> &out, std::uint64_t value, unsigned size) {
      for (unsigned i = 0; i < size; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
      }
    }

    std::uint64_t get_le(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned size) {
      std::uint64_t value = 0;
      for (unsigned i = 0; i < size; ++i) {
        value |= std::uint64_t(bytes[offset + i]) << (i * 8);
      }
      return value;
    }

    std::uint32_t parity_shards(std::uint32_t data, const transport_config_t &config) {
      return config.fec_percentage == 0 ? 0 : std::max((data * config.fec_percentage + 99) / 100, config.min_fec_packets);
    }

    std::uint32_t effective_critical_percentage(const transport_config_t &config) {
      return config.fragmented ? std::max(config.fec_percentage, config.critical_fec_percentage) : config.fec_percentage;
    }

    std::optional<std::uint32_t> active_block_count(std::uint32_t width, std::uint32_t height, bool chroma_444, std::uint32_t bands) {
      if (width == 0 || height == 0 || width > max_dimension || height > max_dimension || bands > 4 ||
          (!chroma_444 && ((width | height) & 1))) return std::nullopt;
      if (bands == 0) return 0;
      const auto aligned_width = std::max(128U, (width + 31) & ~31U);
      const auto aligned_height = std::max(128U, (height + 31) & ~31U);
      const auto last_level = 5 - std::max(1U, bands - 1);
      const auto last_subband = bands == 1 ? 0U : 3U;
      std::uint32_t count = 0;
      for (int level = 4; level >= static_cast<int>(last_level); --level) {
        const auto block_width = ((aligned_width >> (level + 1)) + 31) / 32;
        const auto block_height = ((aligned_height >> (level + 1)) + 31) / 32;
        for (unsigned component = 0; component < 3; ++component) {
          for (unsigned subband = level == 4 ? 0 : 1; subband < 4; ++subband) {
            count += block_width * block_height;
            if (component == 2 && level == static_cast<int>(last_level) && subband == last_subband) return count;
          }
        }
      }
      return std::nullopt;
    }

    bool valid_sequence_header(std::span<const std::uint8_t> header) {
      if (header.size() != 8) return false;
      const auto first = get_le(header, 0, 4);
      const auto second = get_le(header, 4, 4);
      const auto width = static_cast<std::uint32_t>((first & 0x3fff) + 1);
      const auto height = static_cast<std::uint32_t>(((first >> 14) & 0x3fff) + 1);
      return (first & 0x80000000U) != 0 && (second & 0x03000000U) == 0 &&
             ((second & 0x04000000U) != 0 || ((width | height) & 1) == 0);
    }

    bool valid_native_packet(std::span<const std::uint8_t> bytes, std::uint32_t index, std::span<const std::uint8_t> sequence_header) {
      std::size_t offset = 0;
      if (index == 0) {
        if (bytes.size() < 8 || !std::equal(sequence_header.begin(), sequence_header.end(), bytes.begin())) return false;
        offset = 8;
      }
      const auto sequence = (get_le(sequence_header, 0, 4) >> 28) & 7;
      while (offset < bytes.size()) {
        if (bytes.size() - offset < 8) return false;
        const auto word = get_le(bytes, offset + 2, 2);
        const auto block_size = (word & 0xfff) * 4;
        if ((word & 0x8000) != 0 || ((word >> 12) & 7) != sequence || block_size < 8 || block_size > bytes.size() - offset) return false;
        offset += static_cast<std::size_t>(block_size);
      }
      return !bytes.empty();
    }

    bool same_frame(const fragment_view_t &a, const fragment_view_t &b) {
      return a.frame_index == b.frame_index && a.timestamp_us == b.timestamp_us && a.shard_count == b.shard_count &&
             a.native_packet_count == b.native_packet_count && a.critical_packet_count == b.critical_packet_count &&
             a.active_block_bands == b.active_block_bands && a.active_block_count == b.active_block_count &&
             a.sequence_header == b.sequence_header;
    }

    bool add_planned_block(transport_plan_t &plan, std::uint32_t data, std::uint32_t percentage, const transport_config_t &config) {
      if (plan.block_count >= plan.blocks.size() || data == 0 || data > 255) return false;
      auto selected = config;
      selected.fec_percentage = percentage;
      const auto parity = parity_shards(data, selected);
      const auto natural = (data * percentage + 99) / 100;
      const auto wire_percentage = parity > natural ? 100 * parity / data : percentage;
      if (data + parity > 255 || wire_percentage > 255 || (data * wire_percentage + 99) / 100 != parity) return false;
      plan.blocks[plan.block_count++] = {data, parity, percentage};
      plan.data_shards += data;
      const auto wire_bytes = config.packet_size + 16 + (config.encrypted ? 32 : 0) + config.ip_header_size + 8;
      plan.wire_bytes += (data + parity) * wire_bytes;
      return true;
    }
  }  // namespace

  std::optional<std::vector<std::uint8_t>> serialize_frame(
    std::uint64_t frame_index,
    std::uint64_t timestamp_us,
    std::span<const std::span<const std::uint8_t>> packets,
    std::size_t byte_limit
  ) {
    byte_limit = std::min(byte_limit, max_frame_size);
    if (packets.empty() || packets.size() > max_native_packets || byte_limit < frame_header_size) {
      return std::nullopt;
    }
    std::size_t total = frame_header_size;
    for (const auto packet : packets) {
      if (packet.empty() || packet.size() % 4 != 0 || byte_limit - total < 4 || packet.size() > byte_limit - total - 4) {
        return std::nullopt;
      }
      total += 4 + packet.size();
    }

    std::vector<std::uint8_t> out;
    out.reserve(total);
    out.insert(out.end(), {'P', 'W', 'V', 'F'});
    put_le(out, version, 2);
    put_le(out, frame_header_size, 2);
    put_le(out, total, 4);
    put_le(out, packets.size(), 4);
    put_le(out, frame_index, 8);
    put_le(out, timestamp_us, 8);
    for (const auto packet : packets) {
      put_le(out, packet.size(), 4);
      out.insert(out.end(), packet.begin(), packet.end());
    }
    return out;
  }

  std::optional<frame_view_t> parse_frame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < frame_header_size || bytes.size() > max_frame_size ||
        bytes[0] != 'P' || bytes[1] != 'W' || bytes[2] != 'V' || bytes[3] != 'F' ||
        get_le(bytes, 4, 2) != version || get_le(bytes, 6, 2) != frame_header_size ||
        get_le(bytes, 8, 4) != bytes.size()) {
      return std::nullopt;
    }
    const auto count = get_le(bytes, 12, 4);
    if (count == 0 || count > max_native_packets || count > (bytes.size() - frame_header_size) / 8) {
      return std::nullopt;
    }
    frame_view_t result {get_le(bytes, 16, 8), get_le(bytes, 24, 8), {}};
    result.packets.reserve(static_cast<std::size_t>(count));
    std::size_t offset = frame_header_size;
    for (std::size_t i = 0; i < count; ++i) {
      if (bytes.size() - offset < 4) {
        return std::nullopt;
      }
      const auto size = get_le(bytes, offset, 4);
      offset += 4;
      if (size == 0 || size % 4 != 0 || size > bytes.size() - offset) {
        return std::nullopt;
      }
      result.packets.emplace_back(bytes.subspan(offset, static_cast<std::size_t>(size)));
      offset += static_cast<std::size_t>(size);
    }
    if (offset != bytes.size()) {
      return std::nullopt;
    }
    return result;
  }

  std::optional<transport_limits_t> transport_limits(const transport_config_t &config) {
    if (config.path_mtu < 1280 || config.path_mtu > 1500 ||
        (config.ip_header_size != 20 && config.ip_header_size != 40) ||
        config.packet_size < 200 || config.packet_size > 1500 ||
        config.fec_percentage > 255 || config.min_fec_packets > 254 || (config.fragmented && config.critical_fec_percentage > 255)) {
      return std::nullopt;
    }
    // packetSize includes the 16-byte NV header, but excludes RTP and encryption.
    const auto datagram_bytes = config.packet_size + 16 + (config.encrypted ? 32 : 0);
    const auto wire_bytes = datagram_bytes + config.ip_header_size + 8;
    if (wire_bytes > config.path_mtu) {
      return std::nullopt;
    }
    std::uint32_t max_data = 0;
    auto conservative = config;
    conservative.fec_percentage = effective_critical_percentage(config);
    for (std::uint32_t data = 1; data <= 255; ++data) {
      if (data + parity_shards(data, conservative) <= 255) {
        max_data = data;
      }
    }
    if (max_data == 0 || (conservative.fec_percentage != 0 && 100 * config.min_fec_packets / max_data > 255)) {
      return std::nullopt;
    }
    const auto payload_bytes = config.packet_size - 16;
    return transport_limits_t {payload_bytes, datagram_bytes, wire_bytes, max_data,
                               4 * max_data * payload_bytes - (config.fragmented ? 0 : short_frame_header_size)};
  }

  std::optional<transport_plan_t> plan_transport(std::size_t frame_bytes, const transport_config_t &config) {
    const auto limits = transport_limits(config);
    if (!limits || frame_bytes < (config.fragmented ? fragment_header_size + fragment_record_header_size + 4 : frame_header_size) ||
        frame_bytes > limits->max_frame_bytes || (config.fragmented && frame_bytes % limits->payload_bytes != 0)) {
      return std::nullopt;
    }
    const auto data = static_cast<std::uint32_t>((frame_bytes + (config.fragmented ? 0 : short_frame_header_size) + limits->payload_bytes - 1) / limits->payload_bytes);
    transport_plan_t result;
    const auto block_count = (data + limits->max_data_shards - 1) / limits->max_data_shards;
    for (std::uint32_t i = 0; i < block_count; ++i) {
      const auto block_data = data / block_count + (i < data % block_count ? 1 : 0);
      if (!add_planned_block(result, block_data, effective_critical_percentage(config), config)) return std::nullopt;
    }
    return result;
  }

  std::uint32_t frame_budget(std::uint32_t wire_byte_budget, const transport_config_t &config) {
    const auto limits = transport_limits(config);
    if (!limits) {
      return 0;
    }
    std::uint32_t budget = 0;
    for (std::uint32_t shards = 1; shards <= 4 * limits->max_data_shards; ++shards) {
      const auto bytes = shards * limits->payload_bytes - (config.fragmented ? 0 : short_frame_header_size);
      const auto plan = plan_transport(bytes, config);
      if (plan && plan->wire_bytes <= wire_byte_budget) {
        budget = bytes;
      }
    }
    return budget;
  }

  std::optional<std::uint32_t> sideband_size_bound(std::uint32_t width, std::uint32_t height, bool chroma_444, std::uint32_t bands) {
    const auto count = active_block_count(width, height, chroma_444, bands);
    if (!count || *count > max_sideband_words * 32) return std::nullopt;
    return 20 + ((*count + 31) / 32) * 4;
  }

  std::uint32_t fragmented_payload_budget(std::uint32_t frame_byte_budget, const transport_config_t &config, std::uint32_t sideband_byte_limit) {
    const auto limits = transport_limits(config);
    if (!config.fragmented || !limits || sideband_byte_limit < 20 || sideband_byte_limit > 20 + 4 * max_sideband_words || sideband_byte_limit % 4 != 0) return 0;
    const auto shards = std::min(frame_byte_budget, limits->max_frame_bytes) / limits->payload_bytes;
    if (shards == 0) return 0;
    // A slot can waste up to one record header plus three alignment bytes.
    // Every item needs a record, and every shard transition can split one item.
    const auto slot_capacity = (limits->payload_bytes - fragment_header_size) & ~std::uint32_t {3};
    const auto fits = [&](std::uint32_t bytes) {
      constexpr auto minimum_nonfinal_packet_size = native_packet_boundary - 4095 * 4;
      const auto native_count = std::min<std::uint64_t>(max_native_packets, std::uint64_t {bytes} / minimum_nonfinal_packet_size + 1);
      const auto item_records = native_count + 1;  // Manifest plus native packets.
      const auto required = std::uint64_t {bytes} + sideband_byte_limit + item_records * fragment_record_header_size;
      const auto capacity = std::uint64_t {shards} * (slot_capacity - fragment_record_header_size);
      return required <= capacity;
    };
    std::uint32_t low = 0, high = std::min<std::uint32_t>(frame_byte_budget, max_frame_size);
    while (low < high) {
      const auto middle = low + (high - low + 1) / 2;
      if (fits(middle)) low = middle;
      else high = middle - 1;
    }
    return low & ~3U;
  }

  std::optional<fragment_view_t> parse_fragment(std::span<const std::uint8_t> payload) {
    if (payload.size() < fragment_header_size + fragment_record_header_size + 4 || payload.size() > 1484 ||
        payload[0] != 'P' || payload[1] != 'W' || payload[2] != 'P' || payload[3] != 'F' ||
        get_le(payload, 4, 2) != 2 || get_le(payload, 6, 2) != fragment_header_size || payload[35] != 0) return std::nullopt;
    fragment_view_t result;
    result.frame_index = get_le(payload, 8, 8);
    result.timestamp_us = get_le(payload, 16, 8);
    result.shard_index = static_cast<std::uint16_t>(get_le(payload, 24, 2));
    result.shard_count = static_cast<std::uint16_t>(get_le(payload, 26, 2));
    result.native_packet_count = static_cast<std::uint16_t>(get_le(payload, 28, 2));
    const auto record_count = get_le(payload, 30, 2);
    result.critical_packet_count = static_cast<std::uint16_t>(get_le(payload, 32, 2));
    result.active_block_bands = payload[34];
    result.active_block_count = static_cast<std::uint32_t>(get_le(payload, 36, 4));
    std::copy_n(payload.begin() + 40, 8, result.sequence_header.begin());
    if (result.shard_count == 0 || result.shard_count > 4 * 255 || result.shard_index >= result.shard_count ||
        result.native_packet_count == 0 || result.native_packet_count > max_native_packets ||
        result.critical_packet_count == 0 || result.critical_packet_count > result.native_packet_count ||
        result.active_block_bands > 4 || result.active_block_count > max_sideband_words * 32 ||
        record_count == 0 || record_count > (payload.size() - fragment_header_size) / (fragment_record_header_size + 4) ||
        !valid_sequence_header(result.sequence_header)) return std::nullopt;
    const auto first = get_le(result.sequence_header, 0, 4);
    const auto second = get_le(result.sequence_header, 4, 4);
    const auto expected_count = active_block_count(static_cast<std::uint32_t>((first & 0x3fff) + 1),
                                                  static_cast<std::uint32_t>(((first >> 14) & 0x3fff) + 1),
                                                  (second & 0x04000000U) != 0, result.active_block_bands);
    if (!expected_count || *expected_count != result.active_block_count) return std::nullopt;
    const auto manifest_size = 20 + ((result.active_block_count + 31) / 32) * 4;
    std::size_t offset = fragment_header_size;
    result.records.reserve(static_cast<std::size_t>(record_count));
    for (std::size_t i = 0; i < record_count; ++i) {
      if (payload.size() - offset < fragment_record_header_size) return std::nullopt;
      const auto kind = payload[offset];
      const auto flags = payload[offset + 1];
      const auto length = get_le(payload, offset + 2, 2);
      fragment_record_view_t record {
        kind == 1, (flags & 1) != 0,
        static_cast<std::uint32_t>(get_le(payload, offset + 4, 4)),
        static_cast<std::uint32_t>(get_le(payload, offset + 8, 4)),
        static_cast<std::uint32_t>(get_le(payload, offset + 12, 4)), {}};
      offset += fragment_record_header_size;
      if (kind > 1 || flags > 1 || length == 0 || length % 4 != 0 || length > payload.size() - offset ||
          record.item_size == 0 || record.item_size > max_frame_size || record.item_size % 4 != 0 || record.item_offset % 4 != 0 ||
          record.item_offset > record.item_size || length > record.item_size - record.item_offset ||
          (record.manifest && (record.item_index != 0 || record.item_size != manifest_size || !record.critical)) ||
          (!record.manifest && (record.item_index >= result.native_packet_count || record.item_size < 8 ||
                               record.critical != (record.item_index < result.critical_packet_count)))) return std::nullopt;
      record.data = payload.subspan(offset, static_cast<std::size_t>(length));
      result.records.push_back(record);
      offset += static_cast<std::size_t>(length);
    }
    if (!std::all_of(payload.begin() + offset, payload.end(), [](std::uint8_t byte) { return byte == 0; })) return std::nullopt;
    return result;
  }

  std::optional<std::vector<std::uint8_t>> serialize_fragmented_frame(
    std::uint64_t frame_index, std::uint64_t timestamp_us,
    std::span<const std::span<const std::uint8_t>> packets, const partial_metadata_t &metadata,
    const transport_config_t &config, std::size_t byte_limit
  ) {
    const auto limits = transport_limits(config);
    if (!config.fragmented || !limits || packets.empty() || packets.size() > max_native_packets ||
        packets.front().size() < 8 || metadata.active_block_bands > 4 ||
        metadata.active_block_count > max_sideband_words * 32 ||
        metadata.active_block_words.size() != (metadata.active_block_count + 31) / 32) return std::nullopt;
    const auto sequence_header = packets.front().first(8);
    if (!valid_sequence_header(sequence_header)) return std::nullopt;
    const auto first = get_le(sequence_header, 0, 4), second = get_le(sequence_header, 4, 4);
    const auto expected_count = active_block_count(static_cast<std::uint32_t>((first & 0x3fff) + 1),
                                                  static_cast<std::uint32_t>(((first >> 14) & 0x3fff) + 1),
                                                  (second & 0x04000000U) != 0, metadata.active_block_bands);
    if (!expected_count || *expected_count != metadata.active_block_count || metadata.critical_packets[0] == 0 ||
        !std::is_sorted(metadata.critical_packets.begin(), metadata.critical_packets.end()) ||
        metadata.critical_packets.back() > packets.size()) return std::nullopt;
    if (metadata.active_block_count % 32 != 0 && !metadata.active_block_words.empty() &&
        (metadata.active_block_words.back() >> (metadata.active_block_count % 32)) != 0) return std::nullopt;
    std::size_t native_bytes = 0;
    for (std::size_t i = 0; i < packets.size(); ++i) {
      if (packets[i].size() > max_frame_size - native_bytes || packets[i].size() % 4 != 0 ||
          !valid_native_packet(packets[i], static_cast<std::uint32_t>(i), sequence_header)) return std::nullopt;
      native_bytes += packets[i].size();
    }
    byte_limit = std::min({byte_limit, max_frame_size, static_cast<std::size_t>(limits->max_frame_bytes)});
    const auto max_slots = byte_limit / limits->payload_bytes;
    if (max_slots == 0) return std::nullopt;
    std::vector<std::uint8_t> manifest;
    manifest.reserve(20 + metadata.active_block_words.size() * 4);
    for (const auto count : metadata.critical_packets) put_le(manifest, count, 4);
    for (const auto word : metadata.active_block_words) put_le(manifest, word, 4);
    std::vector<std::uint8_t> out;
    std::size_t slot_offset = 0, position = limits->payload_bytes;
    std::uint16_t record_count = 0;
    const auto write_at = [&](std::size_t offset, std::uint64_t value, unsigned count) {
      for (unsigned i = 0; i < count; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    const auto begin_slot = [&]() {
      if (out.size() / limits->payload_bytes >= max_slots) return false;
      slot_offset = out.size();
      out.resize(out.size() + limits->payload_bytes, 0);
      std::copy_n("PWPF", 4, out.begin() + slot_offset);
      write_at(slot_offset + 4, 2, 2);
      write_at(slot_offset + 6, fragment_header_size, 2);
      write_at(slot_offset + 8, frame_index, 8);
      write_at(slot_offset + 16, timestamp_us, 8);
      write_at(slot_offset + 24, slot_offset / limits->payload_bytes, 2);
      write_at(slot_offset + 28, packets.size(), 2);
      write_at(slot_offset + 32, metadata.critical_packets[metadata.active_block_bands], 2);
      write_at(slot_offset + 34, metadata.active_block_bands, 1);
      write_at(slot_offset + 36, metadata.active_block_count, 4);
      std::copy(sequence_header.begin(), sequence_header.end(), out.begin() + slot_offset + 40);
      position = fragment_header_size;
      record_count = 0;
      return true;
    };
    const auto append_item = [&](std::span<const std::uint8_t> item, std::uint32_t index, bool is_manifest) {
      std::size_t offset = 0;
      while (offset < item.size()) {
        if (limits->payload_bytes - position < fragment_record_header_size + 4 && !begin_slot()) return false;
        const auto available = (limits->payload_bytes - position - fragment_record_header_size) & ~std::size_t {3};
        const auto length = std::min(available, item.size() - offset);
        const auto record = slot_offset + position;
        write_at(record, is_manifest ? 1 : 0, 1);
        write_at(record + 1, is_manifest || index < metadata.critical_packets[metadata.active_block_bands] ? 1 : 0, 1);
        write_at(record + 2, length, 2);
        write_at(record + 4, index, 4);
        write_at(record + 8, item.size(), 4);
        write_at(record + 12, offset, 4);
        std::copy_n(item.begin() + offset, length, out.begin() + record + fragment_record_header_size);
        position += fragment_record_header_size + length;
        offset += length;
        write_at(slot_offset + 30, ++record_count, 2);
      }
      return true;
    };
    if (!append_item(manifest, 0, true)) return std::nullopt;
    for (std::size_t i = 0; i < packets.size(); ++i) {
      if (!append_item(packets[i], static_cast<std::uint32_t>(i), false)) return std::nullopt;
    }
    const auto shard_count = out.size() / limits->payload_bytes;
    for (std::size_t slot = 0; slot < out.size(); slot += limits->payload_bytes) write_at(slot + 26, shard_count, 2);
    if (!plan_fragmented_transport(out, config)) return std::nullopt;
    return out;
  }

  std::optional<transport_plan_t> plan_fragmented_transport(std::span<const std::uint8_t> frame, const transport_config_t &config) {
    const auto limits = transport_limits(config);
    if (!config.fragmented || !limits || frame.empty() || frame.size() > limits->max_frame_bytes || frame.size() % limits->payload_bytes != 0) return std::nullopt;
    const auto count = static_cast<std::uint32_t>(frame.size() / limits->payload_bytes);
    std::optional<fragment_view_t> first;
    std::uint32_t critical_shards = 0;
    bool passed_critical = false;
    std::uint32_t expected_item = 0, expected_offset = 0, expected_size = 0;
    bool in_manifest = true;
    for (std::uint32_t index = 0; index < count; ++index) {
      auto fragment = parse_fragment(frame.subspan(std::size_t(index) * limits->payload_bytes, limits->payload_bytes));
      if (!fragment || fragment->shard_index != index || fragment->shard_count != count || (first && !same_frame(*first, *fragment))) return std::nullopt;
      if (!first) first = *fragment;
      for (const auto &record : fragment->records) {
        if (record.manifest != in_manifest || record.item_index != expected_item || record.item_offset != expected_offset ||
            (expected_offset != 0 && record.item_size != expected_size)) return std::nullopt;
        expected_size = record.item_size;
        expected_offset += static_cast<std::uint32_t>(record.data.size());
        if (expected_offset == expected_size) {
          expected_offset = 0;
          expected_size = 0;
          if (in_manifest) in_manifest = false;
          else ++expected_item;
        }
      }
      const bool critical = std::any_of(fragment->records.begin(), fragment->records.end(), [](const auto &record) { return record.critical; });
      if (critical && passed_critical) return std::nullopt;  // Encoder orders manifest and critical packets first.
      if (critical) ++critical_shards;
      else passed_critical = true;
    }
    if (critical_shards == 0 || in_manifest || expected_offset != 0 || expected_item != first->native_packet_count) return std::nullopt;
    const auto conservative = plan_transport(frame.size(), config);
    if (!conservative) return std::nullopt;
    // Keep critical data in early, separately protected blocks when capacity
    // permits. Fill the final critical block further only to respect four blocks.
    transport_plan_t result;
    auto base_config = config;
    base_config.fragmented = false;
    const auto base_limits = transport_limits(base_config);
    if (!base_limits) return std::nullopt;
    std::uint32_t consumed = 0;
    while (consumed < critical_shards) {
      const auto remaining = count - consumed;
      const auto remaining_critical = critical_shards - consumed;
      const auto remaining_slots = 3 - result.block_count;
      const auto minimum_here = remaining > remaining_slots * base_limits->max_data_shards ? remaining - remaining_slots * base_limits->max_data_shards : 1U;
      const auto data = std::min(limits->max_data_shards, std::max(std::min(remaining_critical, limits->max_data_shards), minimum_here));
      if (!add_planned_block(result, data, effective_critical_percentage(config), config)) return conservative;
      consumed += data;
    }
    while (consumed < count) {
      const auto data = std::min(count - consumed, base_limits->max_data_shards);
      if (!add_planned_block(result, data, config.fec_percentage, config)) return conservative;
      consumed += data;
    }
    // Tiny blocks can make min-parity percentages unrepresentable. A balanced
    // all-critical plan is the safe fallback, never an unprotected frame.
    if (result.wire_bytes > conservative->wire_bytes) return conservative;
    return result;
  }

  std::optional<partial_frame_t> reassemble_partial_frame(std::span<const std::span<const std::uint8_t>> payloads, const transport_config_t &config) {
    const auto limits = transport_limits(config);
    if (!config.fragmented || !limits || payloads.empty() || payloads.size() > 8 * 255) return std::nullopt;
    std::vector<fragment_view_t> fragments;
    fragments.reserve(payloads.size());
    std::map<std::uint16_t, std::span<const std::uint8_t>> unique_shards;
    struct item_t {
      std::uint32_t size = 0;
      std::vector<std::uint8_t> bytes;
      std::vector<bool> received_words;
      std::size_t received_count = 0;
    };
    std::map<std::uint32_t, item_t> items;  // UINT32_MAX is the manifest.
    std::size_t total_bytes = 0;
    for (const auto payload : payloads) {
      if (payload.size() != limits->payload_bytes) return std::nullopt;
      auto fragment = parse_fragment(payload);
      if (!fragment || fragment->shard_count > 4 * limits->max_data_shards || (!fragments.empty() && !same_frame(fragments.front(), *fragment))) return std::nullopt;
      const auto [shard, added] = unique_shards.emplace(fragment->shard_index, payload);
      if (!added) {
        if (!std::equal(payload.begin(), payload.end(), shard->second.begin())) return std::nullopt;
        continue;
      }
      for (const auto &record : fragment->records) {
        const auto key = record.manifest ? 0xffffffffU : record.item_index;
        auto [item, inserted] = items.try_emplace(key);
        if (inserted) {
          if (record.item_size > max_frame_size - total_bytes) return std::nullopt;
          total_bytes += record.item_size;
          item->second.size = record.item_size;
        } else if (item->second.size != record.item_size) return std::nullopt;
      }
      fragments.push_back(std::move(*fragment));
    }
    if (fragments.empty()) return std::nullopt;
    // All advertised item sizes and aggregate memory are now bounded.
    for (auto &[index, item] : items) {
      item.bytes.resize(item.size);
      item.received_words.resize(item.size / 4, false);
    }
    for (const auto &fragment : fragments) {
      for (const auto &record : fragment.records) {
        auto &item = items.at(record.manifest ? 0xffffffffU : record.item_index);
        for (std::size_t offset = 0; offset < record.data.size(); offset += 4) {
          const auto destination = record.item_offset + offset;
          const auto word = destination / 4;
          if (item.received_words[word]) {
            if (!std::equal(record.data.begin() + offset, record.data.begin() + offset + 4, item.bytes.begin() + destination)) return std::nullopt;
          } else {
            std::copy_n(record.data.begin() + offset, 4, item.bytes.begin() + destination);
            item.received_words[word] = true;
            ++item.received_count;
          }
        }
      }
    }
    const auto &first = fragments.front();
    partial_frame_t result;
    result.frame_index = first.frame_index;
    result.timestamp_us = first.timestamp_us;
    result.sequence_header = first.sequence_header;
    result.active_block_bands = first.active_block_bands;
    result.active_block_count = first.active_block_count;
    result.critical_complete = true;
    const auto manifest = items.find(0xffffffffU);
    if (manifest != items.end() && manifest->second.received_count == manifest->second.size / 4) {
      const auto bytes = std::span<const std::uint8_t>(manifest->second.bytes);
      for (std::size_t i = 0; i < result.critical_packets.size(); ++i) result.critical_packets[i] = static_cast<std::uint32_t>(get_le(bytes, i * 4, 4));
      if (result.critical_packets[0] == 0 || !std::is_sorted(result.critical_packets.begin(), result.critical_packets.end()) ||
          result.critical_packets.back() > first.native_packet_count || result.critical_packets[first.active_block_bands] != first.critical_packet_count) return std::nullopt;
      for (std::size_t offset = 20; offset < bytes.size(); offset += 4) result.active_block_words.push_back(static_cast<std::uint32_t>(get_le(bytes, offset, 4)));
      if (first.active_block_count % 32 != 0 && !result.active_block_words.empty() &&
          (result.active_block_words.back() >> (first.active_block_count % 32)) != 0) return std::nullopt;
      result.manifest_complete = true;
    }
    for (std::uint32_t index = 0; index < first.native_packet_count; ++index) {
      auto item = items.find(index);
      if (item == items.end() || item->second.received_count != item->second.size / 4) {
        ++result.missing_packets;
        if (index < first.critical_packet_count) result.critical_complete = false;
        continue;
      }
      if (!valid_native_packet(item->second.bytes, index, result.sequence_header)) return std::nullopt;
      result.packet_indices.push_back(index);
      result.packets.push_back(std::move(item->second.bytes));
    }
    result.critical_complete = result.critical_complete && result.manifest_complete;
    result.complete = result.missing_packets == 0 && result.manifest_complete;
    return result;
  }
}  // namespace pyrowave::protocol
