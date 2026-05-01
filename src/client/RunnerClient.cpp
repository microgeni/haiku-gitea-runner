// RunnerClient.cpp — Connect-RPC over libcurl implementation
//
// Protobuf encoding strategy:
//   We use a minimal hand-coded varint/length-delimited encoder so the
//   build does NOT require protoc-generated stubs.  If runner.pb.h is
//   available (#define USE_GENERATED_PROTO), we switch to that instead.
//
// Connect-RPC framing (both request and response):
//   Byte 0:   0x00 = no compression, 0x01 = gzip (we never compress)
//   Bytes 1-4: big-endian uint32 message length
//   Bytes 5+:  serialized protobuf

#include "RunnerClient.h"

#include <curl/curl.h>
#include <stdexcept>
#include <cstring>
#include <ctime>
#include <sstream>
#include <mutex>

namespace runner {

// ═══════════════════════════════════════════════════════════════════════════
// Minimal protobuf encoder / decoder
// ═══════════════════════════════════════════════════════════════════════════
//
// We only need to encode/decode the 5 request/response types.
// Rather than pulling in protobuf or generating stubs, we hand-code the
// wire-format according to https://protobuf.dev/programming-guides/encoding/
//
// Wire types:
//   0 = varint (int32, int64, bool, enum)
//   1 = 64-bit (double, fixed64)
//   2 = length-delimited (string, bytes, embedded messages, repeated fields)
//   5 = 32-bit (float, fixed32)
//
// Field tag = (field_number << 3) | wire_type

namespace proto {

// ─── Encoding helpers ─────────────────────────────────────────────────────

static void appendVarint(std::string& out, uint64_t v) {
    while (v > 0x7F) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

static void appendTag(std::string& out, uint32_t field, uint32_t wire_type) {
    appendVarint(out, (static_cast<uint64_t>(field) << 3) | wire_type);
}

static void appendString(std::string& out, uint32_t field, const std::string& s) {
    appendTag(out, field, 2); // length-delimited
    appendVarint(out, s.size());
    out.append(s);
}

static void appendInt64(std::string& out, uint32_t field, int64_t v) {
    if (v == 0) return; // proto3: skip default values
    appendTag(out, field, 0); // varint
    appendVarint(out, static_cast<uint64_t>(v));
}

static void appendBool(std::string& out, uint32_t field, bool v) {
    if (!v) return;
    appendTag(out, field, 0);
    appendVarint(out, 1);
}

static void appendEnum(std::string& out, uint32_t field, int v) {
    if (v == 0) return;
    appendTag(out, field, 0);
    appendVarint(out, static_cast<uint64_t>(v));
}

// Encode a Timestamp message (google.protobuf.Timestamp) as an embedded msg.
// seconds field = 1, nanos field = 2
static std::string encodeTimestamp(int64_t secs, int32_t nanos = 0) {
    std::string ts;
    appendInt64(ts, 1, secs);
    if (nanos) appendInt64(ts, 2, nanos);
    return ts;
}

static void appendMessage(std::string& out, uint32_t field, const std::string& msg) {
    if (msg.empty()) return;
    appendTag(out, field, 2);
    appendVarint(out, msg.size());
    out.append(msg);
}

// ─── Decoding helpers ─────────────────────────────────────────────────────

static uint64_t decodeVarint(const uint8_t*& p, const uint8_t* end) {
    uint64_t result = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        result |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return result;
}

static std::string decodeString(const uint8_t*& p, const uint8_t* end) {
    uint64_t len = decodeVarint(p, end);
    if (p + len > end) throw std::runtime_error("Proto decode: string overrun");
    std::string s(reinterpret_cast<const char*>(p), len);
    p += len;
    return s;
}

static void skipField(uint32_t wire_type, const uint8_t*& p, const uint8_t* end) {
    switch (wire_type) {
        case 0: decodeVarint(p, end); break;
        case 1: p += 8; break;
        case 2: { uint64_t len = decodeVarint(p, end); p += len; break; }
        case 5: p += 4; break;
        default: throw std::runtime_error("Proto decode: unknown wire type");
    }
}

// ─── Specific message encoders ────────────────────────────────────────────

// PingRequest { string data = 1; }
static std::string encodePingRequest(const std::string& data) {
    std::string out;
    appendString(out, 1, data);
    return out;
}

// PingResponse { string data = 1; }
static std::string decodePingResponse_data(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    std::string data;
    while (p < end) {
        uint64_t tag = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        if (field == 1 && wire_type == 2) {
            data = decodeString(p, end);
        } else {
            skipField(wire_type, p, end);
        }
    }
    return data;
}

// RegisterRequest
static std::string encodeRegisterRequest(
    const std::string& token,
    const std::string& name,
    const std::vector<std::string>& labels,
    const std::string& os,
    const std::string& arch,
    const std::string& version)
{
    std::string out;
    appendString(out, 1, token);
    appendString(out, 2, name);
    for (auto& l : labels) appendString(out, 3, l);
    appendString(out, 4, os);
    appendString(out, 5, arch);
    appendString(out, 6, version);
    return out;
}

// RegisterResponse { string runner_token=1; string uuid=2; string name=3; repeated string labels=4; }
static RegisterResult decodeRegisterResponse(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    RegisterResult r;
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        switch (field) {
            case 1: r.runner_token = decodeString(p, end); break;
            case 2: r.uuid         = decodeString(p, end); break;
            case 3: r.name         = decodeString(p, end); break;
            case 4: r.labels.push_back(decodeString(p, end)); break;
            default: skipField(wire_type, p, end); break;
        }
    }
    return r;
}

// FetchTaskRequest { repeated string labels=1; int64 tasks_version=2; }
static std::string encodeFetchTaskRequest(
    const std::vector<std::string>& labels,
    int64_t tasks_version)
{
    std::string out;
    for (auto& l : labels) appendString(out, 1, l);
    appendInt64(out, 2, tasks_version);
    return out;
}

// Decode map entry (key=1 string, value=2 string) repeated pattern
static std::pair<std::string,std::string> decodeMapEntry(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    std::string key, val;
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        if (field == 1 && wire_type == 2)      key = decodeString(p, end);
        else if (field == 2 && wire_type == 2) val = decodeString(p, end);
        else skipField(wire_type, p, end);
    }
    return {key, val};
}

static TaskDto decodeTask(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    TaskDto t;
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        switch (field) {
            case 1: t.id = static_cast<int64_t>(decodeVarint(p, end)); break;
            case 2: t.workflow_payload = decodeString(p, end); break;
            case 3: t.context.push_back(decodeMapEntry(decodeString(p, end))); break;
            case 4: t.secrets.push_back(decodeMapEntry(decodeString(p, end))); break;
            case 5: t.vars.push_back(decodeMapEntry(decodeString(p, end))); break;
            case 6: t.gitea_runtime_token = decodeString(p, end); break;
            case 8: t.timeout = static_cast<int64_t>(decodeVarint(p, end)); break;
            case 9: t.machine = decodeString(p, end); break;
            default: skipField(wire_type, p, end); break;
        }
    }
    return t;
}

