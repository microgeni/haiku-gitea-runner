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

#include "../util/Logger.h"
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
//
// Field numbers verified against:
// https://gitea.com/gitea/actions-proto-go/raw/branch/main/runner/v1/messages.pb.go

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
        uint64_t tag       = decodeVarint(p, end);
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

// RegisterRequest {
//   string name         = 1;
//   string token        = 2;
//   repeated string agent_labels  = 3;  (deprecated)
//   repeated string custom_labels = 4;  (deprecated)
//   string version      = 5;
//   repeated string labels = 6;
//   bool   ephemeral    = 7;
// }
static std::string encodeRegisterRequest(
    const std::string& token,
    const std::string& name,
    const std::vector<std::string>& labels,
    const std::string& version)
{
    std::string out;
    appendString(out, 1, name);
    appendString(out, 2, token);
    // field 3 (agent_labels) and field 4 (custom_labels) are deprecated — skip
    appendString(out, 5, version);
    for (auto& l : labels) appendString(out, 6, l);
    // ephemeral=false (field 7) — proto3 default, skip
    return out;
}

// Runner {
//   int64  id     = 1;
//   string uuid   = 2;
//   string token  = 3;
//   string name   = 4;
//   ...
//   repeated string labels = 9;
// }
static RegisterResult decodeRunner(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    RegisterResult r;
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        switch (field) {
            case 2: r.uuid          = decodeString(p, end); break;
            case 3: r.runner_token  = decodeString(p, end); break;
            case 4: r.name          = decodeString(p, end); break;
            case 9: r.labels.push_back(decodeString(p, end)); break;
            default: skipField(wire_type, p, end); break;
        }
    }
    return r;
}

// RegisterResponse { Runner runner = 1; }
static RegisterResult decodeRegisterResponse(const std::string& bytes) {
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        if (field == 1 && wire_type == 2) {
            return decodeRunner(decodeString(p, end));
        }
        skipField(wire_type, p, end);
    }
    return RegisterResult{};
}

// DeclareRequest { string version = 1; repeated string labels = 2; }
static std::string encodeDeclareRequest(
    const std::string& version,
    const std::vector<std::string>& labels)
{
    std::string out;
    appendString(out, 1, version);
    for (auto& l : labels) appendString(out, 2, l);
    return out;
}

// DeclareResponse { Runner runner = 1; }
// (same layout as RegisterResponse — reuse decodeRegisterResponse)

// FetchTaskRequest { int64 tasks_version = 1; }
// NOTE: no labels field — labels are stored on the registered runner in Gitea.
static std::string encodeFetchTaskRequest(int64_t tasks_version)
{
    std::string out;
    appendInt64(out, 1, tasks_version);
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

// Decode google.protobuf.Struct into a flat key→value map.
// Struct { map<string, Value> fields = 1; }
// Value is a oneof; we only care about string_value (field 3).
static std::vector<std::pair<std::string,std::string>> decodeStruct(const std::string& bytes) {
    std::vector<std::pair<std::string,std::string>> result;
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        if (field == 1 && wire_type == 2) {
            // map entry: key=1 (string), value=2 (Value message)
            std::string entry_bytes = decodeString(p, end);
            const uint8_t* ep   = reinterpret_cast<const uint8_t*>(entry_bytes.data());
            const uint8_t* eend = ep + entry_bytes.size();
            std::string map_key, map_val;
            while (ep < eend) {
                uint64_t etag      = decodeVarint(ep, eend);
                uint32_t ef        = etag >> 3;
                uint32_t ewt       = etag & 0x7;
                if (ef == 1 && ewt == 2) {
                    map_key = decodeString(ep, eend); // map key
                } else if (ef == 2 && ewt == 2) {
                    // Value message — parse for string_value (field 3)
                    std::string val_bytes = decodeString(ep, eend);
                    const uint8_t* vp   = reinterpret_cast<const uint8_t*>(val_bytes.data());
                    const uint8_t* vend = vp + val_bytes.size();
                    while (vp < vend) {
                        uint64_t vtag = decodeVarint(vp, vend);
                        uint32_t vf   = vtag >> 3;
                        uint32_t vwt  = vtag & 0x7;
                        if (vf == 3 && vwt == 2) {
                            map_val = decodeString(vp, vend); // string_value
                        } else {
                            skipField(vwt, vp, vend);
                        }
                    }
                } else {
                    skipField(ewt, ep, eend);
                }
            }
            if (!map_key.empty()) result.push_back({map_key, map_val});
        } else {
            skipField(wire_type, p, end);
        }
    }
    return result;
}

