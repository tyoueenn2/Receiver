#include "receiver/model.hpp"
#include "receiver/engine.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
namespace receiver {
namespace {
using nlohmann::json;
struct Field {
    unsigned id = 0, wire = 0;
    uint64_t integer = 0;
    Bytes bytes;
};
class Proto {
    Bytes data_;
    size_t at_ = 0;
    uint64_t varint() {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            if (at_ == data_.size())
                throw std::runtime_error("Truncated ONNX metadata");
            auto b = data_[at_++];
            if (shift == 63 && (b & 0xfe))
                throw std::runtime_error("Invalid ONNX varint");
            value |= uint64_t(b & 127) << shift;
            if (!(b & 128))
                return value;
        }
        throw std::runtime_error("Invalid ONNX varint");
    }

  public:
    explicit Proto(Bytes data) : data_(data) {}
    bool next(Field& f) {
        if (at_ == data_.size())
            return false;
        auto tag = varint();
        if (!tag || tag >> 3 > 0x1fffffff)
            throw std::runtime_error("Invalid ONNX field");
        f = {};
        f.id = unsigned(tag >> 3);
        f.wire = unsigned(tag & 7);
        if (!f.wire) {
            f.integer = varint();
            return true;
        }
        uint64_t length = f.wire == 2 ? varint() : f.wire == 1 ? 8 : f.wire == 5 ? 4 : UINT64_MAX;
        if (length > data_.size() - at_)
            throw std::runtime_error("Invalid/truncated ONNX field");
        f.bytes = data_.subspan(at_, size_t(length));
        at_ += size_t(length);
        return true;
    }
};
std::string text(Bytes b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}
std::vector<int64_t> dimensions(Bytes value_info, int& element) {
    std::vector<int64_t> dims;
    Proto value(value_info);
    Field v;
    while (value.next(v))
        if (v.id == 2 && v.wire == 2) {
            Proto type(v.bytes);
            Field t;
            while (type.next(t))
                if (t.id == 1 && t.wire == 2) {
                    Proto tensor(t.bytes);
                    Field a;
                    while (tensor.next(a)) {
                        if (a.id == 1 && a.wire == 0)
                            element = int(a.integer);
                        if (a.id != 2 || a.wire != 2)
                            continue;
                        Proto shape(a.bytes);
                        Field d;
                        while (shape.next(d))
                            if (d.id == 1 && d.wire == 2) {
                                int64_t size = -1;
                                Proto dimension(d.bytes);
                                Field n;
                                while (dimension.next(n))
                                    if (n.id == 1 && n.wire == 0)
                                        size = n.integer > INT64_MAX ? -1 : int64_t(n.integer);
                                dims.push_back(size);
                                if (dims.size() > 8)
                                    throw std::runtime_error("Unsupported ONNX rank");
                            }
                    }
                }
        }
    return dims;
}
// Ultralytics commonly writes Python-style integer-key dictionaries. Parse strings
// and keys explicitly; never evaluate metadata as Python or rewrite text inside labels.
json parse_names(const std::string& source) {
    auto parsed = json::parse(source, nullptr, false);
    if (!parsed.is_discarded())
        return parsed;
    json result = json::object();
    size_t at = 0;
    auto space = [&] {
        while (at < source.size() && std::isspace(static_cast<unsigned char>(source[at])))
            ++at;
    };
    auto expect = [&](char c) {
        space();
        if (at == source.size() || source[at++] != c)
            throw std::runtime_error("Invalid model class metadata");
    };
    auto quoted = [&]() {
        space();
        if (at == source.size() || (source[at] != '\'' && source[at] != '"'))
            throw std::runtime_error("Invalid model class string");
        char quote = source[at++];
        std::string value;
        bool closed = false;
        while (at < source.size()) {
            char c = source[at++];
            if (c == quote) {
                closed = true;
                break;
            }
            if (c == '\\') {
                if (at == source.size())
                    throw std::runtime_error("Invalid class escape");
                c = source[at++];
                if (c == 'n')
                    c = '\n';
                else if (c == 'r')
                    c = '\r';
                else if (c == 't')
                    c = '\t';
                else if (c != '\\' && c != '\'' && c != '"')
                    throw std::runtime_error("Unsupported class escape; use JSON names");
            }
            value += c;
        }
        if (!closed)
            throw std::runtime_error("Unterminated model class string");
        return value;
    };
    expect('{');
    space();
    while (at < source.size() && source[at] != '}') {
        std::string key;
        if (source[at] == '\'' || source[at] == '"')
            key = quoted();
        else {
            while (at < source.size() && std::isdigit(static_cast<unsigned char>(source[at])))
                key += source[at++];
        }
        if (key.empty() || result.contains(key))
            throw std::runtime_error("Invalid/duplicate model class ID");
        expect(':');
        result[key] = quoted();
        space();
        if (at < source.size() && source[at] == ',') {
            ++at;
            space();
        } else
            break;
    }
    expect('}');
    space();
    if (at != source.size())
        throw std::runtime_error("Invalid trailing class metadata");
    return result;
}
void read_names(ModelInfo& info) {
    if (!info.metadata.contains("names"))
        return;
    auto names = info.metadata["names"];
    if (names.is_string())
        names = parse_names(names.get<std::string>());
    if ((!names.is_array() && !names.is_object()) || names.empty() || names.size() > 10000)
        throw std::runtime_error("Invalid model class names");
    info.names.clear();
    for (size_t i = 0; i < names.size(); ++i) {
        auto value = names.is_array() ? names.at(i) : names.at(std::to_string(i));
        if (!value.is_string() || value.get<std::string>().empty())
            throw std::runtime_error("Model classes need contiguous IDs and names");
        info.names.push_back(value.get<std::string>());
    }
    info.metadata["names"] = info.names;
}
int checked_size(int64_t size) {
    if (size < 32 || size > 1024 || size % 32)
        throw std::runtime_error("Model input must be square, 32..1024 in multiples of 32");
    return int(size);
}
json sidecar(const Settings& s) {
    auto path = s.metadata.empty() ? s.model + ".json" : s.metadata;
    std::ifstream file(path);
    if (!file) {
        if (!s.metadata.empty())
            throw std::runtime_error("Cannot open model metadata: " + path);
        return json::object();
    }
    if (std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("Model metadata is too large");
    json value;
    file >> value;
    if (!value.is_object())
        throw std::runtime_error("Model metadata must be an object");
    return value;
}
} // namespace
ModelInfo inspect_onnx(Bytes data) {
    ModelInfo info;
    Proto model(data);
    Field f;
    Bytes graph;
    while (model.next(f)) {
        if (f.id == 7 && f.wire == 2) {
            if (!graph.empty())
                throw std::runtime_error("Duplicate ONNX graph");
            graph = f.bytes;
        }
        if (f.id == 14 && f.wire == 2) {
            Proto entry(f.bytes);
            Field p;
            std::string key, value;
            while (entry.next(p))
                if (p.wire == 2) {
                    if (p.id == 1)
                        key = text(p.bytes);
                    if (p.id == 2)
                        value = text(p.bytes);
                }
            if (key.empty() || info.metadata.contains(key))
                throw std::runtime_error("Invalid/duplicate ONNX metadata");
            if (key.size() > 1024 || value.size() > 1024 * 1024)
                throw std::runtime_error("ONNX metadata is too large");
            info.metadata[key] = value;
        }
    }
    if (graph.empty())
        throw std::runtime_error("ONNX model has no graph");
    int inputs = 0, outputs = 0, in_type = 0, out_type = 0;
    std::vector<int64_t> in, out;
    Proto g(graph);
    while (g.next(f)) {
        if (f.wire != 2)
            continue;
        if (f.id == 11) {
            ++inputs;
            in = dimensions(f.bytes, in_type);
        }
        if (f.id == 12) {
            ++outputs;
            out = dimensions(f.bytes, out_type);
        }
        if (f.id == 5) {
            Proto initializer(f.bytes);
            Field t;
            while (initializer.next(t))
                if (t.id == 13 || (t.id == 14 && t.integer == 1))
                    throw std::runtime_error("External-data ONNX models are unsupported");
        }
    }
    if (inputs != 1 || outputs != 1 || in_type != 1 || out_type != 1 || in.size() != 4 || out.size() != 3 ||
        (in[0] != 1 && in[0] > 0) || in[1] != 3 || (out[0] != 1 && out[0] > 0))
        throw std::runtime_error("Expected float32 [1,3,H,W] input and raw [1,4+classes,N] output");
    if (in[2] > 0 && in[3] > 0 && in[2] != in[3])
        throw std::runtime_error("Non-square model input is unsupported");
    info.fixed_size = in[2] > 0 ? checked_size(in[2]) : in[3] > 0 ? checked_size(in[3]) : 0;
    info.dynamic = info.fixed_size == 0;
    info.preferred_size = info.fixed_size;
    if (info.metadata.contains("args")) {
        auto args = json::parse(info.metadata["args"].get<std::string>(), nullptr, false);
        if (args.is_object())
            info.metadata["args"] = args;
        else {
            const auto value = info.metadata["args"].get<std::string>();
            static const std::regex nms(R"rx((^|[,\{])\s*['"]?nms['"]?\s*:\s*(True|true|1)\b)rx");
            if (std::regex_search(value, nms))
                throw std::runtime_error("Embedded NMS is unsupported");
            info.metadata.erase("args");
        }
    }
    read_names(info);
    if (!info.names.empty() && out[1] > 0 && out[1] != int64_t(4 + info.names.size()))
        throw std::runtime_error("Model output does not match its detection class names");
    return info;
}
ModelInfo inspect_model(const Settings& s) {
    if (s.model.empty())
        return {};
    auto ext = std::filesystem::path(s.model).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    ModelInfo info;
    if (ext == ".onnx") {
        std::ifstream file(s.model, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("Cannot open model: " + s.model);
        auto length = file.tellg();
        if (length <= 0 || length > 1024ll * 1024 * 1024)
            throw std::runtime_error("Invalid model length");
        std::vector<uint8_t> data(static_cast<size_t>(length));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(data.data()), length);
        if (!file)
            throw std::runtime_error("Cannot read model");
        info = inspect_onnx(data);
    } else if (ext == ".engine") {
        std::ifstream file(s.model, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open model: " + s.model);
        std::vector<uint8_t> data(65536 + 20);
        file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
        data.resize(size_t(file.gcount()));
        info.metadata = inspect_engine_payload(data).metadata;
        if (!info.metadata.is_object())
            info.metadata = json::object();
    } else if (ext != ".pt")
        throw std::runtime_error("Choose an .onnx, .engine or .pt model");
    auto companion = sidecar(s);
    for (const auto& [key, value] : companion.items())
        info.metadata[key] = value;
    read_names(info);
    if (info.metadata.contains("input_size"))
        info.preferred_size = checked_size(info.metadata["input_size"].get<int>());
    if (info.metadata.contains("imgsz")) {
        auto value = info.metadata["imgsz"];
        if (value.is_string())
            value = json::parse(value.get<std::string>(), nullptr, false);
        if (value.is_number_integer())
            info.preferred_size = checked_size(value.get<int>());
        else if (value.is_array() && value.size() == 2 && value[0] == value[1])
            info.preferred_size = checked_size(value[0].get<int>());
    }
    // Engine export metadata is advisory. The deserialized engine supplies the
    // authoritative shape and optimization profile, including raw plans.
    if (info.fixed_size)
        info.preferred_size = info.fixed_size;
    return info;
}
void reconcile_classes(Settings& s, const std::vector<std::string>& names) {
    if (names.empty())
        return;
    if (!s.selected_class_names.empty()) {
        s.classes.clear();
        for (const auto& name : s.selected_class_names) {
            auto it = std::find(names.begin(), names.end(), name);
            if (it != names.end())
                s.classes.push_back(int(it - names.begin()));
        }
    }
    std::erase_if(s.classes, [&](int id) { return id < 0 || size_t(id) >= names.size(); });
    std::sort(s.classes.begin(), s.classes.end());
    s.classes.erase(std::unique(s.classes.begin(), s.classes.end()), s.classes.end());
    s.selected_class_names.clear();
    for (int id : s.classes)
        s.selected_class_names.push_back(names[size_t(id)]);
}
Settings prepare_model_settings(Settings s) {
    auto info = inspect_model(s);
    if (info.fixed_size)
        s.input_size = info.fixed_size;
    else if (s.auto_size && info.preferred_size)
        s.input_size = info.preferred_size;
    reconcile_classes(s, info.names);
    validate(s);
    return s;
}
bool connection_settings_changed(const Settings& a, const Settings& b) {
    return a.bind_ip != b.bind_ip || a.sender_ip != b.sender_ip || a.pi_ip != b.pi_ip ||
           a.frame_port != b.frame_port || a.pi_port != b.pi_port || a.mouse_backend != b.mouse_backend;
}
bool model_settings_changed(const Settings& a, const Settings& b) {
    return a.model != b.model || a.metadata != b.metadata || a.input_size != b.input_size ||
           a.auto_size != b.auto_size || a.inference_backend != b.inference_backend ||
           a.omni_python != b.omni_python || a.omni_source != b.omni_source ||
           a.omni_device != b.omni_device || a.omni_worker != b.omni_worker;
}
} // namespace receiver
