#pragma once
// GiteaClient.h — HTTP REST client for Gitea API
//
// Handles the registration REST call and any other direct HTTP interactions
// with the Gitea server (not the gRPC/Connect-RPC runner service).
//
// Implementation uses libcurl (synchronous, single-threaded at call site).

#include <string>
#include <map>

namespace runner {

/// Registration token exchange result.
struct RegistrationTokenResponse {
    std::string token;  ///< one-time registration token
};

/// Thin wrapper around libcurl for Gitea REST API calls.
class GiteaClient {
public:
    /// @param base_url  e.g. "https://gitea.example.com"
    /// @param admin_token  Gitea admin or user API token (for pre-registration REST calls)
    /// @param insecure  skip TLS cert verification (dev only)
    GiteaClient(std::string base_url,
                std::string admin_token = "",
                bool        insecure    = false);

    ~GiteaClient();

    // Non-copyable (owns CURL handle)
    GiteaClient(const GiteaClient&)            = delete;
    GiteaClient& operator=(const GiteaClient&) = delete;

    /// Obtain a runner registration token via the Gitea REST API.
    /// Requires a Gitea admin API token in the Authorization header.
    ///
    /// POST /api/v1/runners/registration-token
    ///
    /// @throws std::runtime_error on HTTP error or connection failure
    RegistrationTokenResponse getRegistrationToken();

    /// Verify that the Gitea server is reachable.
    /// @returns true if the server responded with 200
    bool ping();

private:
    struct Impl;
    Impl* impl_;

    std::string doPost(const std::string& path,
                       const std::string& body,
                       const std::map<std::string, std::string>& headers);

    std::string doGet(const std::string& path,
                      const std::map<std::string, std::string>& headers);
};

} // namespace runner
