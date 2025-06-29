#include "wsock.h"
#include "direct.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <functional>
#include <vector>

// Symbol visibility attributes for shared library export
#if defined(__GNUC__) || defined(__clang__)
#define EXPORT_API __attribute__((visibility("default")))
#else
#define EXPORT_API
#endif

// Forward declarations
namespace Json {
    class Value;
}

// Callback types for peer events
using PeerJoinedCallback = std::function<void(const std::string& peer_id)>;
using PeerListCallback = std::function<void(const std::vector<std::string>& peer_ids)>;
using OfferReceivedCallback = std::function<void(const std::string& peer_id, const std::string& sdp)>;
using AnswerReceivedCallback = std::function<void(const std::string& peer_id, const std::string& sdp)>;
using IceCandidateReceivedCallback = std::function<void(const std::string& peer_id, const std::string& candidate)>;
using HelloReceivedCallback = std::function<void(const std::string& peer_id)>;
using UserListReceivedCallback = std::function<void(const std::vector<std::string>& user_ids)>;

// Callback for receiving a direct IP:port address for a peer
using AddressReceivedCallback = std::function<void(const std::string& user_id,
                                                  const std::string& ip,
                                                  int port)>;

class DirectClient {
public:
    DirectClient(const std::string& user_id);
    ~DirectClient();

    bool connectToSignalingServer(const std::string& server_host, const std::string& server_port);
    bool startTcpServer(int listen_port);  // For callee to act as TCP server
    void disconnect();
    void registerWithRoom(const std::string& room_name);
    bool isConnected() const;
    bool isRegistered() const;
    
    // Callback setters
    void setPeerJoinedCallback(PeerJoinedCallback callback) { peer_joined_callback_ = callback; }
    void setPeerListCallback(PeerListCallback callback) { peer_list_callback_ = callback; }
    void setOfferReceivedCallback(OfferReceivedCallback callback) { offer_received_callback_ = callback; }
    void setAnswerReceivedCallback(AnswerReceivedCallback callback) { answer_received_callback_ = callback; }
    void setIceCandidateReceivedCallback(IceCandidateReceivedCallback callback) { ice_candidate_received_callback_ = callback; }
    void setHelloReceivedCallback(HelloReceivedCallback callback) { hello_received_callback_ = callback; }
    void setAddressReceivedCallback(AddressReceivedCallback callback) { address_received_callback_ = callback; }
    void setUserListReceivedCallback(UserListReceivedCallback callback) { user_list_received_callback_ = callback; }
  
    // Message sending methods
    bool sendOffer(const std::string& target_peer_id, const std::string& sdp);
    bool sendAnswer(const std::string& target_peer_id, const std::string& sdp);
    bool sendIceCandidate(const std::string& target_peer_id, const std::string& candidate);
    bool sendAddress(const std::string& user_id, const std::string& ip, int port);
    bool sendInit();
    bool sendBye();
    bool sendCancel();
    bool sendHelloToUser(const std::string& target_user_id);  // Send HELLO to specific user
    bool requestUserList();  // Request list of users from signaling server
    
    // Called by owner (callee) to ensure ADDRESS line is replayed after reconnect
    void setAddressToPublish(const std::string& addr) { pending_address_ = addr; }
    void setRoomToPublish(const std::string& room) { pending_room_ = room; }
    
    // Temporarily stop / restart WebSocket listening without closing the TCP
    // connection so the peer stays registered on the signalling server.
    void pause();   // Stop keep-alive & async_read
    void resume();  // Restart them

protected:
    std::unique_ptr<WebSocketClient> ws_client_;
    std::unique_ptr<rtc::Thread> network_thread_;
    std::string jwt_token_;
    std::string user_id_;
    bool connected_;
    bool registered_;
    std::string pending_room_;
    
    // Callbacks for peer events
    PeerJoinedCallback peer_joined_callback_;
    PeerListCallback peer_list_callback_;
    OfferReceivedCallback offer_received_callback_;
    AnswerReceivedCallback answer_received_callback_;
    IceCandidateReceivedCallback ice_candidate_received_callback_;
    HelloReceivedCallback hello_received_callback_;
    AddressReceivedCallback address_received_callback_;
    UserListReceivedCallback user_list_received_callback_;
    