static FetchTaskResult decodeFetchTaskResponse(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    FetchTaskResult r;
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        switch (field) {
            case 1: { // task (embedded message)
                std::string sub = decodeString(p, end);
                TaskDto t = decodeTask(sub);
                if (t.id != 0) r.task = t;
                break;
            }
            case 2: r.tasks_version = static_cast<int64_t>(decodeVarint(p, end)); break;
            default: skipField(wire_type, p, end); break;
        }
    }
    return r;
}

// UpdateTaskRequest
static std::string encodeUpdateTaskRequest(
    int64_t task_id,
    int state,
    const std::vector<StepStateDto>& steps,
    int64_t started_at_s,
    int64_t stopped_at_s)
{
    std::string out;
    appendInt64(out, 1, task_id);
    appendEnum(out, 2, state);

    for (auto& s : steps) {
        std::string step;
        appendInt64(step, 1, s.id);
        appendEnum(step, 2, s.result);
        if (s.started_at_s) appendMessage(step, 3, encodeTimestamp(s.started_at_s));
        if (s.stopped_at_s) appendMessage(step, 4, encodeTimestamp(s.stopped_at_s));
        appendInt64(step, 5, s.log_index);
        appendInt64(step, 6, s.log_length);
        appendMessage(out, 3, step);
    }

    if (started_at_s) appendMessage(out, 4, encodeTimestamp(started_at_s));
    if (stopped_at_s) appendMessage(out, 5, encodeTimestamp(stopped_at_s));
    return out;
}

