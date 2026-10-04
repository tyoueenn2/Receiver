#pragma once
#include "receiver/model.hpp"
#include "receiver/network.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace test {
inline int checks = 0;
inline void check(bool value, const char* expression) {
    ++checks;
    if (!value)
        throw std::runtime_error(std::string("Failed: ") + expression);
}
template <class F> void rejects(F f) {
    try {
        f();
    } catch (const std::exception&) {
        ++checks;
        return;
    }
    throw std::runtime_error("Expected rejection");
}
class TempDirectory {
    std::filesystem::path parent_;

  public:
    std::filesystem::path path;
    TempDirectory() {
        parent_ = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
        path = parent_ / ("receiver-ci-" + std::to_string(receiver::random_id()));
        std::filesystem::create_directory(path);
    }
    ~TempDirectory() {
        // Never recursively remove a computed path unless it is our immediate temp child.
        if (path.parent_path() == parent_ && path.filename().string().starts_with("receiver-ci-")) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
    TempDirectory(const TempDirectory&) = delete;
};
using Buffer = std::vector<uint8_t>;
inline void varint(Buffer& b, uint64_t n) {
    while (n > 127) {
        b.push_back(uint8_t(n) | 128);
        n >>= 7;
    }
    b.push_back(uint8_t(n));
}
inline void integer(Buffer& b, unsigned id, uint64_t n) {
    varint(b, id << 3);
    varint(b, n);
}
inline void message(Buffer& b, unsigned id, receiver::Bytes data) {
    varint(b, (id << 3) | 2);
    varint(b, data.size());
    b.insert(b.end(), data.begin(), data.end());
}
inline void string(Buffer& b, unsigned id, const std::string& s) {
    message(b, id, receiver::Bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}
inline Buffer value(std::initializer_list<int> dims, int type = 1) {
    Buffer shape;
    for (int n : dims) {
        Buffer d;
        if (n < 0)
            string(d, 2, "dynamic");
        else
            integer(d, 1, uint64_t(n));
        message(shape, 1, d);
    }
    Buffer tensor;
    integer(tensor, 1, uint64_t(type));
    message(tensor, 2, shape);
    Buffer type_info;
    message(type_info, 1, tensor);
    Buffer info;
    message(info, 2, type_info);
    return info;
}
inline void metadata(Buffer& model, const std::string& key, const std::string& val) {
    Buffer entry;
    string(entry, 1, key);
    string(entry, 2, val);
    message(model, 14, entry);
}
inline Buffer onnx(std::initializer_list<int> input = {1, 3, 640, 640},
                   std::initializer_list<int> output = {1, 6, 10}, int type = 1) {
    Buffer graph;
    message(graph, 11, value(input, type));
    message(graph, 12, value(output));
    Buffer model;
    message(model, 7, graph);
    metadata(model, "task", "detect");
    metadata(model, "names", R"({"0":"object","1":"other"})");
    return model;
}
inline void write(const std::filesystem::path& path, receiver::Bytes data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!f)
        throw std::runtime_error("Fixture write failed");
}
} // namespace test
#define CHECK(x) test::check(bool(x), #x)
