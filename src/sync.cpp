#include <sync.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

namespace opendial::sync {

// ── SyncMessage ──────────────────────────────────────────────────────────

static constexpr char kRecordSep = '\x1E';
static constexpr char kFieldSep  = '\x1F';

static void writeUint32BE(char* buf, std::uint32_t val) {
    buf[0] = static_cast<char>((val >> 24) & 0xFF);
    buf[1] = static_cast<char>((val >> 16) & 0xFF);
    buf[2] = static_cast<char>((val >>  8) & 0xFF);
    buf[3] = static_cast<char>((val      ) & 0xFF);
}

static std::uint32_t readUint32BE(const char* buf) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[2])) <<  8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[3])));
}

std::vector<char> SyncMessage::encode() const {
    // Frame: [4-byte length] [1-byte type] [payload...]
    std::uint32_t payload_len =
        static_cast<std::uint32_t>(1 + payload.size());
    std::vector<char> buf(4 + payload_len);
    writeUint32BE(buf.data(), payload_len);
    buf[4] = static_cast<char>(type);
    std::memcpy(buf.data() + 5, payload.data(), payload.size());
    return buf;
}

std::optional<SyncMessage> SyncMessage::decode(const char* data,
                                                std::size_t len) {
    if (len < 1) return std::nullopt;
    SyncMessage msg;
    msg.type = static_cast<MessageType>(static_cast<unsigned char>(data[0]));
    if (len > 1) msg.payload.assign(data + 1, len - 1);
    return msg;
}

// ── SyncSession ──────────────────────────────────────────────────────────

SyncSession::SyncSession(asio::ip::tcp::socket socket,
                         MessageHandler on_message,
                         DisconnectHandler on_disconnect)
    : socket_(std::move(socket)),
      on_message_(std::move(on_message)),
      on_disconnect_(std::move(on_disconnect)) {}

void SyncSession::start() { readHeader(); }

void SyncSession::send(const SyncMessage& msg) {
    auto buf = std::make_shared<std::vector<char>>(msg.encode());
    auto self = shared_from_this();
    asio::async_write(
        socket_, asio::buffer(*buf),
        [self, buf](std::error_code /*ec*/, std::size_t /*n*/) {
            // fire-and-forget; disconnect handled in read path
        });
}

void SyncSession::readHeader() {
    auto self = shared_from_this();
    asio::async_read(
        socket_, asio::buffer(header_buf_),
        [this, self](std::error_code ec, std::size_t /*n*/) {
            if (ec) {
                on_disconnect_(self);
                return;
            }
            std::uint32_t body_len = readUint32BE(header_buf_.data());
            if (body_len == 0 || body_len > 16 * 1024 * 1024) {
                on_disconnect_(self);
                return;
            }
            readBody(body_len);
        });
}

void SyncSession::readBody(std::uint32_t length) {
    body_buf_.resize(length);
    auto self = shared_from_this();
    asio::async_read(
        socket_, asio::buffer(body_buf_),
        [this, self, length](std::error_code ec, std::size_t /*n*/) {
            if (ec) {
                on_disconnect_(self);
                return;
            }
            auto msg = SyncMessage::decode(body_buf_.data(), length);
            if (msg) on_message_(self, *msg);
            readHeader(); // continue reading
        });
}

// ── SyncServer ───────────────────────────────────────────────────────────

SyncServer::SyncServer(alarm::AlarmManager& manager, std::uint16_t port)
    : manager_(manager), port_(port) {}

SyncServer::~SyncServer() { stop(); }

void SyncServer::start() {
    if (running_.exchange(true)) return; // already running

    acceptor_.emplace(io_context_,
                      asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port_));
    accept();

    io_thread_ = std::thread([this] {
        io_context_.run();
    });
}

void SyncServer::stop() {
    if (!running_.exchange(false)) return;
    io_context_.stop();
    if (io_thread_.joinable()) io_thread_.join();
    std::lock_guard lock(sessions_mutex_);
    sessions_.clear();
}

void SyncServer::accept() {
    acceptor_->async_accept([this](std::error_code ec,
                                   asio::ip::tcp::socket socket) {
        if (ec || !running_) return;

        auto session = std::make_shared<SyncSession>(
            std::move(socket),
            [this](auto s, auto& m) { handleMessage(s, m); },
            [this](auto s) { handleDisconnect(s); });

        {
            std::lock_guard lock(sessions_mutex_);
            sessions_.insert(session);
        }
        session->start();
        accept(); // accept next connection
    });
}

void SyncServer::handleMessage(std::shared_ptr<SyncSession> session,
                                const SyncMessage& msg) {
    switch (msg.type) {
    case MessageType::FullSyncRequest: {
        // Respond with all alarms.
        auto alarms = manager_.getAllAlarms();
        std::string payload;
        for (std::size_t i = 0; i < alarms.size(); ++i) {
            if (i > 0) payload += kRecordSep;
            payload += alarms[i].serialize();
        }
        session->send({MessageType::FullSyncResponse, std::move(payload)});
        break;
    }
    case MessageType::AlarmUpdate: {
        auto alarm = alarm::Alarm::deserialize(msg.payload);
        if (alarm) {
            manager_.mergeAlarm(*alarm);
            // Relay to other sessions.
            broadcast(msg, session);
        }
        break;
    }
    case MessageType::AlarmDelete: {
        // payload = "uuid\x1Fversion"
        auto sep = msg.payload.find(kFieldSep);
        if (sep != std::string::npos) {
            try {
                std::string uuid = msg.payload.substr(0, sep);
                std::uint64_t ver = std::stoull(msg.payload.substr(sep + 1));
                manager_.mergeDelete(uuid, ver);
                broadcast(msg, session);
            } catch (const std::exception&) {
                // Ignore malformed delete payloads from the wire.
            }
        }
        break;
    }
    default:
        break;
    }
}