static UpdateTaskResult decodeUpdateTaskResponse(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    UpdateTaskResult r;
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        switch (field) {
            case 1: r.task_id = static_cast<int64_t>(decodeVarint(p, end)); break;
            case 2: r.state   = decodeString(p, end); break;
            default: skipField(wire_type, p, end); break;
        }
    }
    return r;
}

// UpdateLogRequest { int64 task_id=1; int64 index=2; repeated LogRow rows=3; bool no_more=4; }
// LogRow { Timestamp time=1; string content=2; }
static std::string encodeUpdateLogRequest(
    int64_t task_id,
    int64_t index,
    const std::vector<LogRowDto>& rows,
    bool no_more)
{
    std::string out;
    appendInt64(out, 1, task_id);
    appendInt64(out, 2, index);
    for (auto& row : rows) {
        std::string r;
        std::string ts = encodeTimestamp(row.time_s, row.time_ns);
        appendMessage(r, 1, ts);
        appendString(r, 2, row.content);
        appendMessage(out, 3, r);
    }
    appendBool(out, 4, no_more);
    return out;
}

static UpdateLogResult decodeUpdateLogResponse(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    UpdateLogResult r;
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        if (field == 1 && wire_type == 0) {
            r.ack_index = static_cast<int64_t>(decodeVarint(p, end));
        } else {
            skipField(wire_type, p, end);
        }
    }
    return r;
}

} // namespace proto

// ═══════════════════════════════════════════════════════════════════════════
// RunnerClient::Impl
// ═══════════════════════════════════════════════════════════════════════════

static size_t curlWrite(char* ptr, size_t sz, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, sz * nmemb);
    return sz * nmemb;
}

struct RunnerClient::Impl {
    std::string      base_url;
    std::string      runner_token;
    bool             insecure;
    mutable std::mutex token_mutex;   // protects runner_token reads/writes

    // NOTE: We do NOT keep a shared CURL* here.  Each doRPC() call creates
    // its own CURL handle for its own duration — this makes RunnerClient
    // fully thread-safe for concurrent use by multiple TaskExecutor threads.
    // curl_global_init() must be called once from main() before any threads.

    Impl(std::string url, bool ins)
        : base_url(std::move(url)), insecure(ins)
    {}
};

RunnerClient::RunnerClient(std::string base_url, bool insecure)
    : impl_(std::make_unique<Impl>(std::move(base_url), insecure))
{}

RunnerClient::~RunnerClient() = default;

void RunnerClient::setRunnerToken(std::string token) {
    std::lock_guard<std::mutex> lock(impl_->token_mutex);
    impl_->runner_token = std::move(token);
}

// ─── Connect-RPC framing ──────────────────────────────────────────────────

std::string RunnerClient::encodeRequest(const std::string& proto_bytes) {
    uint32_t len = static_cast<uint32_t>(proto_bytes.size());
    std::string out(5, '\0');
    out[0] = 0x00; // no compression
    out[1] = (len >> 24) & 0xFF;
    out[2] = (len >> 16) & 0xFF;
    out[3] = (len >>  8) & 0xFF;
    out[4] = (len      ) & 0xFF;
    out.append(proto_bytes);
    return out;
}

std::string RunnerClient::decodeResponse(const std::string& raw) {
    if (raw.size() < 5) {
        throw std::runtime_error("Connect-RPC response too short (" +
                                 std::to_string(raw.size()) + " bytes)");
    }
    // Byte 0: flags (0x00 = no compression, 0x01 = end-stream for streaming)
    // uint8_t flags = static_cast<uint8_t>(raw[0]);
    uint32_t len = (static_cast<uint8_t>(raw[1]) << 24)
                 | (static_cast<uint8_t>(raw[2]) << 16)
                 | (static_cast<uint8_t>(raw[3]) <<  8)
                 | (static_cast<uint8_t>(raw[4])      );
    if (raw.size() < 5 + len) {
        throw std::runtime_error("Connect-RPC response body truncated");
    }
    return raw.substr(5, len);
}

// ─── Core HTTP dispatch ───────────────────────────────────────────────────

