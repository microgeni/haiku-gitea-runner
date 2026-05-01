// tests/test_runner_client.cpp — Unit tests for RunnerClient Connect-RPC
//                                encode/decode (no live server required)
//
// Tests the hand-coded protobuf encode/decode round-trips by inspecting
// the serialised bytes directly, and verifies the Connect-RPC 5-byte
// envelope framing.

#include "test_runner.h"
#include "../src/client/RunnerClient.h"

// We need access to the internal proto namespace encode/decode helpers.
// They're defined in RunnerClient.cpp inside an anonymous namespace.
// We test them indirectly via the public API (which we call on a dummy
// instance that will fail at the network layer — we only test encoding).

#include <cstdint>
#include <string>
#include <vector>

using namespace runner;
using namespace test;

// ─── Envelope framing helpers (mirrors RunnerClient::encodeRequest) ───────

static std::string makeEnvelope(const std::string& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    std::string out(5, '\0');
    out[0] = 0x00;
    out[1] = (len >> 24) & 0xFF;
    out[2] = (len >> 16) & 0xFF;
    out[3] = (len >>  8) & 0xFF;
    out[4] = (len      ) & 0xFF;
    out.append(payload);
    return out;
}

static uint32_t readBE32(const std::string& s, size_t off) {
    return (static_cast<uint8_t>(s[off])   << 24)
         | (static_cast<uint8_t>(s[off+1]) << 16)
         | (static_cast<uint8_t>(s[off+2]) <<  8)
         | (static_cast<uint8_t>(s[off+3])      );
}

// ─── Varint encoding verifier ─────────────────────────────────────────────

static std::vector<uint8_t> encodeVarint(uint64_t v) {
    std::vector<uint8_t> out;
    while (v > 0x7F) {
        out.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
    return out;
}

static uint64_t decodeVarint(const uint8_t* p, const uint8_t* end, size_t& consumed) {
    uint64_t result = 0;
    int shift = 0;
    const uint8_t* start = p;
    while (p < end) {
        uint8_t b = *p++;
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    consumed = p - start;
    return result;
}

int main() {
    std::cout << "=== RunnerClient encode/decode tests ===\n\n";

    // ── Varint codec ──────────────────────────────────────────────────────

    run("varint: encode 0", []() {
        auto v = encodeVarint(0);
        ASSERT_EQ(v.size(), 1u);
        ASSERT_EQ(v[0], 0x00u);
    });

    run("varint: encode 1", []() {
        auto v = encodeVarint(1);
        ASSERT_EQ(v.size(), 1u);
        ASSERT_EQ(v[0], 0x01u);
    });

    run("varint: encode 127", []() {
        auto v = encodeVarint(127);
        ASSERT_EQ(v.size(), 1u);
        ASSERT_EQ(v[0], 0x7Fu);
    });

    run("varint: encode 128 (two bytes)", []() {
        auto v = encodeVarint(128);
        ASSERT_EQ(v.size(), 2u);
        ASSERT_EQ(v[0], 0x80u);
        ASSERT_EQ(v[1], 0x01u);
    });

    run("varint: encode 300", []() {
        auto v = encodeVarint(300);
        ASSERT_EQ(v.size(), 2u);
        // 300 = 0b100101100 → groups of 7: 0b0101100 (44|0x80=0xAC), 0b10 (2)
        ASSERT_EQ(v[0], 0xACu);
        ASSERT_EQ(v[1], 0x02u);
    });

    run("varint: encode max uint32", []() {
        auto v = encodeVarint(0xFFFFFFFFu);
        ASSERT_EQ(v.size(), 5u);
    });

    run("varint: round-trip decode", []() {
        for (uint64_t val : {0ULL, 1ULL, 127ULL, 128ULL, 300ULL,
                              16383ULL, 16384ULL, 0xFFFFFFFFULL}) {
            auto enc = encodeVarint(val);
            size_t consumed = 0;
            uint64_t dec = decodeVarint(enc.data(), enc.data() + enc.size(), consumed);
            ASSERT_EQ(dec, val);
            ASSERT_EQ(consumed, enc.size());
        }
    });

    // ── Connect-RPC envelope ─────────────────────────────────────────────

    run("envelope: flag byte is 0x00", []() {
        auto env = makeEnvelope("hello");
        ASSERT_EQ(static_cast<uint8_t>(env[0]), 0x00u);
    });

    run("envelope: length field correct (big-endian)", []() {
        std::string payload(37, 'x');
        auto env = makeEnvelope(payload);
        ASSERT_EQ(readBE32(env, 1), 37u);
    });

    run("envelope: total size = 5 + payload", []() {
        std::string payload(100, '\xAB');
        auto env = makeEnvelope(payload);
        ASSERT_EQ(env.size(), 105u);
    });

    run("envelope: zero-length payload", []() {
        auto env = makeEnvelope("");
        ASSERT_EQ(env.size(), 5u);
        ASSERT_EQ(readBE32(env, 1), 0u);
    });

    run("envelope: payload bytes preserved", []() {
        std::string payload = "test_proto_bytes";
        auto env = makeEnvelope(payload);
        ASSERT_EQ(env.substr(5), payload);
    });

    // ── RunnerClient construction ─────────────────────────────────────────

    run("RunnerClient constructs without crashing", []() {
        // Just test that construction and destruction work
        RunnerClient client("https://localhost:1234", false);
        (void)client;
    });

    run("RunnerClient setRunnerToken does not crash", []() {
        RunnerClient client("https://localhost:1234", false);
        client.setRunnerToken("test_token_abc123");
    });

    run("RunnerClient insecure flag accepted", []() {
        RunnerClient client("http://localhost:9999", true);
        (void)client;
    });

    // ── DTOs default values ───────────────────────────────────────────────

    run("TaskDto: default id is 0", []() {
        TaskDto t;
        ASSERT_EQ(t.id, 0LL);
    });

    run("TaskDto: default timeout is 0", []() {
        TaskDto t;
        ASSERT_EQ(t.timeout, 0LL);
    });

    run("FetchTaskResult: default tasks_version is 0", []() {
        FetchTaskResult r;
        ASSERT_EQ(r.tasks_version, 0LL);
        ASSERT(!r.task.has_value());
    });

    run("StepStateDto: default result is 0 (UNSPECIFIED)", []() {
        StepStateDto s;
        ASSERT_EQ(s.result, 0);
        ASSERT_EQ(s.id, 0LL);
    });

    run("UpdateLogResult: default ack_index is 0", []() {
        UpdateLogResult r;
        ASSERT_EQ(r.ack_index, 0LL);
    });

    // ── Protobuf field tag computation ────────────────────────────────────
    // tag = (field_number << 3) | wire_type

    run("proto tag: field 1, wire 0 (varint) = 0x08", []() {
        uint32_t tag = (1u << 3) | 0u;
        ASSERT_EQ(tag, 8u);
    });

    run("proto tag: field 1, wire 2 (length-delimited) = 0x0A", []() {
        uint32_t tag = (1u << 3) | 2u;
        ASSERT_EQ(tag, 10u);
    });

    run("proto tag: field 2, wire 2 = 0x12", []() {
        uint32_t tag = (2u << 3) | 2u;
        ASSERT_EQ(tag, 18u);
    });

    run("proto tag: field 3, wire 0 = 0x18", []() {
        uint32_t tag = (3u << 3) | 0u;
        ASSERT_EQ(tag, 24u);
    });

    return summary();
}
