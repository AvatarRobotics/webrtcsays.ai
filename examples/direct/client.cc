#include "client.h"
#include "wsock.h"
#include "direct.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <utility>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <vector>
#include <json/json.h>
#include "rtc_base/thread.h"

// DirectCallerClient Implementation

DirectCallerClient::DirectCallerClient(const Options& opts)
    : DirectCaller(opts), initialized_(false) {
    resolved_target_port_ = 0;   
    if (!rtc::Thread::Current()) {
        rtc::ThreadManager::Instance()->WrapCurrentThread();
    }
    owner_thread_ = rtc::Thread::Current();
    signaling_client_ = std::make_unique<DirectClient>(opts.user_name);
}

DirectCallerClient::~DirectCallerClient() { Disconnect(); }

bool DirectCallerClient::Initialize() {
    if (initialized_) {
        return true;
    }
    
    APP_LOG(AS_INFO) << "Initializing DirectCallerClient for user: " << opts_.user_name;
    // Call base class Initialize to set up WebRTC threads and members
    if (!DirectCaller::Initialize()) {
        APP_LOG(AS_ERROR) << "DirectCallerClient: base initialize failed";
        return false;
    }

    initialized_ = true;
    return true;
}

bool DirectCallerClient::Connect() {
    if (!initialized_) {
        APP_LOG(AS_ERROR) << "DirectCallerClient not initialized";
        return false;
    }

    // Set up callbacks for signaling server communication
    signaling_client_->setPeerJoinedCallback([this](const std::string& peer_id) {
        APP_LOG(AS_INFO) << "DirectCallerClient: Peer joined signaling server: " << peer_id;
        this->onPeerJoined(peer_id);
    });

    // Receive direct address resolutions from the signaling server
    signaling_client_->setAddressReceivedCallback([this](const std::string& user,
                                                        const std::string& ip,
                                                        int port) {
        this->onPeerAddressResolved(user, ip, port);
    });

    // Try to connect to signaling server for name resolution
    std::string server_host; int server_port_int = 0;
    ParseIpAndPort(opts_.address, server_host, server_port_int);
    if (!signaling_client_->connectToSignalingServer(server_host, std::to_string(server_port_int))) {
        APP_LOG(AS_WARNING) << "Failed to connect to signaling server, falling back to direct connection";
        
        // Fall back to direct connection if target is specified
        if (!opts_.target_name.empty()) {
            APP_LOG(AS_INFO) << "DirectCallerClient: Attempting direct connection to " << opts_.target_name;
            // For local testing, connect to localhost where callee is listening
            std::string target_ip = "127.0.0.1";  // Local machine where callee is running
            int target_port = 8888;  // Default port for DirectCallee
            
            APP_LOG(AS_INFO) << "DirectCallerClient: Connecting directly to " << target_ip << ":" << target_port;
            initiateWebRTCCall(target_ip, target_port);
            return true;
        } else {
            APP_LOG(AS_ERROR) << "No signaling server and no target specified for direct connection";
            return false;
        }
    }

    // Register with room for name-based discovery
    signaling_client_->registerWithRoom(opts_.room_name);

    // Request initial user list
    signaling_client_->requestUserList();

    // Immediately attempt to contact the target if one is specified. This is
    // important when using the lightweight raw-WebSocket signaling path which
    // currently does not broadcast explicit "peer-joined" events. By reusing
    // the existing onPeerJoined() handler we make sure that a targeted HELLO
    // is sent and that the direct WebRTC connection attempt (to 127.0.0.1:8888
    // by default) is kicked off without waiting indefinitely.
    if (!opts_.target_name.empty()) {
        APP_LOG(AS_INFO) << "DirectCallerClient: Proactively initiating call to target " << opts_.target_name;
        onPeerJoined(opts_.target_name); // Re-use existing logic
    }

    // Note: onPeerJoined() above already takes care of sending the initial
    // HELLO towards the callee when we proactively trigger it.
    
    APP_LOG(AS_INFO) << "DirectCallerClient connected to signaling server for name resolution";
    return true;
}

