#include "receiver/injection.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace receiver;
using namespace std::chrono_literals;
static int checks = 0;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string("Line ") + std::to_string(__LINE__) + ": " #x);          \
    } while (false)

static Telemetry upt3(uint64_t server = 900, uint32_t poll_us = 1000) {
    Telemetry t;
    t.kind = Telemetry::Kind::upt3;
    t.ready = true;
    t.server = server;
    t.endpoint_poll_us = poll_us;
    t.motion_generation = 1;
    t.sample_ns = 1;
    return t;
}
static std::array<uint8_t, 48> ack(ClickStatus status, uint64_t server, uint64_t client,
                                   uint64_t command, uint8_t button, uint32_t accepted,
                                   uint32_t completed, uint16_t depth = 0) {
    std::array<uint8_t, 48> p{};
    std::memcpy(p.data(), "UPA1", 4);
    p[4] = 2;
    p[5] = uint8_t(status);
    p[6] = button;
    put64(p.data() + 8, client);
    put64(p.data() + 16, command);
    put32(p.data() + 24, accepted);
    put32(p.data() + 28, completed);
    put64(p.data() + 32, server);
    put16(p.data() + 40, depth);
    return p;
}
static uint64_t send_one(InjectedButtonManager& buttons, int64_t now) {
    auto due = buttons.due_requests(now);
    CHECK(due.size() == 1);
    return due[0].id;
}

