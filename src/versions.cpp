#include "core.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lab {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
template<class Function> void parallel(std::size_t n, Function task) {
    std::vector<std::exception_ptr> errors(n);
    {
        std::vector<std::jthread> workers;
        workers.reserve(n);
        for (std::size_t i = 0; i < n; ++i) workers.emplace_back([&, i] {
            try { task(i); } catch (...) { errors[i] = std::current_exception(); }
        });
    }
    for (auto& error : errors) if (error) std::rethrow_exception(error);
}
std::vector<std::vector<std::size_t>> distribute(const Corpus& corpus, std::size_t threads) {
    std::vector<std::size_t> order(corpus.documents.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        return corpus.documents[a].bytes.size() > corpus.documents[b].bytes.size();
    });
    std::vector<std::vector<std::size_t>> jobs(threads);
    std::vector<std::uint64_t> sizes(threads, 0);
    for (auto doc : order) {
        const auto worker = static_cast<std::size_t>(std::min_element(sizes.begin(), sizes.end()) - sizes.begin());
        jobs[worker].push_back(doc);
        sizes[worker] += corpus.documents[doc].bytes.size();
    }
    return jobs;
}
}

Result build(const Corpus& corpus, const Options& options) {
    if (options.threads == 0 || options.shards == 0) throw std::runtime_error("Threads and shards must be positive");
    if (options.version != "v0" && options.version != "v1" && options.version != "v2" &&
        options.version != "v3" && options.version != "v4") throw std::runtime_error("Unknown version");
    if (options.merge != "sequential" && options.merge != "tree") throw std::runtime_error("Unknown merge method");
    if (options.v1_mode != "defined" && options.v1_mode != "unsafe") throw std::runtime_error("Unknown V1 mode");
#if !LAB_UNSAFE_V1
    if (options.version == "v1" && options.v1_mode == "unsafe")
        throw std::runtime_error("Unsafe V1 disabled; configure with -DENABLE_UNSAFE_V1=ON to enable deliberate UB");
#endif
    const auto threads = options.version == "v0" ? std::size_t{1} : options.threads;
    const auto jobs = distribute(corpus, threads);
    Result result;
    const auto start = Clock::now();
    auto process = [&](std::size_t worker, auto update) {
        for (auto id : jobs[worker]) {
            const auto& doc = corpus.documents[id];
            try { tokenize(doc.bytes, [&](const std::string& word) { update(word, doc.id); }); }
            catch (const std::exception& e) { throw std::runtime_error(doc.path + ": " + e.what()); }
        }
    };
    if (options.version == "v0") {
        // V0
        process(0, [&](const auto& word, auto doc) { add(result.index, word, doc); });
    } else if (options.version == "v1") {
        // V1
        for (const auto& doc : corpus.documents) tokenize(doc.bytes, [&](const auto& word) {
            result.index[word].postings.try_emplace(doc.id, 0);
        });
        std::unordered_map<std::string, std::unique_ptr<std::atomic<Count>>> counters;
        if (options.v1_mode == "defined")
            for (const auto& [word, entry] : result.index)
                counters.emplace(word, std::make_unique<std::atomic<Count>>(0));
        parallel(threads, [&](auto worker) { process(worker, [&](const auto& word, auto doc) {
            auto& entry = const_cast<Entry&>(std::as_const(result.index).at(word));
            ++entry.postings.at(doc);
            if (options.v1_mode == "defined") {
                auto& counter = *std::as_const(counters).at(word);
                const Count old = counter.load(std::memory_order_relaxed);
                counter.store(old + 1, std::memory_order_relaxed);
            } else {
#if LAB_UNSAFE_V1
                const Count old = entry.total;
                entry.total = old + 1;
#endif
            }
        }); });
        if (options.v1_mode == "defined")
            for (auto& [word, entry] : result.index) entry.total = counters.at(word)->load(std::memory_order_relaxed);
    } else if (options.version == "v2") {
        // V2
        std::mutex mutex;
        parallel(threads, [&](auto worker) { process(worker, [&](const auto& word, auto doc) {
            const std::lock_guard lock(mutex);
            add(result.index, word, doc);
        }); });
    } else if (options.version == "v3") {
        // V3
        struct Shard { std::mutex mutex; Index index; };
        auto shards = std::make_unique<Shard[]>(options.shards);
        parallel(threads, [&](auto worker) { process(worker, [&](const auto& word, auto doc) {
            auto& shard = shards[std::hash<std::string>{}(word) % options.shards];
            const std::lock_guard lock(shard.mutex);
            add(shard.index, word, doc);
        }); });
        for (std::size_t k = 0; k < options.shards; ++k) merge_into(result.index, shards[k].index);
    } else {
        // V4
        std::vector<Index> local(threads);
        parallel(threads, [&](auto worker) { process(worker, [&](const auto& word, auto doc) {
            add(local[worker], word, doc);
        }); });
        result.build_ms = elapsed(start);
        const auto merge_start = Clock::now();
        if (options.merge == "sequential") {
            for (std::size_t i = 1; i < threads; ++i) merge_into(local[0], local[i]);
        } else {
            for (std::size_t stride = 1; stride < threads; stride *= 2) {
                const auto pairs = (threads + 2 * stride - 1) / (2 * stride);
                parallel(pairs, [&](std::size_t pair) {
                    const auto left = pair * 2 * stride, right = left + stride;
                    if (right < threads) merge_into(local[left], local[right]);
                });
            }
        }
        result.index = std::move(local[0]);
        result.merge_ms = elapsed(merge_start);
        return result;
    }
    result.build_ms = elapsed(start);
    return result;
}
}