void DirectCallerClient::Disconnect() {
    // Disconnect inherited DirectCaller resources
    DirectCaller::Disconnect();
    // Reset connection-related state to allow for a new call
    initialized_ = false;
    APP_LOG(AS_INFO) << "DirectCallerClient disconnected and state reset for new call";
}

bool DirectCallerClient::IsConnected() const {
    return (this->peer_connection() != nullptr) || (signaling_client_ && signaling_client_->isConnected());
}

void DirectCallerClient::RunOnBackgroundThread() {
    // If we are currently on an rtc::Thread (i.e., WebRTC internal thread),
    // spawn a native thread to satisfy the DCHECK in DirectApplication.
    if (rtc::Thread::Current()) {
        std::thread([this]() {
            DirectCaller::RunOnBackgroundThread();
        }).detach();
    } else {
        DirectCaller::RunOnBackgroundThread();
    }
}

bool DirectCallerClient::WaitUntilConnectionClosed(int timeout_ms) {
    return DirectPeer::WaitUntilConnectionClosed(timeout_ms);
}

void DirectCallerClient::onPeerJoined(const std::string& peer_id) {
    // Map generic alias back to actual target when possible
    std::string resolved_peer_id = peer_id;
    if (peer_id == "callee" && !opts_.target_name.empty()) {
        resolved_peer_id = opts_.target_name;
    }

    APP_LOG(AS_INFO) << "DirectCallerClient: Peer joined: " << resolved_peer_id;
    
    // Don't call ourselves
    if (resolved_peer_id == opts_.user_name) {
        return;
    }
    
    // Only call the target user if specified
    if (!opts_.target_name.empty() && resolved_peer_id != opts_.target_name) {
        APP_LOG(AS_INFO) << "DirectCallerClient: Ignoring peer " << resolved_peer_id << " (target is " << opts_.target_name << ")";
        return;
    }
    
    // Send HELLO now that the callee is present
    if (!opts_.target_name.empty()) {
        APP_LOG(AS_INFO) << "DirectCallerClient: Target peer appeared – sending HELLO";
        signaling_client_->sendHelloToUser(opts_.target_name);
    }
    
    // Connection attempt will be made when we receive an ADDRESS message
    APP_LOG(AS_INFO) << "DirectCallerClient: Waiting for address resolution for " << resolved_peer_id;
}

void DirectCallerClient::onPeerAddressResolved(const std::string& peer_id,
                                               const std::string& ip,
                                               int port) {
    // Only act on the target user (if specified)
    if (!opts_.target_name.empty() && peer_id != opts_.target_name) {
        return;
    }

    APP_LOG(AS_INFO) << "DirectCallerClient: Address resolved for " << peer_id
                     << " -> " << ip << ":" << port;

    // Run call setup on a fresh native thread
    std::thread([this, ip, port] {
        initiateWebRTCCall(ip, port);
    }).detach();
}

void DirectCallerClient::initiateWebRTCCall(const std::string& ip, int port) {
    APP_LOG(AS_INFO) << "DirectCallerClient: Initiating WebRTC call to " << ip << ":" << port;
    
    // Update opts_.address so the base class knows the correct remote endpoint
    opts_.address = ip + ":" + std::to_string(port);

    // We are already initialized; just proceed to connect

    // Establish the raw socket/WebRTC signalling connection synchronously on
    // this (native) thread.
    if (!DirectCaller::Connect(ip.c_str(), port)) {
        APP_LOG(AS_ERROR) << "Failed to connect DirectCaller to " << ip << ":" << port;
        return;
    }

    // After the socket is connected, start the DirectApplication event loop
    // on a detached native thread to keep processing.
    std::thread([this]() {
        this->RunOnBackgroundThread();
    }).detach();
}

