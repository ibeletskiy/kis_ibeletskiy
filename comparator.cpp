#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <fstream>
#include <exception>
#include <mutex>
#include <ranges>

namespace fs = std::filesystem;

template<class T>
struct CliParam {
    CliParam(T &value, std::vector<std::string_view> keys, std::string_view desc, bool required = false):
            value(value),
            keys(std::move(keys)),
            desc(desc),
            required(required) {}

    bool parse(std::string_view key, const std::string &s) {
        if (std::ranges::find(keys, key) == keys.end())
            return 0;
        if (specified) {
            std::cerr << "Duplicated key " << key << "\n";
            std::exit(1);
        }
        specified = 1;
        if constexpr (std::is_same_v<T, bool>) {
            if (s == "true" || s == "1") {
                value = 1;
            } else if (s == "false" || s == "0") {
                value = 0;
            } else {
                std::cerr << "Boolean value expected\n";
                std::exit(1);
            }
        } else if constexpr (std::is_integral_v<T>) {
            size_t len = 0;
            value = stoi(s, &len);
            if (!len || len < s.size()) {
                std::cerr << "Integer expected\n";
                std::exit(1);
            }
        } else {
            value = T(s);
        }
        return 1;
    }

    void print_desc() {
        for (bool first = 1; auto k : keys) {
            if (first) {
                first = 0;
            } else {
                std::cout << " ";
            }
            std::cout << k;
        }
        std::cout << "\t: ";
        if (required) {
            std::cout << "required. ";
        } else {
            std::cout << "default = " << value << ". ";
        }
        std::cout << desc << "\n";
    }

    T &value;
    std::vector<std::string_view> keys;
    std::string_view desc;
    bool required;
    bool specified = 0;
};

using HashType = uint64_t;

const size_t kOptimization = 10000;

struct OutputFormat {
    fs::path first;
    fs::path second;
    bool is_equal = false;
    double similarity;
};

template <typename T>
double similarity(const std::vector<T>& first, const std::vector<T>& second) {
    // using simple lcs algorithm with optimized memory
    const size_t m = first.size();
    const size_t n = second.size();
    const size_t max = std::max(first.size(), second.size());
    std::vector<size_t> dp(m, max);
    std::vector<size_t> prev(m, max);
    dp[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < m; ++j) {
            if (i != 0) {
                dp[j] = std::min(prev[j], dp[j]);
            }
            if (j != 0) {
                dp[j] = std::min(dp[j - 1], dp[j]);
            }
            if (i != 0 && j != 0 && first[i] == second[j]) {
                dp[j] = std::min(prev[j - 1], dp[j]);
            }
        }
        prev = dp;
        for (size_t j = 0; j < m; ++j) {
            dp[j] = max;
        }
    }
    return static_cast<double>(prev.back()) / max;
}

std::pair<std::vector<uint64_t>, uint64_t> getHashBlocks(const fs::path& path, size_t block = 512) {
    const size_t kPrime = 119;

    std::vector<HashType> seq;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(path.string());
    }
    std::vector<unsigned char> buf(block);
    HashType all_file_hash = 0;
    while (in) {
        in.read((char*)buf.data(), (std::streamsize)block);
        std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }

        // counting hash for block
        size_t current = 0;
        for (int i = 0; i < got; ++i) {
            current = current * kPrime + buf[i];
        }
        all_file_hash = all_file_hash * kPrime + current;

        seq.push_back(current);
    }
    return {seq, all_file_hash};
}

bool checkEqual(const fs::path &a, const fs::path &b) {
    const size_t block = 1<<20;

    if (fs::file_size(a) != fs::file_size(b)) {
        return false;
    }

    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) {
        return false;
    }
    std::vector<char> ba(block);
    std::vector<char> bb(block);
    while (fa && fb) {
        fa.read(ba.data(), (std::streamsize)ba.size());
        fb.read(bb.data(), (std::streamsize)bb.size());
        std::streamsize ca = fa.gcount(), cb = fb.gcount();
        if (ca != cb) {
            return false;
        }
        if (ca == 0) {
            break;
        }
        if (memcmp(ba.data(), bb.data(), (size_t)ca) != 0) {
            return false;
        }
    }
    return true;
}

