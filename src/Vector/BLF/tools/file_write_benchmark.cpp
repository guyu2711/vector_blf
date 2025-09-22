#include <Vector/BLF/File.h>
#include <Vector/BLF/CanMessage.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using Vector::BLF::CanMessage;
using Vector::BLF::File;

namespace {
std::size_t fileSize(const std::string & path)
{
    std::ifstream ifs(path.c_str(), std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return 0;
    }
    const auto size = static_cast<std::size_t>(ifs.tellg());
    return size;
}

void removeFile(const std::string & path)
{
    std::remove(path.c_str());
}
}

int main()
{
    const std::string outputPath = "build/file_write_benchmark_output.blf";

    CanMessage prototype;
    prototype.channel = 1U;
    prototype.dlc = static_cast<uint8_t>(prototype.data.size());
    prototype.id = 0x123U;
    std::memset(prototype.data.data(), 0, prototype.data.size());
    const auto uncompressedFrameSize = prototype.calculateObjectSize();

    File file;
    file.open(outputPath, std::ios_base::out | std::ios_base::trunc);

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(1);

    std::size_t frameCount = 0U;
    while (std::chrono::steady_clock::now() < deadline) {
        auto * canMessage = new CanMessage(prototype);
        canMessage->objectTimeStamp = static_cast<uint64_t>(frameCount);
        file.write(canMessage);
        ++frameCount;
    }

    auto end = std::chrono::steady_clock::now();
    file.close();
    end = std::chrono::steady_clock::now();

    const double durationSeconds = std::chrono::duration<double>(end - start).count();
    const auto bytesWritten = fileSize(outputPath);

    if (bytesWritten == 0U) {
        std::cerr << "Failed to measure file size.\n";
        return 1;
    }

    const double framesPerSecond = static_cast<double>(frameCount) / durationSeconds;
    const double bytesPerSecond = static_cast<double>(bytesWritten) / durationSeconds;
    const double averageBytesPerFrame = static_cast<double>(bytesWritten) / static_cast<double>(frameCount);
    const auto totalUncompressedBytes = static_cast<double>(uncompressedFrameSize) * static_cast<double>(frameCount);
    const auto uncompressedBytesPerSecond = totalUncompressedBytes / durationSeconds;

    std::cout << "Total duration: " << durationSeconds << " s\n";
    std::cout << "Frames written: " << frameCount << "\n";
    std::cout << "Total bytes written: " << bytesWritten << "\n";
    std::cout << "Frames per second: " << framesPerSecond << "\n";
    std::cout << "Bytes per second: " << bytesPerSecond << "\n";
    std::cout << "Average bytes per frame: " << averageBytesPerFrame << "\n";
    std::cout << "Uncompressed bytes per frame: " << uncompressedFrameSize << "\n";
    std::cout << "Uncompressed bytes per second: " << uncompressedBytesPerSecond << "\n";

    removeFile(outputPath);
    return 0;
}