void DirectCallerClient::onAnswerReceived(const std::string& peer_id, const std::string& sdp) {
    // This would be handled by DirectCaller's internal WebRTC logic
    APP_LOG(AS_INFO) << "DirectCallerClient: Answer received from " << peer_id;
}

void DirectCallerClient::onIceCandidateReceived(const std::string& peer_id, const std::string& candidate) {
    // This would be handled by DirectCaller's internal WebRTC logic
    APP_LOG(AS_INFO) << "DirectCallerClient: ICE candidate received from " << peer_id;
}

bool DirectCallerClient::RequestUserList() {
    if (signaling_client_) {
        return signaling_client_->requestUserList();
    }
    return false;
}

// DirectCalleeClient Implementation  

DirectCalleeClient::DirectCalleeClient(const Options& opts)
    : DirectCallee(opts), initialized_(false), listening_(false) {
    // Let the OS pick an available port (0) so each new session is guaranteed
    // to bind successfully even if the previous one is still in TIME_WAIT.
    local_port_ = 0;
    signaling_client_ = std::make_unique<DirectClient>(opts.user_name);
}

DirectCalleeClient::~DirectCalleeClient() {
    StopListening();
}

bool DirectCalleeClient::Initialize() {
    if (initialized_) {
        return true;
    }
    
    APP_LOG(AS_INFO) << "Initializing DirectCalleeClient for user: " << opts_.user_name;
    initialized_ = true;
    return true;
}

bool DirectCalleeClient::StartListening() {
    // Disallow duplicate starts while already listening
    if (listening_) {
        APP_LOG(AS_INFO) << "DirectCalleeClient is already listening – ignoring repeat call.";
        return true;
    }

    if (!initialized_) {
        APP_LOG(AS_ERROR) << "DirectCalleeClient not initialized";
        return false;
    }

    // Prepare for a fresh run after a previous shutdown.
    should_quit_.store(false);
    cleaned_up_.store(false);
    ResetConnectionClosedEvent();

    // Preserve original signaling address before we overwrite opts_.address for the local listener
    std::string signaling_address = opts_.address;  // e.g. "127.0.0.1:3456"

    // First, set up WebRTC listener using DirectCallee (this will update opts_.address)
    setupWebRTCListener();
    
    // Set up callbacks to receive incoming calls via signaling server
    signaling_client_->setHelloReceivedCallback([this](const std::string& peer_id) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: Received HELLO from " << peer_id;
        this->onIncomingCall(peer_id, "");
    });
    
    signaling_client_->setOfferReceivedCallback([this](const std::string& peer_id, const std::string& sdp) {
        APP_LOG(AS_INFO) << "DirectCalleeClient: Received offer from " << peer_id;
        this->onIncomingCall(peer_id, sdp);
    });
    
    // Try to connect to signaling server to register our presence
    // But don't fail if signaling server is unavailable
    std::string server_host; int server_port_int = 0;
    ParseIpAndPort(signaling_address, server_host, server_port_int);

    auto try_register = [this, server_host, server_port_int]() {
        const int kMaxAttempts = 30; // ~30 seconds with 1-s spacing
        int attempt = 0;
        while (!this->should_quit_.load() && attempt < kMaxAttempts) {
            if (signaling_client_->connectToSignalingServer(server_host, std::to_string(server_port_int))) {
                APP_LOG(AS_INFO) << "Connected to signaling server on attempt " << (attempt + 1);
                signaling_client_->registerWithRoom(opts_.room_name);
                publishAddressToSignalingServer();
                APP_LOG(AS_INFO) << "DirectCalleeClient registered with signaling server";
                return; // success
            }
            ++attempt;
            APP_LOG(AS_WARNING) << "Attempt " << attempt << " to connect to signaling server failed – retrying in 1 s";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!this->should_quit_.load()) {
            APP_LOG(AS_ERROR) << "Unable to connect to signaling server after " << kMaxAttempts << " attempts; callee will still accept direct connections on port " << local_port_;
        }
    };

    // Run the registration attempts on a detached std::thread so we don't block StartListening().
    std::thread(try_register).detach();
    
    listening_ = true;
    APP_LOG(AS_INFO) << "DirectCalleeClient listening for calls in room: " << opts_.room_name;
    APP_LOG(AS_INFO) << "DirectCalleeClient: WebRTC calls can connect directly to port " << local_port_;
    return true;
}