// Task {
//   int64  id               = 1;
//   bytes  workflow_payload = 2;
//   Struct context          = 3;   (google.protobuf.Struct — github context)
//   map<string,string> secrets = 4;
//   string machine          = 5;   (deprecated/unused)
//   map<string,TaskNeed> needs = 6;
//   map<string,string> vars = 7;
// }
// TaskNeed { map<string,string> outputs=1; Result result=2; }
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
            case 3: { // google.protobuf.Struct — github context
                auto kv = decodeStruct(decodeString(p, end));
                t.context.insert(t.context.end(), kv.begin(), kv.end());
                break;
            }
            case 4: t.secrets.push_back(decodeMapEntry(decodeString(p, end))); break;
            case 5: t.machine = decodeString(p, end); break;
            case 6: { // needs: map<string, TaskNeed>
                // map entry: key=1 (string), value=2 (TaskNeed message)
                std::string entry = decodeString(p, end);
                const uint8_t* ep   = reinterpret_cast<const uint8_t*>(entry.data());
                const uint8_t* eend = ep + entry.size();
                std::string need_job;
                NeedsContextEntry need_entry;
                while (ep < eend) {
                    uint64_t etag = decodeVarint(ep, eend);
                    uint32_t ef   = etag >> 3;
                    uint32_t ewt  = etag & 0x7;
                    if (ef == 1 && ewt == 2) {
                        need_job = decodeString(ep, eend);
                    } else if (ef == 2 && ewt == 2) {
                        // TaskNeed message: outputs=1 (map), result=2 (enum)
                        std::string need_bytes = decodeString(ep, eend);
                        const uint8_t* np   = reinterpret_cast<const uint8_t*>(need_bytes.data());
                        const uint8_t* nend = np + need_bytes.size();
                        while (np < nend) {
                            uint64_t ntag = decodeVarint(np, nend);
                            uint32_t nf   = ntag >> 3;
                            uint32_t nwt  = ntag & 0x7;
                            if (nf == 1 && nwt == 2) {
                                auto kv = decodeMapEntry(decodeString(np, nend));
                                need_entry.outputs.push_back(kv);
                            } else if (nf == 2 && nwt == 0) {
                                need_entry.result = static_cast<int>(decodeVarint(np, nend));
                            } else {
                                skipField(nwt, np, nend);
                            }
                        }
                    } else {
                        skipField(ewt, ep, eend);
                    }
                }
                if (!need_job.empty()) {
                    t.needs_context.push_back({need_job, need_entry});
                }
                break;
            }
            case 7: t.vars.push_back(decodeMapEntry(decodeString(p, end))); break;
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

// UpdateTaskRequest {
//   TaskState state   = 1;
//   map<string,string> outputs = 2;
// }
// TaskState {
//   int64  id         = 1;
//   Result result     = 2;
//   Timestamp started_at = 3;
//   Timestamp stopped_at = 4;
//   repeated StepState steps = 5;
// }
// StepState {
//   int64  id         = 1;
//   Result result     = 2;
//   Timestamp started_at = 3;
//   Timestamp stopped_at = 4;
//   int64  log_index  = 5;
//   int64  log_length = 6;
// }
static std::string encodeUpdateTaskRequest(
    int64_t task_id,
    int state,
    const std::vector<StepStateDto>& steps,
    int64_t started_at_s,
    int64_t stopped_at_s,
    const std::vector<std::pair<std::string,std::string>>& outputs = {})
{
    // Build TaskState embedded message
    std::string task_state;
    appendInt64(task_state, 1, task_id);
    appendEnum(task_state, 2, state);
    if (started_at_s) appendMessage(task_state, 3, encodeTimestamp(started_at_s));
    if (stopped_at_s) appendMessage(task_state, 4, encodeTimestamp(stopped_at_s));

    for (auto& s : steps) {
        std::string step;
        appendInt64(step, 1, s.id);
        appendEnum(step, 2, s.result);
        if (s.started_at_s) appendMessage(step, 3, encodeTimestamp(s.started_at_s));
        if (s.stopped_at_s) appendMessage(step, 4, encodeTimestamp(s.stopped_at_s));
        appendInt64(step, 5, s.log_index);
        appendInt64(step, 6, s.log_length);
        appendMessage(task_state, 5, step);
    }

    std::string out;
    appendMessage(out, 1, task_state);

    // outputs map (field 2)
    for (auto& kv : outputs) {
        std::string entry;
        appendString(entry, 1, kv.first);
        appendString(entry, 2, kv.second);
        appendMessage(out, 2, entry);
    }

    return out;
}

