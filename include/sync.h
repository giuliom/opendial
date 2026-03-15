#pragma once

#include <alarm.h>

#include <asio.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace opendial::sync {

// ── Wire protocol ────────────────────────────────────────────────────────
//
// Every message on the TCP stream is framed as:
//
//   [4 bytes big-endian payload length] [1 byte message type] [payload …]
//
// Alarm payloads use Alarm::serialize() / Alarm::deserialize().
// Multi-alarm payloads (FullSyncResponse) separate records with '\x1E'.
// AlarmDelete payloads carry  "uuid\x1Fversion".

enum class MessageType : std::uint8_t {
    FullSyncRequest  = 1, // client → server  (empty payload)
    FullSyncResponse = 2, // server → client  (all alarms)
    AlarmUpdate      = 3, // bidirectional     (single alarm)
    AlarmDelete      = 4, // bidirectional     (uuid + version)
    Ack              = 5, // generic ack       (empty payload)
};

struct SyncMessage {
    MessageType type;
    std::string payload;

    // Encode into a length-prefixed byte buffer ready to send on the wire.
    std::vector<char> encode() const;
    // Decode type + payload that was already extracted from the frame.
    static std::optional<SyncMessage> decode(const char* data, std::size_t len);
};

// ── TCP session (one per connected peer) ─────────────────────────────────

class SyncSession : public std::enable_shared_from_this<SyncSession> {
public:
    using MessageHandler =
        std::function<void(std::shared_ptr<SyncSession>, const SyncMessage&)>;
    using DisconnectHandler =
        std::function<void(std::shared_ptr<SyncSession>)>;

    SyncSession(asio::ip::tcp::socket socket,
                MessageHandler on_message,
                DisconnectHandler on_disconnect);

    void start();
    void send(const SyncMessage& msg);

private:
    void readHeader();
    void readBody(std::uint32_t length);

    asio::ip::tcp::socket socket_;
    MessageHandler on_message_;
    DisconnectHandler on_disconnect_;
    std::array<char, 4> header_buf_{};
    std::vector<char> body_buf_;
};

// ── Sync server ──────────────────────────────────────────────────────────
// Listens on a TCP port.  Connected clients exchange alarm updates.
// The server relays every incoming update to all *other* sessions and
// merges it into the local AlarmManager.

class SyncServer {
public:
    SyncServer(alarm::AlarmManager& manager, std::uint16_t port);
    ~SyncServer();

    void start();
    void stop();

    // Push a local change to every connected client.
    void broadcastUpdate(const alarm::Alarm& alarm);
    void broadcastDelete(const std::string& uuid, std::uint64_t version);

    std::uint16_t port() const { return port_; }

private:
    void accept();
    void handleMessage(std::shared_ptr<SyncSession> session,
                       const SyncMessage& msg);
    void handleDisconnect(std::shared_ptr<SyncSession> session);
    void broadcast(const SyncMessage& msg,
                   const std::shared_ptr<SyncSession>& exclude);

    alarm::AlarmManager& manager_;
    std::uint16_t port_;
    asio::io_context io_context_;
    std::optional<asio::ip::tcp::acceptor> acceptor_;
    std::set<std::shared_ptr<SyncSession>> sessions_;
    std::mutex sessions_mutex_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
};

// ── Sync client ──────────────────────────────────────────────────────────
// Connects to a SyncServer, pushes local changes, and applies remote ones.

class SyncClient {
public:
    SyncClient(alarm::AlarmManager& manager,
               const std::string& host,
               std::uint16_t port);
    ~SyncClient();

    void connect();
    void disconnect();
    bool isConnected() const;

    void pushAlarm(const alarm::Alarm& alarm);
    void pushDelete(const std::string& uuid, std::uint64_t version);
    void requestFullSync();

private:
    void readHeader();
    void readBody(std::uint32_t length);
    void handleMessage(const SyncMessage& msg);
    void doSend(const SyncMessage& msg);

    alarm::AlarmManager& manager_;
    std::string host_;
    std::uint16_t port_;
    asio::io_context io_context_;
    std::optional<asio::ip::tcp::socket> socket_;
    std::thread io_thread_;
    std::atomic<bool> connected_{false};
    std::array<char, 4> header_buf_{};
    std::vector<char> body_buf_;
};

} // namespace opendial::sync
