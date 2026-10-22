#include "core.hpp"
#include "unicode_tables.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace lab
{
    namespace
    {
        char32_t decode(const std::string &text, std::size_t &i)
        {
            const auto byte = [&](std::size_t p)
            { return static_cast<unsigned char>(text[p]); };
            unsigned c = byte(i++);
            if (c < 0x80)
                return c;
            unsigned remaining;
            char32_t value, minimum;
            if (c >= 0xC2 && c <= 0xDF)
            {
                remaining = 1;
                value = c & 31;
                minimum = 0x80;
            }
            else if (c >= 0xE0 && c <= 0xEF)
            {
                remaining = 2;
                value = c & 15;
                minimum = 0x800;
            }
            else if (c >= 0xF0 && c <= 0xF4)
            {
                remaining = 3;
                value = c & 7;
                minimum = 0x10000;
            }
            else
                throw std::runtime_error("Invalid UTF-8 leading byte at offset " + std::to_string(i - 1));
            if (remaining > text.size() - i)
                throw std::runtime_error("Truncated UTF-8 sequence");
            while (remaining--)
            {
                c = byte(i++);
                if ((c & 0xC0) != 0x80)
                    throw std::runtime_error("Invalid UTF-8 continuation byte");
                value = (value << 6) | (c & 63);
            }
            if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
                throw std::runtime_error("Invalid UTF-8 code point");
            return value;
        }
        void encode(char32_t c, std::string &out)
        {
            if (c < 0x80)
                out += static_cast<char>(c);
            else if (c < 0x800)
            {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 63));
            }
            else if (c < 0x10000)
            {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 63));
                out += static_cast<char>(0x80 | (c & 63));
            }
            else
            {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 63));
                out += static_cast<char>(0x80 | ((c >> 6) & 63));
                out += static_cast<char>(0x80 | (c & 63));
            }
        }
        bool alnum(char32_t c)
        {
            if (c < 0x80)
                return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9');
            const auto *p = std::lower_bound(std::begin(unicode::alnum), std::end(unicode::alnum), c,
                                             [](const auto &range, char32_t code)
                                             { return range.last < code; });
            return p != std::end(unicode::alnum) && p->first <= c;
        }
        bool utf8_less(const std::string &a, const std::string &b)
        {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                                [](unsigned char x, unsigned char y)
                                                { return x < y; });
        }
    }

    Corpus load_corpus(const std::filesystem::path &directory)
    {
        if (!std::filesystem::is_directory(directory))
            throw std::runtime_error("Input must be a directory");
        std::vector<std::filesystem::path> paths;
        for (const auto &item : std::filesystem::recursive_directory_iterator(directory))
            if (item.is_regular_file() && !item.is_symlink())
                paths.push_back(item.path());
        std::sort(paths.begin(), paths.end(), [&](const auto &a, const auto &b)
                  { return a.lexically_relative(directory).generic_u8string() < b.lexically_relative(directory).generic_u8string(); });
        if (paths.empty())
            throw std::runtime_error("Input directory contains no regular files");
        if (paths.size() > std::numeric_limits<DocId>::max())
            throw std::runtime_error("Too many documents");
        Corpus corpus;
        for (const auto &path : paths)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("Cannot open input file: " + path.string());
            std::string bytes{std::istreambuf_iterator<char>(file), {}};
            if (file.bad())
                throw std::runtime_error("Cannot read input file: " + path.string());
            corpus.bytes += bytes.size();
            auto relative = path.lexically_relative(directory).generic_u8string();
            corpus.documents.push_back({static_cast<DocId>(corpus.documents.size() + 1),
                                        std::string(relative.begin(), relative.end()), std::move(bytes)});
        }
        return corpus;
    }

    void tokenize(const std::string &text, const std::function<void(const std::string &)> &accept)
    {
        std::string word;
        std::size_t length = 0;
        auto flush = [&]
        { if (length >= 3) accept(word); word.clear(); length = 0; };
        for (std::size_t i = 0; i < text.size();)
        {
            const char32_t c = decode(text, i);
            if (!alnum(c))
            {
                flush();
                continue;
            }
            if (c < 0x80)
            {
                word += static_cast<char>(c >= U'A' && c <= U'Z' ? c + (U'a' - U'A') : c);
                ++length;
                continue;
            }
            const auto *lower = std::lower_bound(std::begin(unicode::lowercase), std::end(unicode::lowercase), c,
                                                 [](const auto &mapping, char32_t code)
                                                 { return mapping.code < code; });
            if (lower != std::end(unicode::lowercase) && lower->code == c)
            {
                for (char32_t mapped : lower->text)
                    encode(mapped, word);
            }
            else
                encode(c, word);
            ++length;
        }
        flush();
    }
    void add(Index &index, const std::string &word, DocId doc, Count n)
    {
        auto &entry = index[word];
        entry.total += n;
        entry.postings[doc] += n;
    }
    void merge_into(Index &destination, Index &source)
    {
        for (auto it = source.begin(); it != source.end();)
        {
            auto found = destination.find(it->first);
            if (found == destination.end())
            {
                auto current = it++;
                destination.insert(source.extract(current));
            }
            else
            {
                found->second.total += it->second.total;
                for (const auto &[doc, count] : it->second.postings)
                    found->second.postings[doc] += count;
                it = source.erase(it);
            }
        }
    }
    std::string serialize(const Index &index)
    {
        std::vector<const Index::value_type *> words;
        words.reserve(index.size());
        for (const auto &word : index)
            words.push_back(&word);
        std::sort(words.begin(), words.end(), [](const auto *a, const auto *b)
                  { return utf8_less(a->first, b->first); });
        std::string out;
        for (const auto *word : words)
        {
            out += word->first + '\t' + std::to_string(word->second.total) + '\t';
            std::vector<std::pair<DocId, Count>> postings(word->second.postings.begin(), word->second.postings.end());
            std::sort(postings.begin(), postings.end(), [](const auto &a, const auto &b)
                      { return a.second != b.second ? a.second > b.second : a.first < b.first; });
            bool first = true;
            for (const auto &[doc, count] : postings)
            {
                if (!first)
                    out += ',';
                first = false;
                out += "doc" + std::to_string(doc) + ':' + std::to_string(count);
            }
            out += '\n';
        }
        return out;
    }
    void write_file(const std::filesystem::path &path, const std::string &bytes)
    {
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file)
            throw std::runtime_error("Cannot write output: " + path.string());
    }
    bool files_equal(const std::filesystem::path &a, const std::filesystem::path &b)
    {
        std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
        if (!x || !y)
            throw std::runtime_error("Cannot open comparison file");
        std::array<char, 65536> bx{}, by{};
        do
        {
            x.read(bx.data(), bx.size());
            y.read(by.data(), by.size());
            if (x.bad() || y.bad())
                throw std::runtime_error("Comparison read error");
            if (x.gcount() != y.gcount() || !std::equal(bx.begin(), bx.begin() + x.gcount(), by.begin()))
                return false;
        } while (x.gcount() != 0);
        return true;
    }
    Count total(const Index &index)
    {
        Count n = 0;
        for (const auto &[word, e] : index)
            n += e.total;
        return n;
    }
    void validate(const Index &index)
    {
        for (const auto &[word, entry] : index)
        {
            Count sum = 0;
            for (const auto &[doc, count] : entry.postings)
            {
                if (doc == 0 || count == 0)
                    throw std::runtime_error("Invalid posting");
                sum += count;
            }
            if (sum != entry.total)
                throw std::runtime_error("Posting sum mismatch: " + word);
        }
    }
    std::string unicode_version() { return unicode::version; }
}
