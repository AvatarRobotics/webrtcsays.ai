#include "cloudflare.h"
#include <json/json.h>
#include <thread>
#include <chrono>
#include "status.h"
#include "option.h"
#include "rtc_base/third_party/base64/base64.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/scoped_ref_ptr.h"
#include "api/create_peerconnection_factory.h"
#include "api/scoped_refptr.h"
#include "api/sctp_transport_interface.h"
#include "api/session_description_interface.h"
#include "pc/session_description.h"

constexpr const char* kApiBase = "https://rtk.realtime.cloudflare.com/v2";
constexpr const char* kApiMeetingPayload = R"({
  "title": "string",
  "preferred_region": "us-east-1",
  "record_on_start": false,
  "live_stream_on_start": false,
  "recording_config": {
    "max_seconds": 60,
    "file_name_prefix": "string",
    "video_config": {
      "codec": "H264",
      "width": 1280,
      "height": 720,
      "watermark": {
        "url": "https://github.com/wilddolphin2022/webrtcsays.ai/raw/main/webrtcsaysai.jpg",
        "size": {
          "width": 1,
          "height": 1
        },
        "position": "left top"
      },
      "export_file": true
    },
    "audio_config": {
      "codec": "AAC",
      "channel": "stereo",
      "export_file": true
    },
    "storage_config": {
      "type": "aws",
      "access_key": "string",
      "secret": "string",
      "bucket": "string",
      "region": "us-east-1",
      "path": "string",
      "auth_method": "KEY",
      "username": "string",
      "password": "string",
      "host": "string",
      "port": 0,
      "private_key": "string"
    },
    "realtimekit_bucket_config": {
      "enabled": true
    },
    "live_streaming_config": {
      "rtmp_url": "rtmp://a.rtmp.youtube.com/live2/qc2s-h4ts-se9v-srvg-fu3u"
    }
  },
  "ai_config": {
    "transcription": {
      "keywords": [
        "string"
      ],
      "language": "en-US",
      "profanity_filter": false
    },
    "summarization": {
      "word_limit": 500,
      "text_format": "markdown",
      "summary_type": "general"
    }
  },
  "persist_chat": false,
  "summarize_on_end": false
})";

constexpr const char* kApiSessionNew = "/sessions/new";
constexpr const char* kApiSessionTracksNew = "/sessions/%s/tracks/new";

static size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* str = reinterpret_cast<std::string*>(userdata);
    str->append(ptr, size * nmemb);
    return size * nmemb;
}

CloudflareClient::CloudflareClient(const Options& opts)
    : DirectPeer(::Options{}), // pass default Direct Options
      opts_(opts) {
    api_base_ = kApiBase;
}

CloudflareClient::~CloudflareClient() {
    Stop();
}