void outputPairs(const std::string& preambule, const std::vector<OutputFormat>& files) {
    size_t row = 1;
    std::cout << preambule << '\n';
    for (const OutputFormat& cur : files) {
        if (cur.is_equal) {
            std::cout << row++ << ". " << cur.first.string() << " - " << cur.second.string() << "\n";
        } else {
            std::cout << row++ << ". " << cur.first.string() << " - " << cur.second.string() << " similarity: " << cur.similarity * 100 << "%\n";
        }
    }
}

void outputFiles(const std::string& preambule, const std::vector<fs::path>& files) {
    size_t row = 1;
    std::cout << preambule << '\n';
    for (const auto& cur : files) {
        std::cout << row++ << ". " << cur.string() << "\n";
    }
}

// A and B naming was chosen because of analogy with A/B testing :)

int main(int argc, char **argv) {
    int p = 0;
    size_t threads = std::thread::hardware_concurrency();
    fs::path A_dir;
    fs::path B_dir;

    std::tuple params(
            CliParam(A_dir, {"--A-dir", "-A"}, "First (A) directory", true),
            CliParam(B_dir, {"--B-dir", "-B"}, "Second (B) directory", true),
            CliParam(threads, {"--threads", "-j"}, "Number of threads"),
            CliParam(p, {"--percent", "-p"}, "Similarity percent")
    );

    if (argc == 1) {
        std::apply([](auto&... p) {
            (p.print_desc(), ...);
        }, params);
        return 0;
    }

    if (argc % 2 != 1) {
        std::cout << "Missed value for key " << argv[argc - 1] << "\n";
        return 0;
    }

    for (int i = 1; i < argc; i += 2) {
        std::apply([=](auto&... p) {
            bool parsed = (p.parse(argv[i], argv[i + 1]) || ...);
            if (!parsed) {
                std::cerr << "Unexpected key " << argv[i] << "\n";
                std::exit(1);
            }
        }, params);
    }


    std::apply([=](auto&... p) {
        auto specified = [](auto &p) {
            if (p.required && !p.specified) {
                std::cerr << "Mandatory parameter " << p.keys[0] << " unspecified\n";
                return 0;
            }
            return 1;
        };
        bool all_specified = (specified(p) && ...);
        if (!all_specified) {
            std::exit(1);
        }
    }, params);

    std::vector<fs::path> A_files;
    std::vector<fs::path> B_files;
    for (auto &&file : fs::directory_iterator(A_dir)) {
       A_files.push_back(file);
    }
    for (auto &&file : fs::directory_iterator(B_dir)) {
        B_files.push_back(file);
    }

    std::vector<std::thread> ts(threads - 1);

    std::vector<OutputFormat> equal;
    std::mutex equal_lock;
    std::vector<OutputFormat> similar;
    std::mutex similar_lock;
    std::atomic<size_t> next = 0;

    auto workflow = [&](bool progress) {
        size_t count = 0;
        size_t process = next.fetch_add(1);
        for (auto a_file: A_files) {
            for (auto b_file: B_files) {
                if (count++ != process) {
                    continue;
                }
                if (progress) {
                    std::cout << "Progress: " << (100.0 * count / (A_files.size() * B_files.size())) << "%\n";

                }
                auto getBlockSize = [](size_t size) {
                    return (size + kOptimization - 1) / kOptimization;
                };
                double similarity_coef;
                size_t block_size = std::max(getBlockSize(fs::file_size(a_file)), getBlockSize(fs::file_size(b_file)));
                auto [a_blocks, a_hash] = getHashBlocks(a_file, block_size);
                auto [b_blocks, b_hash] = getHashBlocks(b_file, block_size);
                if (a_hash == b_hash && checkEqual(a_file, b_file)) {
                    std::lock_guard lock(equal_lock);
                    equal.push_back(OutputFormat{a_file, b_file, true});
                } else if ((similarity_coef = similarity<HashType>(a_blocks, b_blocks)) * 100 >= p) {
                    std::lock_guard lock(similar_lock);
                    similar.push_back(OutputFormat{a_file, b_file, false, similarity_coef});
                }
                process = next.fetch_add(1);
            }
        }
    };
    for (auto &x : ts) {
        x = std::thread(workflow, 0);
    }
    workflow(1);
    for (auto &x : ts) {
        x.join();
    }

    std::sort(similar.begin(), similar.end(), [](OutputFormat a, OutputFormat b) {
        return a.similarity > b.similarity;
    });

    outputPairs("Equal files:", equal);
    outputPairs("Similar files:", similar);

}
