#include "receiver/local_mouse.hpp"
#include <thread>
#include <vector>
#include <algorithm>
#include <iostream>
int main() {
    try {
        receiver::LocalMouse mouse;
        bool ready = false;
        std::vector<double> intervals;
        auto previous = receiver::now_ns();
        for (int i = 0; i < 200; ++i) {
            mouse.wait();
            auto state = mouse.poll();
            ready |= state.ready && state.has_motion;
            auto now = receiver::now_ns();
            intervals.push_back(double(now - previous) / 1e6);
            previous = now;
        }
        if (!ready) {
            std::cout << "Unavailable: no interactive Windows desktop.\n";
            return 77;
        }
        std::sort(intervals.begin(), intervals.end());
        std::cout << "Reader wait/poll ms: p50=" << intervals[100] << " p95=" << intervals[190]
                  << " max=" << intervals.back() << " (not end-to-end output timing)\n";
        std::cout << "Windows mouse reader starts and reports state; no output was sent.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