int main() {
    try {
        // Exact UPX1 network-order vector, including signed fields, complete mask and zero tail.
        auto golden = movement(0x89abcdef, -2, 0x1234, -128, 127, 0xa5);
        const std::array<uint8_t, 16> expected = {'U','P','X','1',0x89,0xab,0xcd,0xef,
                                                  0xff,0xfe,0x12,0x34,0x80,0x7f,0xa5,0};
        CHECK(golden == expected);
        CHECK(movement(1, -99999, 99999, -999, 999, 0)[8] == 0x80);
        uint32_t sequence = 0xffffffffu;
        CHECK(be32(movement(sequence++, 0, 0, 0, 0, 0).data() + 4) == 0xffffffffu);
        CHECK(be32(movement(sequence++, 0, 0, 0, 0, 0).data() + 4) == 0);

        InjectedButtonManager buttons(77);
        int wakes = 0;
        buttons.set_wake([&] { ++wakes; });

        // Physical masks never enter manager ownership or the UPX1 synthetic byte.
        Telemetry physical;
        physical.physical = 0xa4;
        buttons.button_down(1);
        auto move = movement(8, 12, -4, -2, 3, buttons.snapshot().mask);
        CHECK(physical.physical == 0xa4 && move[14] == 1);
        CHECK(be16(move.data() + 8) == 12 && be16(move.data() + 10) == uint16_t(-4));
        CHECK(move[12] == uint8_t(-2) && move[13] == 3 && move[15] == 0);
        for (auto kind : {Telemetry::Kind::upt1, Telemetry::Kind::upt2_public,
                          Telemetry::Kind::upt2_legacy, Telemetry::Kind::upt3}) {
            auto observed = upt3();
            observed.kind = kind;
            observed.physical = 0xff;
            buttons.observe_telemetry(observed);
            CHECK(buttons.snapshot().mask == 1);
        }
        auto press = movement(9, 0, 0, 0, 0, buttons.snapshot().mask);
        CHECK(press[14] == 1 && !be16(press.data() + 8) && !be16(press.data() + 10));
        buttons.button_up(1);
        auto released = buttons.snapshot();
        CHECK(released.mask == 0 && !released.release_snapshot_batches);
        CHECK(injected_snapshot_due(released, true, released.revision - 1, 1, 2));

        // Manual and reference-counted scoped holds coexist; stale guards are generation-invalidated.
        auto first = buttons.hold(2);
        auto second = buttons.hold(2);
        buttons.button_down(2);
        first.release();
        CHECK(buttons.snapshot().mask == 2);
        buttons.button_up(2);
        CHECK(buttons.snapshot().mask == 2);
        second.release();
        CHECK(buttons.snapshot().mask == 0);
        auto stale = buttons.hold(4);
        buttons.release_all(ReleaseReason::disarm, true);
        CHECK(buttons.snapshot().mask == 0 && buttons.snapshot().release_snapshot_batches == 1);
        stale.release();
        CHECK(buttons.snapshot().mask == 0);

        // Every centralized safety reason creates its own three-snapshot batch.
        const ReleaseReason reasons[] = {
            ReleaseReason::manual,          ReleaseReason::disarm,
            ReleaseReason::shutdown,        ReleaseReason::sender_loss,
            ReleaseReason::sender_restart,  ReleaseReason::stale_telemetry,
            ReleaseReason::pi_error,        ReleaseReason::gpu_error,
            ReleaseReason::configuration_restart,
            ReleaseReason::server_epoch_change,
            ReleaseReason::command_id_exhaustion,
        };
        uint32_t release_sequence = 0xfffffff0u;
        buttons.reset(77);
        for (auto reason : reasons) {
            buttons.button_down(1);
            auto guard = buttons.hold(3);
            buttons.release_all(reason, true);
            auto state = buttons.snapshot();
            CHECK(state.mask == 0 && state.release_snapshot_batches >= 1 && state.release_barrier);
            CHECK(buttons.metrics().last_release_reason == release_reason_name(reason));
            for (int copy = 0; copy < 3; ++copy) {
                const auto packet = movement(release_sequence++, 0, 0, 0, 0, state.wire_mask());
                CHECK(be32(packet.data() + 4) == release_sequence - 1 &&
                      !be16(packet.data() + 8) && !be16(packet.data() + 10) &&
                      !packet[12] && !packet[13] && !packet[14] && !packet[15]);
            }
            buttons.mark_snapshot_sent(state.revision, 0, true);
        }

        // Virtual-time heartbeat: exactly due at 75 ms, never due for an unchanged zero mask.
        buttons.reset(77);
        auto zero = buttons.snapshot();
        CHECK(!injected_snapshot_due(zero, false, 0, 0, 80'000'000));
        buttons.button_down(3);
        auto state = buttons.snapshot();
        CHECK(injected_snapshot_due(state, false, 0, 0, 1));
        CHECK(!injected_snapshot_due(state, true, state.revision, 1'000'000, 75'999'999));
        CHECK(injected_snapshot_due(state, true, state.revision, 1'000'000, 76'000'000));

        // Clicks fail locally until valid UPT3 endpoint timing is known.
        buttons.reset(77);
        bool rejected = false;
        try { buttons.click(1); } catch (const std::runtime_error&) { rejected = true; }
        CHECK(rejected && buttons.metrics().click_submitted == 0);
        auto capability = upt3();
        capability.physical = 0xff;
        capability.applied_persistent = 0x40;
        capability.scheduled = 0x80;
        buttons.observe_telemetry(capability);
        CHECK(buttons.snapshot().mask == 0 && buttons.metrics().pi_applied_persistent_mask == 0x40 &&
              buttons.metrics().pi_scheduled_mask == 0x80);
        Telemetry button_only = capability;
        button_only.kind = Telemetry::Kind::upt1;
        buttons.observe_telemetry(button_only);
        CHECK(!buttons.metrics().has_upt3 && !buttons.metrics().endpoint_poll_us &&
              !buttons.metrics().pi_applied_persistent_mask && !buttons.metrics().pi_scheduled_mask);
        buttons.observe_telemetry(capability);
        rejected = false;
        try { buttons.click(1, 1, 999us, 0us); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);

        // Exact 40-byte UPC1 v2 golden request. Millisecond-only 32-byte framing is gone.
        auto one = buttons.click(1, 2, 12'345us, 20'001us);
        auto two = buttons.click(2, 1, 8ms, 0us);
        CHECK(one == 1 && two == 2 && buttons.metrics().pending_clicks == 2);
        auto due = buttons.due_requests(1'000'000);
        CHECK(due.size() == 1 && due[0].id == one && due[0].bytes.size() == 40);
        const auto request = due[0].bytes;
        CHECK(!std::memcmp(request.data(), "UPC1", 4) && request[4] == 2 && request[5] == 1 &&
              request[6] == 1 && request[7] == 0);
        CHECK(be64(request.data() + 8) == 77 && be64(request.data() + 16) == one);
        CHECK(be32(request.data() + 24) == 2 && be32(request.data() + 28) == 12'345 &&
              be32(request.data() + 32) == 20'001 && be32(request.data() + 36) == 0);
        CHECK(!parse_click_ack(Bytes(request.data(), 32)));

        // Lost request/ACK retries immutable bytes and ID at 50 ms; N+1 waits for acceptance.
        CHECK(buttons.due_requests(50'999'999).empty());
        due = buttons.due_requests(51'000'000);
        CHECK(due.size() == 1 && due[0].id == one && due[0].bytes == request);
        CHECK(buttons.metrics().click_retries == 1 && buttons.metrics().click_timeouts == 1);
        CHECK(buttons.process_click_ack(ack(ClickStatus::accepted, 900, 77, one, 1, 2, 0),
                                           52'000'000) == AckResult::handled);
        due = buttons.due_requests(52'000'001);
        CHECK(due.size() == 1 && due[0].id == two);

        // Completed can precede delayed Accepted; duplicate/reordered terminal ACKs count once.
        CHECK(buttons.process_click_ack(ack(ClickStatus::duplicate, 900, 77, one, 1, 2, 0),
                                           53'000'000) == AckResult::handled);
        CHECK(buttons.process_click_ack(ack(ClickStatus::completed, 900, 77, two, 2, 1, 1),
                                           54'000'000) == AckResult::handled);
        CHECK(buttons.process_click_ack(ack(ClickStatus::accepted, 900, 77, two, 2, 1, 0),
                                           55'000'000) == AckResult::ignored);
        CHECK(buttons.process_click_ack(ack(ClickStatus::completed, 900, 77, one, 1, 2, 2),
                                           56'000'000) == AckResult::handled);
        CHECK(buttons.process_click_ack(ack(ClickStatus::completed, 900, 77, one, 1, 2, 2),
                                           57'000'000) == AckResult::ignored);
        CHECK(buttons.metrics().click_accepted == 2 && buttons.metrics().click_completed == 2);

        // QueueFull is bounded backpressure, not rejection, and always retries the same ID.
        auto backpressured = buttons.click(3, 1, 2ms, 0us);
        CHECK(send_one(buttons, 100'000'000) == backpressured);
        CHECK(buttons.process_click_ack(ack(ClickStatus::queue_full, 900, 77, backpressured, 3, 0, 0),
                                           101'000'000) == AckResult::handled);
        CHECK(buttons.due_requests(125'999'999).empty());
        due = buttons.due_requests(126'000'000);
        CHECK(due.size() == 1 && due[0].id == backpressured);
        CHECK(buttons.process_click_ack(ack(ClickStatus::queue_full, 900, 77, backpressured, 3, 0, 0),
                                           127'000'000) == AckResult::handled);
        CHECK(buttons.due_requests(176'999'999).empty());
        due = buttons.due_requests(177'000'000);
        CHECK(due.size() == 1 && due[0].id == backpressured);
        CHECK(buttons.process_click_ack(ack(ClickStatus::queue_full, 900, 77, backpressured, 3, 0, 0),
                                           178'000'000) == AckResult::handled);
        CHECK(buttons.due_requests(252'999'999).empty());
        due = buttons.due_requests(253'000'000);
        CHECK(due.size() == 1 && due[0].id == backpressured);
        CHECK(buttons.process_click_ack(ack(ClickStatus::accepted, 900, 77, backpressured, 3, 1, 0),
                                           254'000'000) == AckResult::handled);
        CHECK(buttons.metrics().click_queue_full == 3 && buttons.metrics().click_rejected_before_acceptance == 0);
        CHECK(buttons.due_requests(328'999'999).empty());
        due = buttons.due_requests(329'000'000);
        CHECK(due.size() == 1 && due[0].id == backpressured); // accepted lease/status retry at 75 ms
        CHECK(buttons.process_click_ack(ack(ClickStatus::completed, 900, 77, backpressured, 3, 1, 1),
                                           330'000'000) == AckResult::handled);

        // Every terminal response class, plus strict impossible-count and reserved-byte checks.
        const ClickStatus terminal_rejections[] = {ClickStatus::busy, ClickStatus::unsupported_button,
            ClickStatus::button_active, ClickStatus::not_ready};
        int64_t clock = 400'000'000;
        for (auto status : terminal_rejections) {
            auto id = buttons.click(4, 1, 2ms, 0us);
            CHECK(send_one(buttons, clock) == id);
            CHECK(buttons.process_click_ack(ack(status, 900, 77, id, 4, 0, 0), clock + 1) ==
                  AckResult::handled);
            clock += 1'000'000;
        }
        auto cancelled = buttons.click(4, 1, 2ms, 0us);
        CHECK(send_one(buttons, clock) == cancelled);
        CHECK(buttons.process_click_ack(ack(ClickStatus::accepted, 900, 77, cancelled, 4, 1, 0),
                                           clock + 1) == AckResult::handled);
        CHECK(buttons.process_click_ack(ack(ClickStatus::cancelled, 900, 77, cancelled, 4, 1, 0),
                                           clock + 2) == AckResult::handled);
        CHECK(buttons.metrics().click_accepted_failed == 1);
        clock += 1'000'000;
        for (auto status : {ClickStatus::invalid, ClickStatus::stale_command}) {
            auto id = buttons.click(4, 1, 2ms, 0us);
            CHECK(send_one(buttons, clock) == id);
            CHECK(buttons.process_click_ack(ack(status, 900, 77, id, 4, 0, 0), clock + 1) ==
                  AckResult::protocol_error);
            clock += 1'000'000;
        }
        auto impossible = buttons.click(4, 1, 2ms, 0us);
        CHECK(send_one(buttons, clock) == impossible);
        CHECK(buttons.process_click_ack(ack(ClickStatus::completed, 900, 77, impossible, 4, 1, 2),
                                           clock + 1) == AckResult::protocol_error);
        auto malformed = ack(ClickStatus::accepted, 900, 77, impossible, 4, 1, 0);
        malformed[47] = 1;
        CHECK(buttons.process_click_ack(malformed, clock + 2) == AckResult::protocol_error);
        auto unsent = buttons.click(5, 1, 2ms, 0us);
        CHECK(buttons.process_click_ack(ack(ClickStatus::accepted, 900, 77, unsent, 5, 1, 0),
                                           clock + 3) == AckResult::ignored);

        // ReleaseAll cancels local/accepted click state, emits v2 op=2 before later presses,
        // and keeps the desired mask off wire until valid writer completion.
        buttons.release_all(ReleaseReason::manual, true);
        buttons.button_down(6);
        state = buttons.snapshot();
        CHECK(state.mask == 0x20 && state.wire_mask() == 0 && state.release_barrier);
        due = buttons.due_requests(clock + 4);
        CHECK(due.size() == 1 && due[0].release_all && due[0].bytes[5] == 2 && due[0].bytes[6] == 0);
        CHECK(!be32(due[0].bytes.data() + 24) && !be32(due[0].bytes.data() + 28));
        const auto release_id = due[0].id;
        const auto release_bytes = due[0].bytes;
        CHECK(buttons.due_requests(clock + 50'000'003).empty());
        due = buttons.due_requests(clock + 50'000'004);
        CHECK(due.size() == 1 && due[0].id == release_id && due[0].bytes == release_bytes &&
              buttons.metrics().release_retries == 1);
        CHECK(buttons.process_click_ack(ack(ClickStatus::completed, 900, 77, release_id, 0, 0, 0),
                                           clock + 50'000'005) == AckResult::handled);
        CHECK(!buttons.snapshot().release_barrier && buttons.snapshot().wire_mask() == 0x20);
        CHECK(buttons.due_requests(clock + 100'000'000).empty());

        // An epoch change retires the old ReleaseAll retry and creates a new-session fence.
        InjectedButtonManager epoch_release(66);
        epoch_release.observe_telemetry(upt3(900));
        epoch_release.release_all(ReleaseReason::manual, true);
        auto old_release = epoch_release.due_requests(clock + 1);
        CHECK(old_release.size() == 1 && old_release[0].release_all);
        const auto old_release_id = old_release[0].id;
        CHECK(epoch_release.process_click_ack(
                  ack(ClickStatus::busy, 900, 66, old_release_id, 0, 0, 0),
                  clock + 2) == AckResult::handled);
        CHECK(epoch_release.snapshot().release_barrier &&
              epoch_release.metrics().last_release_all_state ==
                  "release not completed; retrying same ID");
        CHECK(epoch_release.due_requests(clock + 50'000'001).empty());
        auto old_retry = epoch_release.due_requests(clock + 50'000'002);
        CHECK(old_retry.size() == 1 && old_retry[0].id == old_release_id &&
              old_retry[0].bytes == old_release[0].bytes);
        CHECK(epoch_release.observe_telemetry(upt3(901)));
        auto replacement_release = epoch_release.due_requests(clock + 100'000'000);
        CHECK(replacement_release.size() == 1 && replacement_release[0].release_all &&
              replacement_release[0].id == 1 && replacement_release[0].bytes != old_release[0].bytes);
        CHECK(epoch_release.process_click_ack(
                  ack(ClickStatus::completed, 900, 66, old_release_id, 0, 0, 0),
                  clock + 100'000'001) == AckResult::ignored);
        const auto replacement_session = epoch_release.click_client_session();
        CHECK(epoch_release.process_click_ack(
                  ack(ClickStatus::completed, 901, replacement_session,
                      replacement_release[0].id, 0, 0, 0),
                  clock + 100'000'002) == AckResult::handled);
        CHECK(epoch_release.due_requests(clock + 500'000'000).empty());

        // Different-button persistent state coexists; same-button click/hold conflicts fail locally.
        auto different = buttons.click(1, 1, 2ms, 0us);
        CHECK(different != 0 && buttons.snapshot().mask == 0x20);
        rejected = false;
        try { buttons.click(6, 1, 2ms, 0us); } catch (const std::runtime_error&) { rejected = true; }
        CHECK(rejected);
        rejected = false;
        try { auto conflict = buttons.hold(1); } catch (const std::runtime_error&) { rejected = true; }
        CHECK(rejected);

        // A confirmed epoch change drops old work, clears persistent ownership, rotates session,
        // creates a new release fence, and never resubmits an old click ID.
        const auto old_session = buttons.click_client_session();
        const auto old_id = different;
        CHECK(buttons.observe_telemetry(upt3(901)));
        CHECK(buttons.click_client_session() && buttons.click_client_session() != old_session);
        CHECK(buttons.snapshot().mask == 0 && buttons.snapshot().release_barrier);
        due = buttons.due_requests(clock + 6);
        CHECK(due.size() == 1 && due[0].release_all && due[0].id == 1);
        CHECK(be64(due[0].bytes.data() + 8) == buttons.click_client_session());
        CHECK(buttons.process_click_ack(ack(ClickStatus::completed, 900, old_session, old_id, 1, 1, 1),
                                           clock + 7) == AckResult::ignored);
        CHECK(buttons.metrics().click_epoch_resets == 1);

        // API submission is memory-only and returns promptly even when the fake Pi is stalled.
        auto release2 = due[0].id;
        auto current_session = buttons.click_client_session();
        CHECK(buttons.process_click_ack(ack(ClickStatus::completed, 901, current_session, release2, 0, 0, 0),
                                           clock + 8) == AckResult::handled);
        auto future = std::async(std::launch::async, [&] { return buttons.click(2, 1, 2ms, 0us); });
        CHECK(future.wait_for(250ms) == std::future_status::ready && future.get() != 0);
        buttons.release_all(ReleaseReason::shutdown, true);
        CHECK(buttons.snapshot().mask == 0 && buttons.metrics().pending_clicks == 0);
        CHECK(buttons.metrics().last_release_all_state == "queued locally" && wakes > 0);

        InjectedButtonManager bounded(88);
        bounded.observe_telemetry(upt3(902));
        for (int i = 0; i < 32; ++i)
            CHECK(bounded.click(i % 8 + 1, 1, 2ms, 0us) != 0);
        rejected = false;
        try { bounded.click(1, 1, 2ms, 0us); } catch (const std::runtime_error&) { rejected = true; }
        CHECK(rejected && bounded.metrics().pending_clicks == 32 &&
              bounded.metrics().click_rejected_before_acceptance == 1);

        std::cout << checks << " synthetic-button and UPC1/UPA1 v2 checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
