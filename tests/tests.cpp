#include "core.hpp"
#include <atomic>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string utf8(std::u8string_view text) { return {reinterpret_cast<const char*>(text.data()), text.size()}; }
template<class F> void throws(F function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    require(failed, "Expected rejection");
}
}
int main() {
    try {
        const auto corpus = lab::load_corpus(FIXTURE_DIR);
        lab::Options options;
        const auto baseline = lab::build(corpus, options);
        lab::validate(baseline.index);
        const auto expected = utf8(u8"123\t2\tdoc1:1,doc2:1\napple\t3\tdoc1:2,doc2:1\nbanana\t2\tdoc1:1,doc2:1\ncat\t2\tdoc1:1,doc2:1\ndog\t1\tdoc1:1\nкіт\t3\tdoc1:2,doc2:1\nїжак\t2\tdoc1:1,doc2:1\n");
        require(lab::serialize(baseline.index) == expected, "Golden UTF-8 output mismatch");
        require(lab::total(baseline.index) == 15, "Accepted token total mismatch");
        lab::Corpus stress;
        for (lab::DocId id = 1; id <= 17; ++id) {
            std::string text;
            for (int i = 0; i < 400; ++i) text += "alpha beta alpha gamma 123 ";
            stress.bytes += text.size();
            stress.documents.push_back({id, std::to_string(id), std::move(text)});
        }
        const auto stress_expected = lab::serialize(lab::build(stress, options).index);
        for (const auto* version : {"v2","v3","v4"}) for (const auto* merge : {"sequential","tree"}) {
            options = {}; options.version = version; options.threads = 5; options.merge = merge;
            const auto result = lab::build(stress, options);
            lab::validate(result.index);
            require(lab::serialize(result.index)==stress_expected, "Contended hot-word stress mismatch");
        }
        for (const auto* version : {"v2", "v3", "v4"}) for (std::size_t threads : {1,2,3,4,8})
            for (const auto* merge : {"sequential", "tree"}) {
                options = {}; options.version = version; options.threads = threads; options.merge = merge;
                for (std::size_t k : {1,4,16,64,256,1024,4096}) {
                    if (options.version != "v3" && k != 1) continue;
                    options.shards = k;
                    const auto result = lab::build(corpus, options);
                    lab::validate(result.index);
                    require(lab::serialize(result.index) == expected, "Parallel mismatch");
                }
            }
        std::vector<std::string> tokens;
        lab::tokenize(utf8(u8"ab аб 12 abc АБВ 123 -- can't 😀 統計学"), [&](const auto& s) { tokens.push_back(s); });
        require(tokens == std::vector<std::string>{"abc",utf8(u8"абв"),"123","can",utf8(u8"統計学")}, "Normalization rules");
        tokens.clear();
        lab::tokenize(utf8(u8"İST ABC\uFEFFDEF"), [&](const auto& s) { tokens.push_back(s); });
        require(tokens == std::vector<std::string>{"ist","abc","def"}, "Simple lowercase and BOM separator");
        for (const auto& bad : {std::string("\xC0\xAF"),std::string("\xED\xA0\x80"),std::string("\xF4\x90\x80\x80"),std::string("\xE2\x82"),std::string("\x80")})
            throws([&] { lab::tokenize(bad, [](const auto&) {}); });
        lab::Index ties;
        lab::add(ties,"word",10); lab::add(ties,"word",2); lab::add(ties,"word",3,2);
        require(lab::serialize(ties) == "word\t4\tdoc3:2,doc2:1,doc10:1\n", "Numeric tie order");
        lab::Index other;
        lab::add(other,"word",3,4); lab::add(other,"new",1);
        lab::merge_into(ties,other); lab::validate(ties);
        require(other.empty() && ties.at("word").postings.at(3)==6, "Merge must add colliding counts");
        options = {}; options.version = "v1";
        require(lab::serialize(lab::build(corpus,options).index)==expected, "Single-thread defined V1");
        std::atomic<int> count{0}; std::barrier gate(2);
        auto race = [&] { const auto old = count.load(std::memory_order_relaxed); gate.arrive_and_wait(); count.store(old+1,std::memory_order_relaxed); };
        { std::jthread a(race), b(race); }
        require(count.load()==1, "Controlled lost-update demonstration");
        options = {}; options.version = "v4"; options.threads = 3;
        lab::Corpus malformed{{{1,"bad.txt",std::string("\x80")}},1};
        throws([&] { (void)lab::build(malformed,options); });
        lab::Corpus empty{{{1,"empty.txt",""}},0};
        require(lab::build(empty,options).index.empty(), "Empty document");
#if !LAB_UNSAFE_V1
        options = {}; options.version = "v1"; options.v1_mode = "unsafe";
        throws([&] { (void)lab::build(corpus,options); });
#else
        options = {}; options.version = "v1"; options.v1_mode = "unsafe";
        require(lab::serialize(lab::build(corpus, options).index)==expected, "Single-worker literal V1");
#endif
        options = {};
        options.shards = 0;
        throws([&] { (void)lab::build(corpus,options); });
        std::cout << "All tests passed: golden output, versions/K/P/merges, Unicode, errors, defined lost update.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