void SyncServer::handleDisconnect(std::shared_ptr<SyncSession> session) {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(session);
}

void SyncServer::broadcast(const SyncMessage& msg,
                            const std::shared_ptr<SyncSession>& exclude) {
    std::lock_guard lock(sessions_mutex_);
    for (auto& s : sessions_) {
        if (s != exclude) s->send(msg);
    }
}

void SyncServer::broadcastUpdate(const alarm::Alarm& alarm) {
    SyncMessage msg{MessageType::AlarmUpdate, alarm.serialize()};
    broadcast(msg, nullptr);
}

void SyncServer::broadcastDelete(const std::string& uuid,
                                  std::uint64_t version) {
    std::string payload = uuid;
    payload += kFieldSep;
    payload += std::to_string(version);
    SyncMessage msg{MessageType::AlarmDelete, std::move(payload)};
    broadcast(msg, nullptr);
}

// ── SyncClient ───────────────────────────────────────────────────────────

SyncClient::SyncClient(alarm::AlarmManager& manager,
                       const std::string& host,
                       std::uint16_t port)
    : manager_(manager), host_(host), port_(port) {}

SyncClient::~SyncClient() { disconnect(); }

void SyncClient::connect() {
    if (connected_) return;

    asio::ip::tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(host_, std::to_string(port_));

    socket_.emplace(io_context_);
    asio::connect(*socket_, endpoints);
    connected_ = true;

    readHeader();

    io_thread_ = std::thread([this] {
        io_context_.run();
    });
}

void SyncClient::disconnect() {
    if (!connected_.exchange(false)) return;
    io_context_.stop();
    if (io_thread_.joinable()) io_thread_.join();
    if (socket_ && socket_->is_open()) {
        std::error_code ec;
        socket_->close(ec);
    }
}

bool SyncClient::isConnected() const { return connected_; }

void SyncClient::pushAlarm(const alarm::Alarm& alarm) {
    doSend({MessageType::AlarmUpdate, alarm.serialize()});
}

void SyncClient::pushDelete(const std::string& uuid, std::uint64_t version) {
    std::string payload = uuid;
    payload += kFieldSep;
    payload += std::to_string(version);
    doSend({MessageType::AlarmDelete, std::move(payload)});
}

void SyncClient::requestFullSync() {
    doSend({MessageType::FullSyncRequest, {}});
}

void SyncClient::doSend(const SyncMessage& msg) {
    if (!connected_ || !socket_) return;
    auto buf = std::make_shared<std::vector<char>>(msg.encode());
    asio::async_write(
        *socket_, asio::buffer(*buf),
        [buf](std::error_code /*ec*/, std::size_t /*n*/) {});
}

void SyncClient::readHeader() {
    if (!socket_) return;
    auto* self = this;
    asio::async_read(
        *socket_, asio::buffer(header_buf_),
        [self](std::error_code ec, std::size_t /*n*/) {
            if (ec) {
                self->connected_ = false;
                return;
            }
            std::uint32_t body_len = readUint32BE(self->header_buf_.data());
            if (body_len == 0 || body_len > 16 * 1024 * 1024) {
                self->connected_ = false;
                return;
            }
            self->readBody(body_len);
        });
}

void SyncClient::readBody(std::uint32_t length) {
    body_buf_.resize(length);
    auto* self = this;
    asio::async_read(
        *socket_, asio::buffer(body_buf_),
        [self, length](std::error_code ec, std::size_t /*n*/) {
            if (ec) {
                self->connected_ = false;
                return;
            }
            auto msg = SyncMessage::decode(self->body_buf_.data(), length);
            if (msg) self->handleMessage(*msg);
            self->readHeader();
        });
}

void SyncClient::handleMessage(const SyncMessage& msg) {
    switch (msg.type) {
    case MessageType::FullSyncResponse: {
        // Parse record-separated alarms.
        std::string_view payload(msg.payload);
        std::size_t start = 0;
        while (start <= payload.size()) {
            auto end = payload.find(kRecordSep, start);
            if (end == std::string_view::npos) end = payload.size();
            auto record = payload.substr(start, end - start);
            if (!record.empty()) {
                auto alarm = alarm::Alarm::deserialize(record);
                if (alarm) manager_.mergeAlarm(*alarm);
            }
            start = end + 1;
        }
        break;
    }
    case MessageType::AlarmUpdate: {
        auto alarm = alarm::Alarm::deserialize(msg.payload);
        if (alarm) manager_.mergeAlarm(*alarm);
        break;
    }
    case MessageType::AlarmDelete: {
        auto sep = msg.payload.find(kFieldSep);
        if (sep != std::string::npos) {
            try {
                std::string uuid = msg.payload.substr(0, sep);
                std::uint64_t ver = std::stoull(msg.payload.substr(sep + 1));
                manager_.mergeDelete(uuid, ver);
            } catch (const std::exception&) {
                // Ignore malformed delete payloads from the wire.
            }
        }
        break;
    }
    default:
        break;
    }
}

} // namespace opendial::sync