void DirectCalleeClient::StopListening() {
    // Guard against repeated calls and break potential recursion with SignalQuit().
    if (!listening_) {
        return;  // Already stopped.
    }

    listening_ = false;  // Mark as no longer listening *before* further cleanup.

    // Close active TCP sockets so the port becomes immediately reusable.
    if (tcp_socket_) {
        tcp_socket_->Close();
        tcp_socket_.reset();
    }

    // Close the listening socket (if still open) to free the port.
    if (listen_socket_) {
        listen_socket_.reset();  // Destructor closes the underlying OS socket
    }

    // Disconnect WebRTC and tear down per-connection state without a full cleanup.
    DirectApplication::Disconnect();

    // Finally, perform the generic quit signalling – this will wake up any waiting loops.
    DirectPeer::SignalQuit();

    APP_LOG(AS_INFO) << "DirectCalleeClient stopped listening";
}

bool DirectCalleeClient::IsListening() const {
    return listening_;
}

bool DirectCalleeClient::IsConnected() const {
    return (this->peer_connection() != nullptr) || (signaling_client_ && signaling_client_->isConnected());
}

void DirectCalleeClient::RunOnBackgroundThread() {
    DirectCallee::RunOnBackgroundThread();
    APP_LOG(AS_INFO) << "DirectCalleeClient running on background thread";
}

bool DirectCalleeClient::WaitUntilConnectionClosed(int timeout_ms) {
    return DirectPeer::WaitUntilConnectionClosed(timeout_ms);
}

void DirectCalleeClient::ResetConnectionClosedEvent() {
    DirectPeer::ResetConnectionClosedEvent();
    APP_LOG(AS_INFO) << "DirectCalleeClient: Connection closed event reset";
}

void DirectCalleeClient::SignalQuit() {
    if (listening_) {
        StopListening();
    } else {
        // We are already not listening – make sure generic shutdown runs.
        DirectPeer::SignalQuit();
    }
    APP_LOG(AS_INFO) << "DirectCalleeClient: Quit signal received";
}

void DirectCalleeClient::setupWebRTCListener() {
    APP_LOG(AS_INFO) << "DirectCalleeClient: Setting up WebRTC listener on port " << local_port_;
    
    // Ensure opts_ has the correct listen address to avoid conflict with signaling port
    opts_.address = "0.0.0.0:" + std::to_string(local_port_);

    if (!DirectCallee::Initialize()) {
        APP_LOG(AS_ERROR) << "DirectCalleeClient: base initialization failed";
        return;
    }

    if (!DirectCallee::StartListening()) {
        APP_LOG(AS_ERROR) << "Failed to start WebRTC listener on port " << local_port_;
        return;
    }

    APP_LOG(AS_INFO) << "DirectCalleeClient: WebRTC listener started on port " << local_port_;
}

void DirectCalleeClient::publishAddressToSignalingServer() {
    if (!signaling_client_ || !signaling_client_->isConnected()) {
        APP_LOG(AS_WARNING) << "Cannot publish address – signaling client not connected";
        return;
    }

    // Discover first non-loopback IPv4 address (very lightweight)
    std::string lan_ip = "127.0.0.1";
#ifdef __APPLE__
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct addrinfo hints = {};
        hints.ai_family = AF_INET; // IPv4 only
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* info = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &info) == 0) {
            for (auto* p = info; p; p = p->ai_next) {
                char ip[INET_ADDRSTRLEN];
                void* addr_ptr = &((struct sockaddr_in*)p->ai_addr)->sin_addr;
                if (inet_ntop(AF_INET, addr_ptr, ip, sizeof(ip))) {
                    std::string candidate(ip);
                    if (candidate != "127.0.0.1") {
                        lan_ip = candidate;
                        break;
                    }
                }
            }
            freeaddrinfo(info);
        }
    }