static UpdateTaskResult decodeUpdateTaskResponse(const std::string& bytes) {
    // UpdateTaskResponse { TaskState state=1; repeated string sent_outputs=2; }
    // We just need to confirm success; extract task_id from TaskState.
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint8_t* end = p + bytes.size();
    UpdateTaskResult r;
    while (p < end) {
        uint64_t tag       = decodeVarint(p, end);
        uint32_t field     = tag >> 3;
        uint32_t wire_type = tag & 0x7;
        if (field == 1 && wire_type == 2) {
            // TaskState — extract id (field 1) and result (field 2)
            std::string ts_bytes = decodeString(p, end);
            const uint8_t* tp   = reinterpret_cast<const uint8_t*>(ts_bytes.data());
            const uint8_t* tend = tp + ts_bytes.size();
            while (tp < tend) {
                uint64_t ttag = decodeVarint(tp, tend);
                uint32_t tf   = ttag >> 3;
                uint32_t twt  = ttag & 0x7;
                if (tf == 1 && twt == 0) {
                    r.task_id = static_cast<int64_t>(decodeVarint(tp, tend));
                } else if (tf == 2 && twt == 0) {
                    int res = static_cast<int>(decodeVarint(tp, tend));
                    // Result enum: 0=unspecified,1=success,2=failure,3=cancelled,4=skipped
                    r.state = (res == 1) ? "success" :
                              (res == 2) ? "failure" :
                              (res == 3) ? "cancelled" :
                              (res == 4) ? "skipped" : "unknown";
                } else {
                    skipField(twt, tp, tend);
                }
            }
        } else {
            skipField(wire_type, p, end);
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
    std::string      runner_uuid;
    bool             insecure;
    mutable std::mutex token_mutex;   // protects runner_token and runner_uuid

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

void RunnerClient::setRunnerUUID(std::string uuid) {
    std::lock_guard<std::mutex> lock(impl_->token_mutex);
    impl_->runner_uuid = std::move(uuid);
}

// ─── Connect-RPC framing ──────────────────────────────────────────────────

std::string RunnerClient::encodeRequest(const std::string& proto_bytes) {
    // Connect-RPC UNARY: request body is just the raw protobuf bytes.
    // The 5-byte envelope (flag + 4-byte length) is only for streaming RPCs.
    return proto_bytes;
}

std::string RunnerClient::decodeResponse(const std::string& raw) {
    // Connect-RPC UNARY: response body is just the raw protobuf bytes.
    return raw;
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
    std::string base_url, runner_token, runner_uuid;
    bool insecure;
    {
        std::lock_guard<std::mutex> lock(impl_->token_mutex);
        base_url     = impl_->base_url;
        runner_token = impl_->runner_token;
        runner_uuid  = impl_->runner_uuid;
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
    if (!runner_uuid.empty()) {
        std::string uid = "x-runner-uuid: " + runner_uuid;
        hdrs = curl_slist_append(hdrs, uid.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     framed.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)framed.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        (long)timeout_s);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // NOSIGNAL: prevent libcurl from sending signals (SIGALRM, SIGPIPE, etc.)
    // in multithreaded daemons.  Required on Haiku to avoid random signal
    // delivery to arbitrary threads.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
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
    const std::string& /*os*/,
    const std::string& /*arch*/,
    const std::string& version)
{
    auto req  = proto::encodeRegisterRequest(reg_token, name, labels, version);
    auto resp = doRPC("Register", req, 30);
    if (resp.empty()) throw std::runtime_error("Register RPC returned empty response");
    return proto::decodeRegisterResponse(resp);
}

RegisterResult RunnerClient::declare(
    const std::vector<std::string>& labels,
    const std::string& version)
{
    auto req  = proto::encodeDeclareRequest(version, labels);
    auto resp = doRPC("Declare", req, 30);
    if (resp.empty()) throw std::runtime_error("Declare RPC returned empty response");
    // DeclareResponse { Runner runner=1; } — same layout as RegisterResponse
    return proto::decodeRegisterResponse(resp);
}

FetchTaskResult RunnerClient::fetchTask(
    const std::vector<std::string>& /*labels*/,
    int64_t tasks_version,
    int timeout_s)
{
    // FetchTaskRequest only has tasks_version; labels are stored on the
    // registered runner in Gitea and are not repeated per-request.
    auto req  = proto::encodeFetchTaskRequest(tasks_version);
    auto resp = doRPC("FetchTask", req, timeout_s);
    if (resp.empty()) return FetchTaskResult{};
    return proto::decodeFetchTaskResponse(resp);
}

UpdateTaskResult RunnerClient::updateTask(
    int64_t task_id,
    int state,
    const std::vector<StepStateDto>& steps,
    int64_t started_at_s,
    int64_t stopped_at_s,
    const std::vector<std::pair<std::string,std::string>>& outputs)
{
    auto req  = proto::encodeUpdateTaskRequest(task_id, state, steps, started_at_s, stopped_at_s, outputs);
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
