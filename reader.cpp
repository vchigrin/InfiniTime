#include <iostream>
#include <optional>
#include <algorithm>
#include <vector>
#include <string>
#include <fstream>

#define HEADER_TYPE_STD     1U
#define HEADER_TYPE_HEXDUMP 2U
#define HEADER_TYPE_INVALID 3U

typedef struct
{
    uint32_t type       : 2;
    uint32_t in_progress: 1;
    uint32_t data       : 29;
} nrf_log_generic_header_t;

typedef struct
{
    uint32_t type       : 2;
    uint32_t in_progress: 1;
    uint32_t severity   : 3;
    uint32_t nargs      : 4;
    uint32_t addr       : 22;
} nrf_log_std_header_t;

typedef struct
{
    uint32_t type       : 2;
    uint32_t in_progress: 1;
    uint32_t severity   : 3;
    uint32_t offset     : 10;
    uint32_t reserved   : 6;
    uint32_t len        : 10;
} nrf_log_hexdump_header_t;

typedef union
{
    nrf_log_generic_header_t generic;
    nrf_log_std_header_t     std;
    nrf_log_hexdump_header_t hexdump;
    uint32_t                 raw;
} nrf_log_main_header_t;

typedef struct
{
    nrf_log_main_header_t base;
    uint16_t module_id;
    uint16_t dropped;
    uint32_t timestamp;
} nrf_log_header_t;

class BootImage {
 public:
  std::optional<std::string> GetStringAtAddress(uint32_t addr) const;
  static std::optional<BootImage> Load(const char* file_path);

 private:
  // See bootloader/README.md
  static constexpr uint32_t kImageBaseAddress = 0x8000;
  explicit BootImage(std::vector<char> content)
      : image_data_(std::move(content)) {}

  const std::vector<char> image_data_;
};

std::optional<BootImage> BootImage::Load(const char* file_path) {
  std::fstream strm(file_path, std::ios::in);
  if (!strm.is_open()) {
    return std::nullopt;
  }
  strm.seekg(0, std::ios_base::end);
  const size_t file_size = strm.tellg();
  if (!file_size) {
    return std::nullopt;
  }
  strm.seekg(0, std::ios_base::beg);
  std::vector<char> buf(file_size, 0);
  strm.read(buf.data(), file_size);
  return BootImage(std::move(buf));
}

std::optional<std::string> BootImage::GetStringAtAddress(uint32_t addr) const {
  if (addr < kImageBaseAddress) {
    return std::nullopt;
  }
  const uint32_t offset = addr - kImageBaseAddress;
  if (offset > image_data_.size()) {
    return std::nullopt;
  }
  const auto str_end = std::find(
      image_data_.begin() + offset,
      image_data_.end(),
      '\0');
  if (str_end == image_data_.end()) {
    return std::nullopt;
  }
  return std::string(
      image_data_.begin() + offset,
      str_end);
}


void ReadStdHeader(
    const BootImage& boot_img,
    const nrf_log_main_header_t& header,
    std::fstream& strm) {
  std::cout << "STD header" <<
    " in_progress " << header.std.in_progress <<
    " severity " << header.std.severity;
  const auto maybe_str = boot_img.GetStringAtAddress(header.std.addr);
  if (maybe_str) {
    std::cout << " msg: |" << *maybe_str << "|";
  } else {
    std::cout << " unknown msg addr: "
              << std::hex << header.std.addr << std::dec;
  }
  std::cout << std::endl;
  for (uint32_t i = 0; i < header.std.nargs; ++i) {
    uint32_t arg = 0;
    strm.read(reinterpret_cast<char*>(&arg), sizeof(arg));
    std::cout << "   Arg " << i << std::hex
              << " 0x" << arg << std::dec << std::endl;
  }
}

void ReadHexdumpHeader(
    const BootImage& boot_img,
    const nrf_log_main_header_t& header,
    std::fstream& strm) {
  std::cout << "HEX header" <<
    " in_progress " << header.hexdump.in_progress <<
    " severity " << header.hexdump.severity  <<
    " offset " << header.hexdump.offset  <<
    " len " << header.hexdump.len  <<
    std::endl;
  for (uint32_t i = 0; i < header.hexdump.len; ++i) {
    uint32_t arg = 0;
    strm.read(reinterpret_cast<char*>(&arg), sizeof(arg));
    std::cout << "   Byte " << i << std::hex
              << " 0x" << arg << std::dec << std::endl;
  }
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "Usage: log_reader log_file_path pinetime-mcuboot-app-image-*.bin file path" << std::endl;
    return 1;
  }
  const auto maybe_boot_image = BootImage::Load(argv[2]);
  if (!maybe_boot_image) {
    std::cerr << "Unable to load boot image from " << argv[2] << std::endl;
    return 1;
  }
  std::fstream strm(argv[1], std::ios::in);
  if (!strm.is_open()) {
    std::cerr << "Failed open file " << argv[1] << std::endl;
    return 1;
  }
  int counter = 0;
  while (strm.good()) {
    nrf_log_header_t header;
    strm.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (strm.gcount() != sizeof(header)) {
      break;
    }
    std::cout << "==> Record "
        << counter
        << " module " << header.module_id
        << " dropped " << header.dropped
        << " timestamp " << header.timestamp
        << std::endl;
    ++counter;
    if (header.base.generic.type == HEADER_TYPE_STD) {
      ReadStdHeader(*maybe_boot_image, header.base, strm);
    } else if (header.base.generic.type == HEADER_TYPE_HEXDUMP) {
      ReadHexdumpHeader(*maybe_boot_image, header.base, strm);
    }
  }
  return 0;
}