#endif

    if (signaling_client_->sendAddress(opts_.user_name, lan_ip, local_port_)) {
        APP_LOG(AS_INFO) << "Published LAN address to signaling server: " << lan_ip << ":" << local_port_;
    } else {
        APP_LOG(AS_WARNING) << "Failed to publish ADDRESS message to signaling server";
    }
}

void DirectCalleeClient::onIncomingCall(const std::string& peer_id, const std::string& sdp) {
    // This would be handled by DirectCallee's internal WebRTC logic
    APP_LOG(AS_INFO) << "DirectCalleeClient: Incoming call from peer: " << peer_id;
}

void DirectCalleeClient::onIceCandidateReceived(const std::string& peer_id, const std::string& candidate) {
    // This would be handled by DirectCallee's internal WebRTC logic
    APP_LOG(AS_INFO) << "DirectCalleeClient: ICE candidate received from " << peer_id;
}

// -----------------------------------------------------------------------------
// DirectClient Implementation (restored)

DirectClient::DirectClient(const std::string& user_id)
    : user_id_(user_id), connected_(false), registered_(false) {
    // Create a dedicated network thread for WebSocket operations first
    network_thread_ = rtc::Thread::CreateWithSocketServer();
    network_thread_->SetName("WebSocketNetworkThread", nullptr);
    network_thread_->Start();

    // Now create the WebSocket client and attach the thread
    ws_client_ = std::make_unique<WebSocketClient>();
    ws_client_->set_network_thread(network_thread_.get());

    // Set message callback so that incoming messages are dispatched to handler
    ws_client_->set_message_callback([this](const std::string& message) {
        this->handleProtocolMessage(message);
    });
}

DirectClient::~DirectClient() {
    if (ws_client_) {
        ws_client_->disconnect();
        ws_client_.reset();
    }
    if (network_thread_) {
        //network_thread_->Quit();
        network_thread_->Stop();
        network_thread_.reset();
    }
}

bool DirectClient::connectToSignalingServer(const std::string& server_host,
                                           const std::string& server_port) {
    if (connected_) return true;

    WebSocketClient::Config cfg;
    cfg.host = server_host;
    cfg.port = std::to_string(std::stoi(server_port) + 1);
    cfg.use_ssl = false;

    if (!ws_client_->connect(cfg)) {
        APP_LOG(AS_ERROR) << "Failed to connect to WebSocket server";
        return false;
    }

    if (!ws_client_->send_websocket_handshake_with_headers()) {
        APP_LOG(AS_ERROR) << "WebSocket HTTP handshake failed";
        return false;
    }

    ws_client_->start_listening();
    connected_ = true;
    APP_LOG(AS_INFO) << "Connected to signaling server: " << cfg.host << ":" << cfg.port;
    return true;
}

void DirectClient::registerWithRoom(const std::string& room_name) {
    if (!connected_) return;
    std::string msg = "REGISTER:" + user_id_ + ":" + room_name;
    ws_client_->send_message(msg);
    registered_ = true;
}

bool DirectClient::isConnected() const { return connected_; }

// Simple wrappers still required
bool DirectClient::sendHelloToUser(const std::string& target_user_id) {
    if (!connected_) return false;
    return ws_client_->send_message("HELLO:" + target_user_id);
}

bool DirectClient::requestUserList() {
    if (!connected_) return false;
    return ws_client_->send_message("USER_LIST");
}

