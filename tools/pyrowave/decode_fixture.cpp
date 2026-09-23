// Offline fixture decoding. Never captures a desktop or opens a connection.
#include "fixture.h"

int main(int argc, char **argv) {
  using namespace pyrowave_smoke;
  try {
    if (argc < 4 || argc > 5) throw std::runtime_error("Usage: pyrowave-decode-fixture.exe frame.pwvf width height [adapter-index]");
    const auto width = std::stoi(argv[2]), height = std::stoi(argv[3]);
    const auto adapter_index = argc == 5 ? std::stoi(argv[4]) : 0;
    if (width < 64 || height < 64 || width > 4096 || height > 4096 || (width & 1) || (height & 1) || adapter_index < 0) throw std::runtime_error("Invalid fixture dimensions or adapter index");
    const auto length = std::filesystem::file_size(argv[1]);
    if (length < ::pyrowave::protocol::frame_header_size || length > ::pyrowave::protocol::max_frame_size) throw std::runtime_error("Invalid PWVF file size");
    std::vector<std::uint8_t> bytes(length);
    std::ifstream input(argv[1], std::ios::binary);
    input.exceptions(std::ios::badbit | std::ios::failbit);
    input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    const auto parsed = ::pyrowave::protocol::parse_frame(bytes);
    if (!parsed) throw std::runtime_error("Invalid PWVF frame");
    platf::pyrowave::packet_list_t packets;
    for (const auto packet : parsed->packets) packets.emplace_back(packet.begin(), packet.end());
    com_ptr<IDXGIFactory1> factory;
    com_ptr<IDXGIAdapter1> adapter;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "Create fixture DXGI factory");
    checked(factory->EnumAdapters1(adapter_index, adapter.GetAddressOf()), "Select fixture adapter");
    DXGI_ADAPTER_DESC1 desc {};
    checked(adapter->GetDesc1(&desc), "Read fixture adapter");
    reference_decoder_t reference;
    reference.initialize(desc.AdapterLuid, width, height);
    std::array<std::vector<std::uint8_t>, 3> planes;
    pyrowave_cpu_buffer output {};
    output.width = width;
    output.height = height;
    output.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    for (int p = 0; p < 3; ++p) {
      const int scale = p ? 2 : 1;
      planes[p].resize(static_cast<std::size_t>(width / scale) * (height / scale));
      output.data[p] = planes[p].data();
      output.row_stride_in_bytes[p] = width / scale;
      output.plane_size_in_bytes[p] = planes[p].size();
    }
    reference.decode(packets, output);
    std::cout << "{\"decoded_plane_sha256\":[\"" << sha256(planes[0]) << "\",\"" << sha256(planes[1]) << "\",\"" << sha256(planes[2]) << "\"]}" << std::endl;
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << "FAIL: " << failure.what() << std::endl;
    return 1;
  }
}
