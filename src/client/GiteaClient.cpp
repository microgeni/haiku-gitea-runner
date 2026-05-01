// GiteaClient.cpp — HTTP REST implementation using libcurl
#include "GiteaClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sstream>
#include <vector>
#include <cstring>

namespace runner {

using json = nlohmann::json;

// ─── libcurl write callback ────────────────────────────────────────────────

static size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// ─── GiteaClient::Impl ────────────────────────────────────────────────────

struct GiteaClient::Impl {
    std::string base_url;
    std::string admin_token;
    bool        insecure;
    CURL*       curl;

    Impl(std::string url, std::string token, bool ins)
        : base_url(std::move(url))
        , admin_token(std::move(token))
        , insecure(ins)
        , curl(curl_easy_init())
    {
        if (!curl) {
            throw std::runtime_error("curl_easy_init() failed");
        }
    }

    ~Impl() {
        if (curl) curl_easy_cleanup(curl);
    }
};

// ─── Constructor/destructor ───────────────────────────────────────────────

GiteaClient::GiteaClient(std::string base_url, std::string admin_token, bool insecure)
    : impl_(new Impl(std::move(base_url), std::move(admin_token), insecure))
{}

GiteaClient::~GiteaClient() {
    delete impl_;
}

// ─── Internal helpers ─────────────────────────────────────────────────────

std::string GiteaClient::doPost(const std::string& path,
                                 const std::string& body,
                                 const std::map<std::string, std::string>& headers)
{
    CURL* curl = impl_->curl;
    std::string response;
    std::string url = impl_->base_url + path;

    // Build header list
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (!impl_->admin_token.empty()) {
        std::string auth = "Authorization: token " + impl_->admin_token;
        hdrs = curl_slist_append(hdrs, auth.c_str());
    }
    for (auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        hdrs = curl_slist_append(hdrs, h.c_str());
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (impl_->insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("HTTP POST failed: ") + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error(
            "HTTP POST " + url + " returned " + std::to_string(http_code)
            + ": " + response);
    }

    return response;
}

std::string GiteaClient::doGet(const std::string& path,
                                const std::map<std::string, std::string>& headers)
{
    CURL* curl = impl_->curl;
    std::string response;
    std::string url = impl_->base_url + path;

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (!impl_->admin_token.empty()) {
        std::string auth = "Authorization: token " + impl_->admin_token;
        hdrs = curl_slist_append(hdrs, auth.c_str());
    }
    for (auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        hdrs = curl_slist_append(hdrs, h.c_str());
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (impl_->insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("HTTP GET failed: ") + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error(
            "HTTP GET " + url + " returned " + std::to_string(http_code)
            + ": " + response);
    }

    return response;
}

// ─── Public API ───────────────────────────────────────────────────────────

RegistrationTokenResponse GiteaClient::getRegistrationToken() {
    // POST /api/v1/runners/registration-token  (empty body)
    std::string resp = doPost("/api/v1/runners/registration-token", "{}", {});

    json j;
    try {
        j = json::parse(resp);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("Failed to parse registration token response: ") + e.what());
    }

    RegistrationTokenResponse result;
    result.token = j.value("token", "");
    if (result.token.empty()) {
        throw std::runtime_error("Registration token response missing 'token' field: " + resp);
    }
    return result;
}

bool GiteaClient::ping() {
    try {
        doGet("/api/swagger", {});
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace runner