    std::function<void(const std::string&)> default_ws_handler_;
    
private:
    std::string getJwtToken(const std::string& server_host, int server_port, 
                           const std::string& userId, const std::string& password);
    void handleProtocolMessage(const std::string& message);
    void handleWebSocketMessage(const std::string& message);
    void handleSocketIOEvent(const std::string& json_content);
    void handlePeerJoined(const Json::Value& message);
    void handlePeerList(const Json::Value& message);
    void handleOffer(const Json::Value& message);
    void handleAnswer(const Json::Value& message);
    void handleIceCandidate(const Json::Value& message);
    void handleError(const Json::Value& message);

    std::string saved_room_;        // last room we registered with
    std::string pending_address_;   // full ADDRESS:... line to replay after reconnect
};

// DirectCallerClient - Initiates calls to other peers by name using signaling server
class EXPORT_API DirectCallerClient : public DirectCaller {
protected:
    // For signaling server communication
    std::unique_ptr<DirectClient> signaling_client_;

private:
    bool initialized_ = false;
    std::string resolved_target_ip_;
    int resolved_target_port_ = 0;
    rtc::Thread* owner_thread_ = nullptr; // Thread where the object was created
    UserListReceivedCallback user_list_callback_;

public:
    // Alternate constructor taking fully-populated Options directly
    explicit DirectCallerClient(const Options& opts);
    ~DirectCallerClient();

    bool Initialize();
    bool Connect();
    void Disconnect();
    bool IsConnected() const;
    void RunOnBackgroundThread();
    bool WaitUntilConnectionClosed(int timeout_ms = 5000);
    
    // Set target user to call by name
    void SetTargetUser(const std::string& target_user_id) { opts_.target_name = target_user_id; }
    
    // Add SetUserListCallback and RequestUserList to DirectCallerClient public section
    void SetUserListCallback(UserListReceivedCallback callback) { signaling_client_->setUserListReceivedCallback(callback); }
    bool RequestUserList();
    
private:
    void onPeerJoined(const std::string& peer_id);
    void onPeerAddressResolved(const std::string& peer_id, const std::string& ip, int port);
    void initiateWebRTCCall(const std::string& ip, int port);
    void onAnswerReceived(const std::string& peer_id, const std::string& sdp);
    void onIceCandidateReceived(const std::string& peer_id, const std::string& candidate);
};

// DirectCalleeClient - Accepts incoming calls by name using signaling server  
class EXPORT_API DirectCalleeClient : public DirectCallee, public std::enable_shared_from_this<DirectCalleeClient> {
protected:
    std::unique_ptr<DirectClient> signaling_client_;  // For signaling server communication

private:
    bool initialized_ = false;
    bool listening_ = false;

public:
    explicit DirectCalleeClient(const Options& opts);
    ~DirectCalleeClient();

    bool Initialize();
    bool StartListening();
    void StopListening();
    bool IsListening() const;
    bool IsConnected() const;
    void RunOnBackgroundThread();
    bool WaitUntilConnectionClosed(int timeout_ms = 5000);
    void ResetConnectionClosedEvent();
    void SignalQuit();
    
private:
    void onIncomingCall(const std::string& peer_id, const std::string& sdp);
    void setupWebRTCListener();
    void publishAddressToSignalingServer();
    void onIceCandidateReceived(const std::string& peer_id, const std::string& candidate);
};

// ---------------------------------------------------------------------------
//  C-friendly wrapper: allows Objective-C / Swift etc. to register a callback
//  without constructing a std::function object in the caller's binary – this
//  prevents libc++ ABI mismatches between the framework and the application.
//
//  The wrapper lives inside the framework (compiled with the same tool-chain
//  as the rest of the C++ code) and internally converts the plain C callback
//  into the std::function that DirectClient expects.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DirectUserListCallbackC)(const char** users,
                                        int          count,
                                        void*        context);

// Registers a C-style callback that will be invoked whenever a fresh list of
// user IDs is received from the signaling server. The callback is executed on
// the signaling / network thread, therefore the callee must forward to the UI
// thread if UI work is required.
EXPORT_API void DirectCallerClient_SetUserListCallbackC(
        DirectCallerClient*       client,
        DirectUserListCallbackC   callback,
        void*                     context);

#ifdef __cplusplus
} // extern "C"
#endif
