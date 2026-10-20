#include "receiver/calibration.hpp"
#include "receiver/cursor_protocol.hpp"
#include "receiver/preferences.hpp"
#include "test_support.hpp"
#include <cmath>
#include <iostream>
#include <random>

using namespace receiver;
int main() {
    try {
        std::vector<CalibrationObservation> samples;
        for (int n : {-16, -8, -4, 4, 8, 16}) {
            samples.push_back({double(n), 0, 2.0 * n, -.25 * n});
            samples.push_back({0, double(n), .5 * n, -1.5 * n});
        }
        auto record = fit_calibration(samples, .1);
        CHECK(std::abs(record.xx - 2) < 1e-9 && std::abs(record.xy - .5) < 1e-9);
        CHECK(std::abs(record.yx + .25) < 1e-9 && std::abs(record.yy + 1.5) < 1e-9);
        CHECK((calibrated_counts(record, 11, -10) == std::pair<int, int>{4, 6}));
        test::rejects([&] { calibrated_counts(record, 10000, 10000); });
        test::rejects([&] { calibrated_counts(record, .6, .7); });
        auto broken = samples;
        broken.back().pixels_x += 10;
        test::rejects([&] { fit_calibration(broken, .1); });
        record.verified = true;
        record.layout = 1;
        record.poll_us = 1000;
        test::TempDirectory temporary;
        Settings settings;
        settings.calibration = record;
        settings.return_action.enabled = true;
        auto path = temporary.path / "calibration.json";
        save_settings(settings, path.string());
        auto restored = load_settings(path.string());
        CHECK(restored.calibration.verified && restored.calibration.yy == record.yy &&
              restored.return_action.enabled);
        auto invalid = record;
        invalid.xy = invalid.xx;
        invalid.yy = invalid.yx;
        test::rejects([&] { validate_calibration(invalid); });

        // Independent byte expectations for the mirrored transaction codec.
        move_protocol::Request request;
        request.session = 0x0102030405060708ull;
        request.command = 9;
        request.epoch = 10;
        request.generation = 11;
        request.dx = -123;
        request.dy = 456;
        auto wire = move_protocol::packet(request);
        CHECK(wire.size() == 80 && wire[5] == 1 && wire[8] == 1 && wire[15] == 8);
        CHECK(wire[40] == 255 && wire[43] == 133 && wire[46] == 1 && wire[47] == 200);
        move_protocol::Request decoded;
        CHECK(move_protocol::parse(wire.data(), wire.size(), decoded) && decoded.dx == -123);
        for (size_t n = 0; n < wire.size(); ++n)
            CHECK(!move_protocol::parse(wire.data(), n, decoded));
        wire[70] = 1;
        CHECK(!move_protocol::parse(wire.data(), wire.size(), decoded));
        move_protocol::Ack ack;
        ack.session = 1;
        ack.command = 2;
        ack.epoch = 3;
        ack.generation = 4;
        ack.status = move_protocol::Status::Completed;
        ack.out_x = 123;
        ack.return_x = -123;
        auto answer = move_protocol::packet(ack);
        move_protocol::Ack decoded_ack;
        CHECK(move_protocol::parse_ack(answer.data(), answer.size(), decoded_ack) &&
              decoded_ack.return_x == -123);
        answer[95] = 1;
        CHECK(!move_protocol::parse_ack(answer.data(), answer.size(), decoded_ack));

        cursor_protocol::Sample cursor;
        cursor.flags = 5;
        cursor.session = 1;
        cursor.token = 2;
        cursor.generation = 1;
        cursor.left = -1920;
        cursor.top = -100;
        cursor.width = 1920;
        cursor.height = 1080;
        cursor.crop_width = cursor.crop_height = 128;
        cursor.crop_left = -1024;
        cursor.crop_top = 376;
        cursor.x = -900;
        cursor.y = 400;
        cursor.display = 5;
        auto cursor_wire = cursor_protocol::packet(cursor);
        cursor_protocol::Sample cursor_out;
        CHECK(cursor_protocol::parse(cursor_wire.data(), cursor_wire.size(), cursor_out) &&
              cursor_out.left == -1920);
        cursor_wire[84] = 1;
        CHECK(!cursor_protocol::parse(cursor_wire.data(), cursor_wire.size(), cursor_out));

        Frame before, after;
        before.header.width = after.header.width = 128;
        before.header.height = after.header.height = 128;
        before.header.format = after.header.format = 1;
        before.header.bytes = after.header.bytes = 128 * 128 * 3;
        std::mt19937 random(47);
        for (size_t i = 0; i < before.header.bytes; ++i)
            before.pixels[i] = uint8_t(random());
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                for (int channel = 0; channel < 3; ++channel) {
                    const int sx = x - 5, sy = y + 3;
                    after.pixels[size_t(y * 128 + x) * 3 + channel] =
                        (sx >= 0 && sy >= 0 && sx < 128 && sy < 128)
                            ? before.pixels[size_t(sy * 128 + sx) * 3 + channel]
                            : 0;
                }
        auto displacement = register_view(before, after, 12);
        CHECK(displacement.x == 5 && displacement.y == -3 && displacement.patches >= 3);
        std::fill(after.pixels.begin(), after.pixels.begin() + after.header.bytes, 0);
        test::rejects([&] { register_view(before, after, 12); });
        std::cout << test::checks << " calibration math, geometry, wire, profile, and image checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