std::string RunnerClient::doRPC(const std::string& method,
                                 const std::string& request_proto,
                                 int timeout_s)
{
    return doRPC_url("runner.v1.RunnerService/" + method, request_proto, timeout_s);
}

std::string RunnerClient::doRPC_url(const std::string& service_method,
                                     const std::string& request_proto,
                                     int timeout_s)
{
    // Create a fresh CURL handle for each RPC call.
    // This makes RunnerClient thread-safe: multiple threads can call
    // different RPCs concurrently without sharing any libcurl state.
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init() failed in doRPC");

    // Read shared fields under the token lock
    std::string base_url, runner_token;
    bool insecure;
    {
        std::lock_guard<std::mutex> lock(impl_->token_mutex);
        base_url     = impl_->base_url;
        runner_token = impl_->runner_token;
        insecure     = impl_->insecure;
    }

    std::string response_body;
    std::string url = base_url + "/api/actions/" + service_method;

    std::string framed = encodeRequest(request_proto);

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/proto");
    hdrs = curl_slist_append(hdrs, "Accept: application/proto");
    if (!runner_token.empty()) {
        std::string tok = "x-runner-token: " + runner_token;
        hdrs = curl_slist_append(hdrs, tok.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     framed.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)framed.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        (long)timeout_s);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // HTTP/1.1 — avoid HTTP/2 multiplexing complexity for simplicity
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,   CURL_HTTP_VERSION_1_1);
    if (insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        // Timeout is expected for FetchTask long-poll with no tasks
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return {}; // empty = no response (treat as empty FetchTaskResponse)
        }
        throw std::runtime_error(
            std::string("RPC ") + service_method + " failed: " + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    // Connect-RPC error responses use 4xx/5xx with JSON error body
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error(
            "RPC " + service_method + " HTTP " + std::to_string(http_code)
            + ": " + response_body);
    }

    if (response_body.empty()) return {};

    return decodeResponse(response_body);
}

// ─── Public RPC methods ───────────────────────────────────────────────────

PingResult RunnerClient::ping(const std::string& data) {
    // Ping lives in a separate service: ping.v1.PingService/Ping
    // We call doRPC_url() directly with the full path.
    auto req  = proto::encodePingRequest(data);
    auto resp = doRPC_url("ping.v1.PingService/Ping", req, 10);
    PingResult r;
    r.data = resp.empty() ? "" : proto::decodePingResponse_data(resp);
    return r;
}

RegisterResult RunnerClient::registerRunner(
    const std::string& reg_token,
    const std::string& name,
    const std::vector<std::string>& labels,
    const std::string& os,
    const std::string& arch,
    const std::string& version)
{
    auto req  = proto::encodeRegisterRequest(reg_token, name, labels, os, arch, version);
    auto resp = doRPC("Register", req, 30);
    if (resp.empty()) throw std::runtime_error("Register RPC returned empty response");
    return proto::decodeRegisterResponse(resp);
}

FetchTaskResult RunnerClient::fetchTask(
    const std::vector<std::string>& labels,
    int64_t tasks_version,
    int timeout_s)
{
    auto req  = proto::encodeFetchTaskRequest(labels, tasks_version);
    auto resp = doRPC("FetchTask", req, timeout_s);
    if (resp.empty()) return FetchTaskResult{};
    return proto::decodeFetchTaskResponse(resp);
}

UpdateTaskResult RunnerClient::updateTask(
    int64_t task_id,
    int state,
    const std::vector<StepStateDto>& steps,
    int64_t started_at_s,
    int64_t stopped_at_s)
{
    auto req  = proto::encodeUpdateTaskRequest(task_id, state, steps, started_at_s, stopped_at_s);
    auto resp = doRPC("UpdateTask", req, 30);
    if (resp.empty()) return UpdateTaskResult{task_id, ""};
    return proto::decodeUpdateTaskResponse(resp);
}

UpdateLogResult RunnerClient::updateLog(
    int64_t task_id,
    int64_t index,
    const std::vector<LogRowDto>& rows,
    bool no_more)
{
    auto req  = proto::encodeUpdateLogRequest(task_id, index, rows, no_more);
    auto resp = doRPC("UpdateLog", req, 30);
    if (resp.empty()) return UpdateLogResult{index + static_cast<int64_t>(rows.size())};
    return proto::decodeUpdateLogResponse(resp);
}

} // namespace runner