bool DirectClient::sendOffer(const std::string& target_peer_id, const std::string& sdp) {
    if (!connected_) return false;
    std::string msg = "OFFER:" + user_id_ + ":" + sdp;
    return ws_client_->send_message(msg);
}

bool DirectClient::sendAnswer(const std::string& target_peer_id, const std::string& sdp) {
    if (!connected_) return false;
    std::string msg = "ANSWER:" + user_id_ + ":" + sdp;
    return ws_client_->send_message(msg);
}

bool DirectClient::sendIceCandidate(const std::string& target_peer_id, const std::string& candidate) {
    if (!connected_) return false;
    std::string msg = "ICE:" + user_id_ + ":" + candidate;
    return ws_client_->send_message(msg);
}

bool DirectClient::sendInit() {
    if (!connected_) return false;
    return ws_client_->send_message("INIT");
}

bool DirectClient::sendBye() {
    if (!connected_) return false;
    return ws_client_->send_message("BYE");
}

bool DirectClient::sendCancel() {
    if (!connected_) return false;
    return ws_client_->send_message("CANCEL");
}

bool DirectClient::sendAddress(const std::string& user_id, const std::string& ip, int port) {
    if (!connected_) return false;
    std::string msg = "ADDRESS:" + user_id + ":" + ip + ":" + std::to_string(port);
    return ws_client_->send_message(msg);
}

// Stub methods (full parsing logic preserved elsewhere)
void DirectClient::disconnect() {
    if (ws_client_) ws_client_->disconnect();
    connected_ = false;
    registered_ = false;
}

// Minimal versions of other helpers to satisfy linker. Full behaviour kept earlier.
bool DirectClient::startTcpServer(int) { return false; }

