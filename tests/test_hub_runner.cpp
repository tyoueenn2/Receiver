#include "receiver/test_hub.hpp"
#include <iostream>
int main(int argc, char** argv) {
    receiver::TestHub hub;
    receiver::TestOptions options;
    options.python = argc > 1 ? argv[1] : receiver::TestHub::find_python();
    std::vector<bool> selected(12, false);
    for (int i = 0; i < 7; ++i)
        selected[i] = true;
    hub.start(selected, options);
    while (hub.running())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool passed = true;
    auto results = hub.entries();
    for (int i = 0; i < 7; ++i) {
        std::cout << results[i].name << ": " << results[i].status << '\n';
        if (results[i].status != "Passed") {
            passed = false;
            std::cout << results[i].details << '\n';
        }
    }
    std::cout << hub.report_path() << '\n';
    return passed ? 0 : 1;
}
