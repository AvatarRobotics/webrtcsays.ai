#pragma once

#include "direct.h"  // for DirectPeer convenience helpers
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <curl/curl.h>

// Forward declaration for list type (already defined in curl.h but safe)
struct curl_slist;
namespace Json { class Value; }

// High-level callback signalling that the Cloudflare session is ready (ICE/DTLS connected).
using CfReadyCallback   = std::function<void()>;
using CfErrorCallback   = std::function<void(const std::string&)>;

// CloudflareClient wraps the REST signalling flow shown in the echo demo HTML page.
// It can push local tracks (audio/video) and/or pull remote tracks (echo).
//
// 1. createCallsSession()   – POST /sessions/new -> returns sessionId
// 2. createOffer()          – RTCPeerConnection local offer
// 3. POST /tracks/new       – push local tracks, receive answer
// 4. setRemoteDescription() – apply answer
// 5. ICE/DTLS completes     – media flows to Cloudflare.
//
// For pulling tracks (echo):
// 6. POST /tracks/new (remote) with track list, get offer
// 7. setRemoteDescription(offer) + createAnswer() + PUT /renegotiate
//
class DIRECT_API CloudflareClient : public DirectPeer {
public:
    struct Options {
        std::string app_id;         // Calls App ID
        std::string app_token;      // Short-lived API token (DO NOT embed in production)
        std::string user_name;      // Arbitrary label for logging
        bool        echo_mode = true; // If true perform send+pull echo similar to demo
    };

    explicit CloudflareClient(const Options& opts);
    ~CloudflareClient() override;

    // High-level lifecycle
    bool Initialize();                // creates threads, capturers, etc.
    bool Start();                     // starts signalling with Cloudflare and WebRTC.
    void Stop();                      // tears down everything.

    // Status helpers
    bool IsReady()   const { return ready_.load(); }
    bool HasFailed() const { return failed_.load(); }

    // App-level callbacks
    void SetReadyCallback(CfReadyCallback cb) { ready_cb_ = std::move(cb); }
    void SetErrorCallback(CfErrorCallback cb) { error_cb_ = std::move(cb); }

private:
    // === Internal helpers ===
    bool createMeeting();
    bool addParticipant();

    bool createLocalMediaTracks();
    bool createCloudflareSession();           // POST /sessions/new -> local_session_id_
    bool pushLocalTracks();                   // POST /tracks/new (local)
    bool setupEchoPull();                     // optional echo part

    // HTTP helpers (blocking – ok for prototype)
    bool httpPost(const std::string& url, const Json::Value& body, Json::Value& out, curl_slist* headers = nullptr);
    bool httpPut (const std::string& url, const Json::Value& body, Json::Value& out);

    // DirectPeer overrides
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

private:
    Options opts_;

    std::string api_base_;            // https://rtc.live.cloudflare.com/v1/apps/<app_id>
    std::string meeting_id_;
    std::string participant_id_;
    std::string participant_token_;
    std::string local_session_id_;  
    std::string remote_session_id_;   // echo session

    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};

    // CURL shared objects
    CURL* curl_ = nullptr;
    curl_slist* default_headers_ = nullptr;

    CfReadyCallback ready_cb_;
    CfErrorCallback error_cb_;
}; 