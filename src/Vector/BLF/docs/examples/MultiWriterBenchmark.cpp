// SPDX-FileCopyrightText: 2024 Vector_BLF contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Vector/BLF/File.h>
#include <Vector/BLF/CanMessage.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace {

struct CommandLineOptions {
    std::size_t fileCount;
    std::size_t messagesPerFile;
    uint32_t queueSize;
    std::streamsize uncompressedBufferSize;
    uint32_t logContainerSize;
    uint32_t compressionThreads;
    std::string outputDirectory;
};

struct WriterResult {
    std::size_t messagesWritten;
    double secondsTaken;
};

const char * requireValue(int & index, int argc, char const * const argv[]) {
    if (index + 1 >= argc) {
        throw std::runtime_error("missing value for argument");
    }
    ++index;
    return argv[index];
}

std::size_t parseSizeT(const char * text) {
    char * end = nullptr;
    unsigned long long value = std::strtoull(text, &end, 10);
    if ((end == nullptr) || (*end != '\0')) {
        throw std::runtime_error("unable to parse numeric argument");
    }
    return static_cast<std::size_t>(value);
}

uint32_t parseUint32(const char * text) {
    char * end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if ((end == nullptr) || (*end != '\0')) {
        throw std::runtime_error("unable to parse numeric argument");
    }
    if (value > 0xFFFFFFFFul) {
        throw std::runtime_error("numeric argument exceeds uint32_t range");
    }
    return static_cast<uint32_t>(value);
}

std::streamsize parseStreamSize(const char * text) {
    char * end = nullptr;
    long long value = std::strtoll(text, &end, 10);
    if ((end == nullptr) || (*end != '\0')) {
        throw std::runtime_error("unable to parse numeric argument");
    }
    if (value <= 0) {
        throw std::runtime_error("buffer sizes must be positive");
    }
    return static_cast<std::streamsize>(value);
}

CommandLineOptions parseCommandLine(int argc, char const * const argv[]) {
    Vector::BLF::File defaults;
    CommandLineOptions options;
    options.fileCount = 10U;
    options.messagesPerFile = 200000U;
    options.queueSize = 10000U;
    options.logContainerSize = defaults.defaultLogContainerSize();
    options.uncompressedBufferSize = static_cast<std::streamsize>(options.logContainerSize) * 16;
    options.compressionThreads = defaults.compressionThreadCount();
    options.outputDirectory = "blf_multi_writer_logs";

    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if ((argument == "-h") || (argument == "--help")) {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --files <count>              Number of BLF files to write in parallel (default: 10)\n"
                      << "  --messages <count>           Number of CAN frames per file (default: 200000)\n"
                      << "  --queue-size <count>         Object queue capacity per file (default: 10000)\n"
                      << "  --uncompressed-bytes <size>  In-memory staging size in bytes (default: logContainerSize * 16)\n"
                      << "  --log-container-bytes <size> Log container size for new files (default: library default)\n"
                      << "  --compression-threads <n>    Background compression workers per file (default: hardware concurrency)\n"
                      << "  --output-dir <path>          Directory used for generated BLF files\n";
            std::exit(EXIT_SUCCESS);
        } else if (argument == "--files") {
            options.fileCount = parseSizeT(requireValue(i, argc, argv));
            if (options.fileCount == 0U) {
                throw std::runtime_error("--files requires a positive value");
            }
        } else if (argument == "--messages") {
            options.messagesPerFile = parseSizeT(requireValue(i, argc, argv));
            if (options.messagesPerFile == 0U) {
                throw std::runtime_error("--messages requires a positive value");
            }
        } else if (argument == "--queue-size") {
            options.queueSize = parseUint32(requireValue(i, argc, argv));
            if (options.queueSize == 0U) {
                throw std::runtime_error("--queue-size requires a positive value");
            }
        } else if (argument == "--uncompressed-bytes") {
            options.uncompressedBufferSize = parseStreamSize(requireValue(i, argc, argv));
        } else if (argument == "--log-container-bytes") {
            options.logContainerSize = parseUint32(requireValue(i, argc, argv));
        } else if (argument == "--compression-threads") {
            options.compressionThreads = parseUint32(requireValue(i, argc, argv));
            if (options.compressionThreads == 0U) {
                throw std::runtime_error("--compression-threads requires a positive value");
            }
        } else if (argument == "--output-dir") {
            options.outputDirectory = requireValue(i, argc, argv);
        } else {
            std::ostringstream oss;
            oss << "Unknown argument '" << argument << "'. Pass --help for usage.";
            throw std::runtime_error(oss.str());
        }
    }

    if (options.uncompressedBufferSize < static_cast<std::streamsize>(options.logContainerSize)) {
        options.uncompressedBufferSize = static_cast<std::streamsize>(options.logContainerSize);
    }

    return options;
}

