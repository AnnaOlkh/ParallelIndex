#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lab {
using Count = std::uint64_t;
using DocId = std::uint32_t;
struct Document { DocId id; std::string path, bytes; };
struct Corpus { std::vector<Document> documents; std::uint64_t bytes = 0; };
struct Entry { Count total = 0; std::unordered_map<DocId, Count> postings; };
using Index = std::unordered_map<std::string, Entry>;
struct Options {
    std::string version = "v0", merge = "sequential", v1_mode = "defined";
    std::size_t threads = 1, shards = 64;
};
struct Result {
    Index index;
    double build_ms = 0, merge_ms = 0;
};
Corpus load_corpus(const std::filesystem::path& directory);
void tokenize(const std::string& bytes, const std::function<void(const std::string&)>& accept);
void add(Index& index, const std::string& word, DocId doc, Count n = 1);
void merge_into(Index& destination, Index& source);
Result build(const Corpus& corpus, const Options& options);
std::string serialize(const Index& index);
void write_file(const std::filesystem::path& path, const std::string& bytes);
bool files_equal(const std::filesystem::path& a, const std::filesystem::path& b);
Count total(const Index& index);
void validate(const Index& index);
std::string unicode_version();
}
