#pragma once

#include "src/pyrowave_protocol.h"
#include "reference_codec.h"
#include <bcrypt.h>
#include <fstream>
#include <sstream>

namespace pyrowave_smoke {
  inline std::string sha256(std::span<const std::uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto check_status = [](NTSTATUS status) {
      if (status < 0) throw std::runtime_error("SHA-256 operation failed");
    };
    check_status(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    try {
      check_status(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
      check_status(BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0));
      std::array<std::uint8_t, 32> digest {};
      check_status(BCryptFinishHash(hash, digest.data(), digest.size(), 0));
      BCryptDestroyHash(hash);
      BCryptCloseAlgorithmProvider(algorithm, 0);
      std::ostringstream text;
      for (auto value : digest) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(value);
      return text.str();
    } catch (...) {
      if (hash) BCryptDestroyHash(hash);
      BCryptCloseAlgorithmProvider(algorithm, 0);
      throw;
    }
  }

  inline void save_fixture(const std::filesystem::path &directory,
                           const platf::pyrowave::packet_list_t &packets,
                           const std::vector<std::uint8_t> &source, const std::array<std::vector<std::uint8_t>, 3> &decoded,
                           int width, int height, const std::string &pattern, std::size_t budget, reference_decoder_t &reference) {
    // Refuse accidental overwrite of a previous fixture. This function is used
    // only by the synthetic harness; real desktop capture has no dump option.
    if (std::filesystem::exists(directory) && !std::filesystem::is_empty(directory)) throw std::runtime_error("Fixture output directory must be empty");
    std::filesystem::create_directories(directory);
    std::vector<std::span<const std::uint8_t>> views;
    for (const auto &packet : packets) views.emplace_back(packet);
    const auto frame = ::pyrowave::protocol::serialize_frame(0, 0, views, ::pyrowave::protocol::max_frame_size);
    if (!frame) throw std::runtime_error("Fixture does not fit the PWVF envelope bound");
    auto write = [&](const std::string &name, std::span<const std::uint8_t> bytes) {
      std::ofstream output(directory / name, std::ios::binary);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    };
    write("frame.pwvf", *frame);
    // Decode the exact serialized bytes again through the production parser and
    // the pinned reference decoder before they are accepted as a fixture.
    const auto parsed = ::pyrowave::protocol::parse_frame(*frame);
    if (!parsed || parsed->packets.size() != packets.size()) throw std::runtime_error("Fixture PWVF roundtrip failed");
    for (std::size_t i = 0; i < packets.size(); ++i) {
      if (!std::equal(parsed->packets[i].begin(), parsed->packets[i].end(), packets[i].begin(), packets[i].end())) throw std::runtime_error("Fixture native packet mismatch");
    }
    platf::pyrowave::packet_list_t parsed_packets;
    for (const auto packet : parsed->packets) parsed_packets.emplace_back(packet.begin(), packet.end());
    auto reconstructed = decoded;
    pyrowave_cpu_buffer reconstructed_buffer {};
    reconstructed_buffer.width = width;
    reconstructed_buffer.height = height;
    reconstructed_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    for (int plane = 0; plane < 3; ++plane) {
      std::fill(reconstructed[plane].begin(), reconstructed[plane].end(), 0xff);
      reconstructed_buffer.data[plane] = reconstructed[plane].data();
      reconstructed_buffer.row_stride_in_bytes[plane] = width / (plane ? 2 : 1);
      reconstructed_buffer.plane_size_in_bytes[plane] = reconstructed[plane].size();
    }
    reference.decode(parsed_packets, reconstructed_buffer);
    if (reconstructed != decoded) throw std::runtime_error("Serialized fixture reference decode differed from the native frame");
    const auto luma_size = static_cast<std::size_t>(width) * height;
    std::array<double, 3> mae {};
    for (int plane = 0; plane < 3; ++plane) {
      for (std::size_t i = 0; i < decoded[plane].size(); ++i) {
        const auto expected = plane ? source[luma_size + 2 * i + plane - 1] : source[i];
        mae[plane] += std::abs(int(decoded[plane][i]) - int(expected));
      }
      mae[plane] /= decoded[plane].size();
    }
    std::ofstream manifest(directory / "manifest.json");
    manifest.exceptions(std::ios::badbit | std::ios::failbit);
    manifest << "{\n  \"fixture_version\": 1,\n  \"api_version\": \"0.5.0\",\n  \"bitstream_revision\": \"" << ::pyrowave::protocol::bitstream_revision
             << "\",\n  \"profile\": \"" << ::pyrowave::protocol::profile << "\",\n  \"width\": " << width << ",\n  \"height\": " << height
             << ",\n  \"synthetic_pattern\": \"" << pattern << "\",\n  \"synthetic_frame_index\": 0,\n  \"source_layout\": \"NV12\",\n  \"target_bytes\": " << budget
             << ",\n  \"source_sha256\": \"" << sha256(source) << "\",\n  \"pwvf_sha256\": \"" << sha256(*frame) << "\",\n  \"pwvf_size\": " << frame->size()
             << ",\n  \"decoded_plane_sha256\": [\"" << sha256(decoded[0]) << "\", \"" << sha256(decoded[1]) << "\", \"" << sha256(decoded[2])
             << "\"],\n  \"decoded_plane_mae\": [" << mae[0] << ", " << mae[1] << ", " << mae[2] << "],\n  \"native_packets\": [\n";
    for (std::size_t i = 0; i < packets.size(); ++i) {
      const auto name = "packet-" + std::to_string(i) + ".bin";
      write(name, packets[i]);
      manifest << "    {\"file\": \"" << name << "\", \"bytes\": " << packets[i].size() << ", \"sha256\": \"" << sha256(packets[i]) << "\"}" << (i + 1 == packets.size() ? "\n" : ",\n");
    }
    manifest << "  ]\n}\n";
  }
}