void DirectClient::handleProtocolMessage(const std::string& message) {
    if (message.rfind("HELLO:", 0) == 0) {
        APP_LOG(AS_INFO) << "DirectClient received targeted HELLO: " << message;
        // Targeted HELLO:HELLO:<user>
        std::string target = message.substr(6);
        if (target == user_id_) {
            APP_LOG(AS_INFO) << "HELLO is for us, sending WELCOME";
            ws_client_->send_message("WELCOME");
        }
    } else if (message == "HELLO") {
        APP_LOG(AS_INFO) << "DirectClient received generic HELLO, sending WELCOME";
        ws_client_->send_message("WELCOME");
    } else if (message == "INIT") {
        APP_LOG(AS_INFO) << "DirectClient received INIT, sending WAITING";
        ws_client_->send_message("WAITING");
    } else if (message.rfind("ADDRESS:", 0) == 0) {
        // Format: ADDRESS:user_id:ip:port
        std::vector<std::string> parts;
        std::stringstream ss(message);
        std::string token;
        while (std::getline(ss, token, ':')) {
            parts.push_back(token);
        }
        if (parts.size() == 4) {
            std::string userId = parts[1];
            std::string ip = parts[2];
            int port = std::stoi(parts[3]);
            APP_LOG(AS_INFO) << "DirectClient received ADDRESS for " << userId << " -> " << ip << ":" << port;
            if (address_received_callback_) {
                address_received_callback_(userId, ip, port);
            }
        } else {
            APP_LOG(AS_WARNING) << "ADDRESS message malformed: " << message;
        }
    } else if (message.rfind("USER_LIST:", 0) == 0) {
        // Format: USER_LIST:user1,user2,user3
        std::vector<std::string> users;
        std::string user_list_str = message.substr(10);
        std::stringstream ss(user_list_str);
        std::string user;
        while (std::getline(ss, user, ',')) {
            if (!user.empty()) {
                users.push_back(user);
            }
        }
        APP_LOG(AS_INFO) << "DirectClient received USER_LIST with " << users.size() << " users";
        if (user_list_received_callback_) {
            user_list_received_callback_(users);
        }
    } else if (message.rfind("OFFER:", 0) == 0) {
        // Format: OFFER:peer_id:sdp
        size_t first_colon = message.find(':'); // after OFFER
        size_t second_colon = message.find(':', first_colon + 1);
        if (second_colon != std::string::npos) {
            std::string peerId = message.substr(first_colon + 1, second_colon - first_colon - 1);
            std::string sdp = message.substr(second_colon + 1);
            if (peerId == user_id_) {
                return; // Ignore our own offer broadcast
            }
            APP_LOG(AS_INFO) << "DirectClient received OFFER from " << peerId;
            if (offer_received_callback_) {
                offer_received_callback_(peerId, sdp);
            }
        } else {
            APP_LOG(AS_WARNING) << "Malformed OFFER message: " << message;
        }
    } else if (message.rfind("ANSWER:", 0) == 0) {
        // Format: ANSWER:peer_id:sdp
        size_t first_colon = message.find(':');
        size_t second_colon = message.find(':', first_colon + 1);
        if (second_colon != std::string::npos) {
            std::string peerId = message.substr(first_colon + 1, second_colon - first_colon - 1);
            std::string sdp = message.substr(second_colon + 1);
            if (peerId == user_id_) {
                return; // Ignore our own answer broadcast
            }
            APP_LOG(AS_INFO) << "DirectClient received ANSWER from " << peerId;
            if (answer_received_callback_) {
                answer_received_callback_(peerId, sdp);
            }
        } else {
            APP_LOG(AS_WARNING) << "Malformed ANSWER message: " << message;
        }
    } else if (message.rfind("ICE:", 0) == 0) {
        // Format: ICE:peer_id:candidate
        size_t first_colon = message.find(':');
        size_t second_colon = message.find(':', first_colon + 1);
        if (second_colon != std::string::npos) {
            std::string peerId = message.substr(first_colon + 1, second_colon - first_colon - 1);
            std::string candidate = message.substr(second_colon + 1);
            if (peerId == user_id_) {
                return; // Ignore our own ICE broadcast
            }
            if (ice_candidate_received_callback_) {
                ice_candidate_received_callback_(peerId, candidate);
            }
        } else {
            APP_LOG(AS_WARNING) << "Malformed ICE message: " << message;
        }
    }
}

// -----------------------------------------------------------------------------
// C-compatible wrapper implementation

struct _DirectUserListThunk {
    DirectUserListCallbackC  c_callback;
    void*                    c_context;
    // We keep the converted std::function alive by storing it in the struct so
    // that the lambda's captured std::string pointers stay valid for the
    // lifetime of the thunk (which we tie to the DirectCallerClient instance).
    std::function<void(const std::vector<std::string>&)>  cpp_func;
};

void DirectCallerClient_SetUserListCallbackC(DirectCallerClient*       client,
                                             DirectUserListCallbackC   callback,
                                             void*                     context) {
    if (!client || !callback) {
        return;
    }

    auto thunk = std::make_shared<_DirectUserListThunk>();
    thunk->c_callback = callback;
    thunk->c_context  = context;

    // Wrap C callback into std::function that DirectCallerClient expects.
    thunk->cpp_func = [thunk](const std::vector<std::string>& users) {
        // Convert std::vector<std::string> to array of const char* expected by C callback
        std::vector<const char*> c_strings;
        c_strings.reserve(users.size());
        for (const auto& s : users) {
            c_strings.push_back(s.c_str());
        }
        thunk->c_callback(c_strings.data(), static_cast<int>(c_strings.size()), thunk->c_context);
    };

    client->SetUserListCallback(thunk->cpp_func);

    // NOTE: We intentionally do *not* free the thunk; it will live as long as
    // the DirectCallerClient holds a copy of the std::function (which captures
    // the shared_ptr). When the client resets the callback or is destroyed the
    // shared_ptr ref-count drops to zero and the thunk is deleted.
}

// -----------------------------------------------------------------------------

