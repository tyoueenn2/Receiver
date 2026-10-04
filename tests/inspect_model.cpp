#include "receiver/model.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("inspect_model MODEL");
        receiver::Settings s; s.model = argv[1];
        const auto info = receiver::inspect_model(s);
        std::cout << nlohmann::json{{"fixed_size", info.fixed_size}, {"preferred_size", info.preferred_size},
                                  {"dynamic", info.dynamic}, {"names", info.names}}.dump() << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