bool ensureDirectoryExists(const std::string & path) {
#if defined(_WIN32)
    if (_mkdir(path.c_str()) == 0) {
        return true;
    }
#else
    if (mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
#endif
    if (errno == EEXIST) {
        return true;
    }
    return false;
}

struct WriterConfig {
    std::size_t index;
    std::size_t messagesPerFile;
    uint32_t queueSize;
    std::streamsize uncompressedBufferSize;
    uint32_t logContainerSize;
    uint32_t compressionThreads;
    std::string outputDirectory;
};

void runWriter(const WriterConfig & config, WriterResult & result, std::exception_ptr & error) {
    try {
        Vector::BLF::File file;
        file.setDefaultLogContainerSize(config.logContainerSize);
        file.setWriteBufferSizes(config.queueSize, config.uncompressedBufferSize);
        file.setCompressionThreadCount(config.compressionThreads);

        std::ostringstream fileName;
        fileName << config.outputDirectory << "/can_channel_" << (config.index + 1) << ".blf";
        file.open(fileName.str(), std::ios_base::out);
        if (!file.is_open()) {
            std::ostringstream oss;
            oss << "unable to open output file '" << fileName.str() << "'";
            throw std::runtime_error(oss.str());
        }

        const std::size_t payloadCount = 8U;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < config.messagesPerFile; ++i) {
            Vector::BLF::CanMessage * message = new Vector::BLF::CanMessage();
            message->channel = static_cast<uint16_t>((config.index % 0xFFFF) + 1U);
            message->dlc = static_cast<uint8_t>(payloadCount);
            message->id = static_cast<uint32_t>(0x100 + (i % 0x700));
            message->objectTimeStamp = static_cast<uint64_t>(i);
            message->flags = 0U;

            for (std::size_t byteIndex = 0; byteIndex < payloadCount; ++byteIndex) {
                message->data[byteIndex] = static_cast<uint8_t>((i + byteIndex) & 0xFFU);
            }

            file.write(message);
        }
        file.close();
        const auto end = std::chrono::steady_clock::now();
        result.messagesWritten = config.messagesPerFile;
        result.secondsTaken = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    } catch (...) {
        error = std::current_exception();
    }
}

void printSummary(const CommandLineOptions & options, const std::vector<WriterResult> & results, double totalSeconds) {
    const std::size_t totalMessages = options.messagesPerFile * options.fileCount;
    const double messageThroughput = totalMessages / totalSeconds;
    const double approximateFrameSize = 32.0; // bytes per frame including headers (rough estimate)
    const double megabytes = (totalMessages * approximateFrameSize) / (1024.0 * 1024.0);
    const double megabytesPerSecond = megabytes / totalSeconds;

    std::cout << "\nBenchmark summary" << std::endl;
    std::cout << "-----------------" << std::endl;
    std::cout << "Files written          : " << options.fileCount << std::endl;
    std::cout << "Frames per file        : " << options.messagesPerFile << std::endl;
    std::cout << "Object queue size      : " << options.queueSize << std::endl;
    std::cout << "Uncompressed buffer    : " << options.uncompressedBufferSize << " bytes" << std::endl;
    std::cout << "Log container size     : " << options.logContainerSize << " bytes" << std::endl;
    std::cout << "Compression threads    : " << options.compressionThreads << std::endl;
    std::cout << "Total frames           : " << totalMessages << std::endl;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Wall-clock time        : " << totalSeconds << " s" << std::endl;
    std::cout << "Frames per second      : " << messageThroughput << " fps" << std::endl;
    std::cout << "Approx. throughput     : " << megabytesPerSecond << " MiB/s" << std::endl;

    std::cout << "\nPer-writer timings" << std::endl;
    std::cout << "------------------" << std::endl;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const WriterResult & result = results[i];
        const double fps = result.messagesWritten / result.secondsTaken;
        std::cout << "Writer " << (i + 1) << ": " << result.secondsTaken << " s (" << fps << " fps)" << std::endl;
    }
}

} // namespace

int main(int argc, char const * const argv[]) {
    try {
        const CommandLineOptions options = parseCommandLine(argc, argv);

        if (!ensureDirectoryExists(options.outputDirectory)) {
            std::cerr << "Failed to create output directory '" << options.outputDirectory << "'" << std::endl;
            return EXIT_FAILURE;
        }

        std::vector<WriterResult> results(options.fileCount);
        std::vector<std::exception_ptr> errors(options.fileCount);
        std::vector<std::thread> threads;
        threads.reserve(options.fileCount);

        const auto overallStart = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < options.fileCount; ++index) {
            WriterConfig config;
            config.index = index;
            config.messagesPerFile = options.messagesPerFile;
            config.queueSize = options.queueSize;
            config.uncompressedBufferSize = options.uncompressedBufferSize;
            config.logContainerSize = options.logContainerSize;
            config.compressionThreads = options.compressionThreads;
            config.outputDirectory = options.outputDirectory;

            threads.push_back(std::thread(runWriter, config, std::ref(results[index]), std::ref(errors[index])));
        }

        for (std::size_t index = 0; index < threads.size(); ++index) {
            threads[index].join();
        }
        const auto overallEnd = std::chrono::steady_clock::now();

        for (std::size_t index = 0; index < errors.size(); ++index) {
            if (errors[index]) {
                std::rethrow_exception(errors[index]);
            }
        }

        const double totalSeconds = std::chrono::duration_cast<std::chrono::duration<double>>(overallEnd - overallStart).count();
        printSummary(options, results, totalSeconds);
    } catch (const std::exception & ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
