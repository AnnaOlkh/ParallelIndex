#include <algorithm>
#include <barrier>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {
std::size_t detect_line() {
#ifdef _WIN32
    DWORD size = 0;
    GetLogicalProcessorInformation(nullptr, &size);
    if (size) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
            (size + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)-1) / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(info.data(), &size))
            for (std::size_t i = 0; i < size/sizeof(info[0]); ++i)
                if (info[i].Relationship == RelationCache && info[i].Cache.Level == 1 &&
                    (info[i].Cache.Type == CacheData || info[i].Cache.Type == CacheUnified)) return info[i].Cache.LineSize;
    }
#elif defined(__linux__) && defined(_SC_LEVEL1_DCACHE_LINESIZE)
    const long value = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (value > 0) return static_cast<std::size_t>(value);
#endif
    return 0;
}
std::size_t number(const std::string& text) {
    std::size_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
    if (error != std::errc{} || end != text.data()+text.size() || value == 0) throw std::runtime_error("Expected positive integer");
    return value;
}
void print_system_info() {
    std::cout << "logical_threads_hint=" << std::thread::hardware_concurrency()
              << "\ncache_line=" << detect_line() << '\n';
#ifdef _WIN32
    DWORD size = 0;
    GetLogicalProcessorInformation(nullptr, &size);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
        (size + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)-1) / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (size && GetLogicalProcessorInformation(info.data(), &size)) {
        std::size_t physical = 0;
        for (std::size_t i = 0; i < size/sizeof(info[0]); ++i) {
            if (info[i].Relationship == RelationProcessorCore) ++physical;
            if (info[i].Relationship == RelationCache) {
                const auto& c = info[i].Cache;
                std::cout << "cache level=" << static_cast<unsigned>(c.Level)
                          << " type=" << (c.Type == CacheData ? "data" : (c.Type == CacheInstruction ? "instruction" : "unified"))
                          << " bytes=" << c.Size << " line=" << c.LineSize << '\n';
            }
        }
        std::cout << "physical_cores=" << physical << '\n';
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        std::cout << "ram_bytes=" << memory.ullTotalPhys << "\navailable_ram_bytes=" << memory.ullAvailPhys << '\n';
#endif
}
struct AlignedDelete {
    std::size_t alignment;
    void operator()(void* p) const { ::operator delete(p, std::align_val_t(alignment)); }
};
double run(std::size_t threads, std::size_t stride, std::size_t line, std::size_t iterations) {
    std::unique_ptr<void, AlignedDelete> storage(
        ::operator new(threads*stride, std::align_val_t(line)), AlignedDelete{line});
    auto* data = static_cast<std::byte*>(storage.get());
    for (std::size_t i = 0; i < threads; ++i)
        std::construct_at(reinterpret_cast<std::uint64_t*>(data+i*stride), std::uint64_t{0});
    std::barrier gate(static_cast<std::ptrdiff_t>(threads+1));
    std::vector<std::jthread> workers;
    workers.reserve(threads);
    try {
        for (std::size_t i = 0; i < threads; ++i) workers.emplace_back([&, i] {
            gate.arrive_and_wait();
            auto* own = reinterpret_cast<volatile std::uint64_t*>(data+i*stride);
            for (std::size_t j = 0; j < iterations; ++j) {
                const std::uint64_t value = *own;
                *own = value + 1;
            }
        });
    } catch (...) {
        for (std::size_t i = workers.size(); i < threads; ++i) gate.arrive_and_drop();
        gate.arrive_and_wait();
        throw;
    }
    const auto start = std::chrono::steady_clock::now();
    gate.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    for (std::size_t i = 0; i < threads; ++i)
        if (*reinterpret_cast<std::uint64_t*>(data+i*stride) != iterations) throw std::runtime_error("Counter verification failed");
    return ms;
}
}
int main(int argc, char** argv) {
    try {
        std::size_t repeats = 5, iterations = 10000000, line = detect_line();
        const bool detected = line != 0;
        bool override_line = false;
        std::string csv_path;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--info") { print_system_info(); return 0; }
            if (option == "--help") {
                std::cout << "false_sharing [--repeats N] [--iterations N] [--cache-line BYTES] [--csv FILE]\n"
                             "Runs P=1,2,4,8, both layouts, one warmup per point. Formal iterations=10000000.\n"
                             "Use --info for cache topology and available memory (Windows).\n";
                return 0;
            }
            if (++i == argc) throw std::runtime_error("Missing argument");
            if (option == "--repeats") repeats = number(argv[i]);
            else if (option == "--iterations") iterations = number(argv[i]);
            else if (option == "--cache-line") { line = number(argv[i]); override_line = true; }
            else if (option == "--csv") csv_path = argv[i];
            else throw std::runtime_error("Unknown option: " + option);
        }
        if (line < alignof(std::max_align_t) || line > 4096 || !std::has_single_bit(line))
            throw std::runtime_error("Cache line unknown/invalid: specify measured --cache-line (power of two)");
        std::cout << "cache_line=" << line << " source=" << (override_line ? "user" : (detected ? "OS" : "unknown"))
                  << " counter_bytes=" << sizeof(std::uint64_t) << " padded_stride=" << line << '\n';
        std::ofstream csv;
        if (!csv_path.empty()) {
            const std::filesystem::path path(csv_path);
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            const bool header = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
            csv.open(path, std::ios::app);
            if (!csv) throw std::runtime_error("Cannot open CSV");
            if (header) csv << "experiment,version,threads,K,merge,v1_mode,repeat,build_ms,merge_ms,canonicalize_ms,compute_ms,read_ms,write_ms,total_occurrences,correct,files,bytes\n";
        }
        for (std::size_t threads : {1,2,4,8}) for (bool padded : {false,true}) {
            const std::size_t stride = padded ? line : sizeof(std::uint64_t);
            (void)run(threads, stride, line, iterations);
            std::vector<double> times;
            for (std::size_t repeat = 1; repeat <= repeats; ++repeat) {
                const double ms = run(threads, stride, line, iterations);
                times.push_back(ms);
                if (csv.is_open()) {
                    csv << std::fixed << std::setprecision(6) << "false_sharing," << (padded ? "padded" : "adjacent") << ','
                        << threads << ",0,na,na," << repeat << ',' << ms << ",0,0," << ms << ",0,0,"
                        << threads*iterations << ",1,0,0\n";
                    csv.flush();
                    if (!csv) throw std::runtime_error("CSV write failed");
                }
            }
            std::sort(times.begin(), times.end());
            const auto m = times.size()/2;
            const auto median = times.size()%2 ? times[m] : (times[m-1]+times[m])/2;
            std::cout << (padded ? "padded" : "adjacent") << " P=" << threads << " median_ms=" << median
                      << " min_ms=" << times.front() << " max_ms=" << times.back() << " verified=yes\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
