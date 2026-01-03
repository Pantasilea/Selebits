#include "deflate.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main()
{
    const std::filesystem::path p {u8"C:/Users/Miryu/Documents/DEFLATE and ZLIB test suite/deflate-stream-1.bin"};
    // 77,954 bytes
    const std::uintmax_t file_size = std::filesystem::file_size(p);
    std::vector<std::uint8_t> deflate_stream(file_size);
    std::ifstream ifs {p, std::ios_base::binary};
    ifs.read(reinterpret_cast<char*>(deflate_stream.data()), file_size);
    ifs.close();

    std::vector<std::uint8_t> inflated_stream;
    try {
        inflated_stream = sel::decompress_deflate(deflate_stream);
    }
    catch(const std::exception& e) {
        std::cout << e.what() << '\n';
        return 1;
    }
    // must be 432425 (deflate-stream-0.bin)
    // must be 6176 (deflate-stream-1.bin)
    std::cout << "Byte quantity of inflated stream: " << inflated_stream.size() << '\n';

    return 0;
}