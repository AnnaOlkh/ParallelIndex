#include "core.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now()-start).count(); }
std::size_t number(const std::string& text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
    if (error != std::errc{} || end != text.data()+text.size() || value == 0 || value > 1000000)
        throw std::runtime_error("Expected positive integer <= 1000000: " + text);
    return value;
}
bool inside(const std::filesystem::path& file, const std::filesystem::path& directory) {
    const auto f = std::filesystem::weakly_canonical(file);
    const auto d = std::filesystem::weakly_canonical(directory);
    auto fi = f.begin();
    for (auto di = d.begin(); di != d.end(); ++di, ++fi) if (fi == f.end() || *fi != *di) return false;
    return true;
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--info") {
            std::cout << "cxx=" << __cplusplus << "\nunsafe_v1=" << LAB_UNSAFE_V1
                      << "\nunicode=" << lab::unicode_version()
                      << "\nlogical_threads_hint=" << std::thread::hardware_concurrency() << '\n';
#if defined(__clang__)
            std::cout << "compiler=Clang " << __clang_version__ << '\n';
#elif defined(__GNUC__)
            std::cout << "compiler=GCC " << __VERSION__ << '\n';
#elif defined(_MSC_VER)
            std::cout << "compiler=MSVC " << _MSC_VER << '\n';
#endif
#ifdef NDEBUG
            std::cout << "ndebug=1\n";
#else
            std::cout << "ndebug=0\n";
#endif
            return 0;
        }
        std::map<std::string, std::string> args;
        for (int i = 1; i < argc; ++i) {
            std::string name = argv[i];
            if (name == "--help") {
                std::cout << "indexer --input DIR --output FILE [--version v0|v1|v2|v3|v4]\n"
                    "  indexer --info : compiler, Unicode, build configuration\n"
                    "  [--threads N] [--shards K] [--merge sequential|tree]\n"
                    "  [--v1-mode defined|unsafe] [--verify V0_FILE]\n"
                    "  [--repeats N] [--warmup N] [--csv FILE] [--require-corpus yes]\n"
                    "  [--experiment index|scaling|v1_repro|v3_sweep] [--repeat-start N]\n"
                    "Output/CSV must be outside input. Input includes all regular files recursively.\n";
                return 0;
            }
            if (name != "--input" && name != "--output" && name != "--version" && name != "--threads" &&
                name != "--shards" && name != "--merge" && name != "--v1-mode" && name != "--verify" &&
                name != "--repeats" && name != "--warmup" && name != "--csv" && name != "--require-corpus" &&
                name != "--experiment" && name != "--repeat-start")
                throw std::runtime_error("Unknown option: " + name);
            if (i+1 == argc || !args.emplace(name, argv[++i]).second) throw std::runtime_error("Missing value or duplicate: " + name);
        }
        if (!args.contains("--input") || !args.contains("--output")) throw std::runtime_error("Use --help for usage");
        lab::Options options;
        if (args.contains("--version")) options.version = args.at("--version");
        if (args.contains("--threads")) options.threads = number(args.at("--threads"));
        if (args.contains("--shards")) options.shards = number(args.at("--shards"));
        if (args.contains("--merge")) options.merge = args.at("--merge");
        if (args.contains("--v1-mode")) options.v1_mode = args.at("--v1-mode");
        if (options.threads > 4096 || options.shards > 65536) throw std::runtime_error("Threads <= 4096 and shards <= 65536 required");
        if (options.version == "v0" && options.threads != 1) throw std::runtime_error("V0 requires --threads 1");
        const auto repeats = args.contains("--repeats") ? number(args.at("--repeats")) : 1;
        const auto warmup = args.contains("--warmup") ? number(args.at("--warmup")) : 1;
        const auto repeat_start = args.contains("--repeat-start") ? number(args.at("--repeat-start")) : 1;
        const auto experiment = args.contains("--experiment") ? args.at("--experiment") : "index";
        if (experiment != "index" && experiment != "scaling" && experiment != "v1_repro" && experiment != "v3_sweep")
            throw std::runtime_error("Unknown experiment label");
        const std::filesystem::path input = args.at("--input"), output = args.at("--output");
        if (options.version == "v0") {
            const auto manifest = std::filesystem::weakly_canonical(output.string()+".documents.tsv");
            if ((args.contains("--csv") && manifest == std::filesystem::weakly_canonical(args.at("--csv"))) ||
                (args.contains("--verify") && manifest == std::filesystem::weakly_canonical(args.at("--verify"))))
                throw std::runtime_error("V0 document manifest must not overwrite CSV/reference");
        }
        if (inside(output, input) || (args.contains("--csv") && inside(args.at("--csv"), input)))
            throw std::runtime_error("Output and CSV must be outside input directory");
        if (args.contains("--verify") && std::filesystem::weakly_canonical(output) == std::filesystem::weakly_canonical(args.at("--verify")))
            throw std::runtime_error("Output must not overwrite the reference");
        if (args.contains("--csv")) {
            const auto csv_path = std::filesystem::weakly_canonical(args.at("--csv"));
            if (csv_path == std::filesystem::weakly_canonical(output) ||
                (args.contains("--verify") && csv_path == std::filesystem::weakly_canonical(args.at("--verify"))))
                throw std::runtime_error("CSV, output, and reference paths must differ");
        }
        const auto read_start = Clock::now();
        const auto corpus = lab::load_corpus(input);
        const double read_ms = elapsed(read_start);
        const bool qualified = corpus.documents.size() >= 200 && corpus.bytes >= 200ULL * 1024 * 1024;
        if (args.contains("--require-corpus") && args.at("--require-corpus") != "yes") throw std::runtime_error("--require-corpus expects yes");
        if (args.contains("--require-corpus") && !qualified) throw std::runtime_error("Formal corpus needs >=200 files and >=200 MiB");
        std::cout << "files=" << corpus.documents.size() << " bytes=" << corpus.bytes << " unicode=" << lab::unicode_version()
                  << " formal_size=" << (qualified ? "yes" : "no (demo only)") << '\n';
        const bool v1 = options.version == "v1";
        if (v1) std::cout << "V1 " << options.v1_mode << ": intentionally incorrect; frozen-layout prepass INCLUDED in build_ms.\n";
        std::string reference;
        if (!v1) {
            lab::Options baseline;
            auto check = lab::build(corpus, baseline);
            lab::validate(check.index);
            reference = lab::serialize(check.index);
        }
        for (std::size_t n = 0; n < warmup; ++n) {
            auto check = lab::build(corpus, options);
            if (!v1) {
                lab::validate(check.index);
                const auto check_bytes = lab::serialize(check.index);
                if (check_bytes != reference) throw std::runtime_error("Preflight differs from V0");
                lab::write_file(output, check_bytes);
                if (args.contains("--verify") && !lab::files_equal(output, args.at("--verify")))
                    throw std::runtime_error("Preflight file differs from reference");
            }
        }
        std::ofstream csv;
        if (args.contains("--csv")) {
            const std::filesystem::path path = args.at("--csv");
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            const bool header = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
            csv.open(path, std::ios::app);
            csv.imbue(std::locale::classic());
            if (!csv) throw std::runtime_error("Cannot open CSV");
            if (header) csv << "experiment,version,threads,K,merge,v1_mode,repeat,build_ms,merge_ms,canonicalize_ms,compute_ms,read_ms,write_ms,total_occurrences,correct,files,bytes\n";
        }
        std::vector<double> times;
        for (std::size_t n = 1; n <= repeats; ++n) {
            auto result = lab::build(corpus, options);
            const auto sort_start = Clock::now();
            const auto bytes = lab::serialize(result.index);
            const double canonicalize_ms = elapsed(sort_start);
            const double compute_ms = result.build_ms + result.merge_ms + canonicalize_ms;
            const auto write_start = Clock::now();
            lab::write_file(output, bytes);
            const double write_ms = elapsed(write_start);
            bool correct = !v1 && bytes == reference;
            if (!v1) lab::validate(result.index);
            if (args.contains("--verify")) {
                const bool equal = lab::files_equal(output, args.at("--verify"));
                correct = v1 ? equal : correct && equal;
            }
            if (!v1 && !correct) throw std::runtime_error("Output differs from V0/reference");
            times.push_back(compute_ms);
            const auto count = lab::total(result.index);
            std::cout << std::fixed << std::setprecision(3) << "repeat=" << n << " compute_ms=" << compute_ms
                      << " merge_ms=" << result.merge_ms << " total=" << count
                      << " verified=" << (correct ? "yes" : (v1 ? "V1-diagnostic" : "no")) << '\n';
            if (csv.is_open()) {
                csv << std::fixed << std::setprecision(6) << experiment << ',' << options.version << ',' << options.threads << ','
                    << (options.version == "v3" ? options.shards : 0) << ',' << (options.version == "v4" ? options.merge : "na") << ','
                    << (v1 ? options.v1_mode : "na") << ',' << n+repeat_start-1 << ',' << result.build_ms << ',' << result.merge_ms << ','
                    << canonicalize_ms << ',' << compute_ms << ',' << read_ms << ',' << write_ms << ',' << count << ','
                    << (correct ? 1 : 0) << ',' << corpus.documents.size() << ',' << corpus.bytes << '\n';
                csv.flush();
                if (!csv) throw std::runtime_error("CSV write failed");
            }
            if (n == repeats && options.version == "v0") {
                std::vector<lab::Count> counts;
                for (const auto& [word, entry] : result.index) counts.push_back(entry.total);
                std::sort(counts.begin(), counts.end(), std::greater<>());
                const auto top = std::accumulate(counts.begin(), counts.begin()+std::min<std::size_t>(20, counts.size()), lab::Count{0});
                std::cout << std::setprecision(9) << "top20_occurrences=" << top << " top20_share=" << (count ? static_cast<double>(top)/count : 0) << '\n';
                std::string manifest = "doc_id\trelative_path\tbytes\n";
                for (const auto& doc : corpus.documents) manifest += std::to_string(doc.id) + '\t' + doc.path + '\t' + std::to_string(doc.bytes.size()) + '\n';
                lab::write_file(output.string()+".documents.tsv", manifest);
            }
        }
        std::sort(times.begin(), times.end());
        const auto middle = times.size()/2;
        const double median = times.size()%2 ? times[middle] : (times[middle-1]+times[middle])/2;
        std::cout << "median_ms=" << median << " min_ms=" << times.front() << " max_ms=" << times.back() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