bool CloudflareClient::Initialize() {
    if (!DirectPeer::Initialize()) {
        APP_LOG(AS_ERROR) << "CloudflareClient: DirectPeer init failed";
        return false;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_ = curl_easy_init();
    if (!curl_) {
        APP_LOG(AS_ERROR) << "CloudflareClient: curl_easy_init failed";
        return false;
    }

    // Prepare default Authorization header
    std::string auth_raw = opts_.app_id + ":" + opts_.app_token;
    std::string auth_b64 = rtc::Base64::Encode(auth_raw);
    std::string auth = "Authorization: Basic " + auth_b64;
    default_headers_ = curl_slist_append(default_headers_, auth.c_str());
    default_headers_ = curl_slist_append(default_headers_, "Content-Type: application/json");

    return true;
}

bool CloudflareClient::Start() {
    if (!createLocalMediaTracks()) {
        APP_LOG(AS_ERROR) << "CloudflareClient: failed to create local tracks";
        return false;
    }
    if (!createCloudflareSession()) return false;
    if (!pushLocalTracks())        return false;
    if (opts_.echo_mode) {
        if (!setupEchoPull()) {
            APP_LOG(AS_WARNING) << "CloudflareClient: echo pull failed (continuing send-only)";
        }
    }
    return true;
}

void CloudflareClient::Stop() {
    curl_slist_free_all(default_headers_);
    if (curl_) curl_easy_cleanup(curl_);
    curl_global_cleanup();
    DirectPeer::Disconnect();
}

bool CloudflareClient::createLocalMediaTracks() {
    // For MVP we create a dummy audio/video source, or reuse DirectPeer helpers.
    return CreatePeerConnection();
}

bool CloudflareClient::createMeeting() {
    std::string url = api_base_ + kApiSessionNew;
    Json::Value body(kApiMeetingPayload);
    Json::Value resp;
    if (!httpPost(url, body, resp)) return false;
    if (!resp.isMember("data")) {
        APP_LOG(AS_ERROR) << "Cloudflare Meeting API: data missing in response " << resp.toStyledString();
        return false;
    }
    meeting_id_ = resp["data"]["id"].asString();
    return true;
}

bool CloudflareClient::addParticipant() {
    std::string url = api_base_ + "/meetings/" + meeting_id_ + "/participants";

    Json::Value body(Json::objectValue);
    body["name"] = opts_.user_name;
    body["picture"] = "https://i.imgur.com/test.jpg";
    body["preset_name"] = "all";

    Json::Value resp;
    if (!httpPost(url, body, resp)) return false;

    if (!resp["success"].asBool()) {
        APP_LOG(AS_ERROR) << "addParticipant failed: " << resp.toStyledString();
        return false;
    }

    participant_id_ = resp["data"]["id"].asString();
    participant_token_ = resp["data"]["token"].asString();

    return true;
}

bool CloudflareClient::createCloudflareSession() {
    if (!createMeeting()) return false;
    if (!addParticipant()) return false;
    return true;
}

bool CloudflareClient::pushLocalTracks() {
    // Create offer
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
    rtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver> offer_observer =
        rtc::make_ref_counted<LambdaCreateSessionDescriptionObserver>([this](std::unique_ptr<webrtc::SessionDescriptionInterface> offer) {
            std::string sdp;
            offer->ToString(&sdp);
            this->peer_connection()->SetLocalDescription(std::move(offer), nullptr);
        });
    peer_connection()->CreateOffer(offer_observer.get(), opts);

    // Busy-wait until local description set (simplified for skeleton)
    int wait_ms = 0;
    while (!peer_connection()->local_description() && wait_ms < 3000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        wait_ms += 10;
    }
    if (!peer_connection()->local_description()) {
        APP_LOG(AS_ERROR) << "CloudflareClient: local description not ready";
        return false;
    }
    std::string local_sdp;
    peer_connection()->local_description()->ToString(&local_sdp);

    // Build tracks array (single transceiver sendonly for now)
    Json::Value tracks(Json::arrayValue);
    tracks.append(Json::Value(Json::objectValue)); // placeholder

    Json::Value body(Json::objectValue);
    body["sessionDescription"]["sdp"]  = local_sdp;
    body["sessionDescription"]["type"] = "offer";
    body["tracks"] = tracks;

    std::string url = api_base_ + kApiSessionTracksNew;
    Json::Value resp;
    if (!httpPost(url, body, resp)) return false;

    if (!resp.isMember("sessionDescription")) {
        APP_LOG(AS_ERROR) << "CloudflareClient: answer missing";
        return false;
    }
    std::string answer_sdp = resp["sessionDescription"]["sdp"].asString();
    webrtc::SdpParseError err;
    auto answer_desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, answer_sdp, &err);
    if (!answer_desc) {
        APP_LOG(AS_ERROR) << "CloudflareClient: parse answer failed " << err.description;
        return false;
    }
    peer_connection()->SetRemoteDescription(std::move(answer_desc), nullptr);
    return true;
}

bool CloudflareClient::setupEchoPull() {
    // Placeholder simplified: we skip full echo pulling logic.
    return true;
}

bool CloudflareClient::httpPost(const std::string& url, const Json::Value& body, Json::Value& out, curl_slist* headers = default_headers_) {
    std::string payload = Json::writeString(Json::StreamWriterBuilder(), body);
    std::string response;

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, payload.size());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        APP_LOG(AS_ERROR) << "Curl POST failed: " << curl_easy_strerror(res);
        return false;
    }
    Json::CharReaderBuilder rb; std::string errs;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    if (!reader->parse(response.c_str(), response.c_str()+response.size(), &out, &errs)) {
        APP_LOG(AS_ERROR) << "JSON parse error: " << errs;
        return false;
    }
    return true;
}

bool CloudflareClient::httpPut(const std::string& url, const Json::Value& body, Json::Value& out) {
    std::string payload = Json::writeString(Json::StreamWriterBuilder(), body);
    std::string response;

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, default_headers_);
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, payload.size());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        APP_LOG(AS_ERROR) << "Curl PUT failed: " << curl_easy_strerror(res);
        return false;
    }
    Json::CharReaderBuilder rb; std::string errs;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    if (!reader->parse(response.c_str(), response.c_str()+response.size(), &out, &errs)) {
        APP_LOG(AS_ERROR) << "JSON parse error: " << errs;
        return false;
    }
    return true;
}

void CloudflareClient::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    DirectPeer::OnIceConnectionChange(new_state);
    if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected ||
        new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
        ready_ = true;
        if (ready_cb_) ready_cb_();
    }
}

void CloudflareClient::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    DirectPeer::OnConnectionChange(new_state);
    if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed) {
        failed_ = true;
        if (error_cb_) error_cb_("Peer connection failed");
    }
} 