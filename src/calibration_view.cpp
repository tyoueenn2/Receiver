#include "receiver/calibration.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace receiver {
namespace {
struct Image {
    int w = 0, h = 0;
    std::vector<double> gray;
    double at(int x, int y) const {
        return gray[size_t(y) * w + x];
    }
};
Image gray(const Frame& f) {
    Image im;
    im.w = f.header.width;
    im.h = f.header.height;
    const int c = f.header.format == 2 ? 4 : 3;
    if ((f.header.format != 1 && f.header.format != 2) || im.w < 64 || im.h < 64 || f.header.bytes != uint32_t(im.w * im.h * c) ||
        f.pixels.size() < f.header.bytes)
        throw std::runtime_error("View calibration needs a valid image of at least 64 by 64 pixels");
    im.gray.resize(size_t(im.w) * im.h);
    for (size_t i = 0; i < im.gray.size(); ++i) {
        const auto* p = f.pixels.data() + i * c;
        im.gray[i] =
            c == 4 ? .114 * p[0] + .587 * p[1] + .299 * p[2] : .299 * p[0] + .587 * p[1] + .114 * p[2];
    }
    return im;
}
double correlation(const Image& a, const Image& b, int ax, int ay, int bx, int by) {
    double aa = 0, bb = 0, ab = 0, sa = 0, sb = 0;
    constexpr int radius = 6, n = 169;
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            double av = a.at(ax + x, ay + y), bv = b.at(bx + x, by + y);
            aa += av * av;
            bb += bv * bv;
            ab += av * bv;
            sa += av;
            sb += bv;
        }
    aa -= sa * sa / n;
    bb -= sb * sb / n;
    ab -= sa * sb / n;
    if (aa < 100 * n || bb < 100 * n)
        return -1;
    return ab / std::sqrt(aa * bb);
}
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
} // namespace
ViewDisplacement register_view(const Frame& before, const Frame& after, int search) {
    const auto a = gray(before), b = gray(after);
    if (a.w != b.w || a.h != b.h || search < 1 || search > 48)
        throw std::runtime_error("View geometry changed during calibration");
    std::vector<double> xs, ys, scores;
    for (int gy : {1, 2, 3})
        for (int gx : {1, 2, 3}) {
            if (gx == 2 && gy == 2)
                continue; // Keep the crosshair and central foreground out of the fit.
            int x = gx * a.w / 4, y = gy * a.h / 4;
            double best = -1, second = -1;
            int dx = 0, dy = 0;
            std::vector<std::pair<double, std::pair<int, int>>> candidates;
            for (int oy = -search; oy <= search; ++oy)
                for (int ox = -search; ox <= search; ++ox) {
                    if (x + ox < 6 || y + oy < 6 || x + ox >= b.w - 6 || y + oy >= b.h - 6)
                        continue;
                    const double v = correlation(a, b, x, y, x + ox, y + oy);
                    candidates.push_back({v, {ox, oy}});
                    if (v > best) {
                        best = v;
                        dx = ox;
                        dy = oy;
                    }
                }
            for (const auto& c : candidates)
                if (std::abs(c.second.first - dx) > 2 || std::abs(c.second.second - dy) > 2)
                    second = std::max(second, c.first);
            if (best < .85 || best - second < .03 || std::abs(dx) == search || std::abs(dy) == search)
                continue;
            // A forward match must also match the original patch on the reverse search.
            double reverse = -1;
            int rx = 0, ry = 0;
            for (int oy = -search; oy <= search; ++oy)
                for (int ox = -search; ox <= search; ++ox) {
                    if (x + dx + ox < 6 || y + dy + oy < 6 || x + dx + ox >= a.w - 6 ||
                        y + dy + oy >= a.h - 6)
                        continue;
                    double v = correlation(b, a, x + dx, y + dy, x + dx + ox, y + dy + oy);
                    if (v > reverse) {
                        reverse = v;
                        rx = ox;
                        ry = oy;
                    }
                }
            if (std::abs(rx + dx) > 1 || std::abs(ry + dy) > 1)
                continue;
            xs.push_back(dx);
            ys.push_back(dy);
            scores.push_back(best);
        }
    if (xs.size() < 3)
        throw std::runtime_error(
            "Not enough stable texture. Hold a stationary scene with no moving foreground and retry.");
    double mx = median(xs), my = median(ys), score = 0;
    int accepted = 0;
    for (size_t i = 0; i < xs.size(); ++i)
        if (std::hypot(xs[i] - mx, ys[i] - my) <= 1.5) {
            ++accepted;
            score += scores[i];
        }
    if (accepted < 3 || size_t(accepted) * 4 < xs.size() * 3)
        throw std::runtime_error("Scene motion is inconsistent across the image");
    return {mx, my, score / accepted, accepted};
}
} // namespace receiver
