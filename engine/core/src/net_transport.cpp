#include <shark/net/transport.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace shark::net {
namespace {

Error net_error(ErrorCode code, const char* operation, const char* message) {
    return Error{code, message, {.operation = operation}};
}

Error tls_error(const char* operation, const char* message, int code) {
    return Error{ErrorCode::io_failure,
                 std::string{message} + " (TLS code " + std::to_string(code) + ")",
                 {.operation = operation}};
}

struct CryptoRuntime {
    std::mutex mutex;
    bool initialized{false};
    std::uint64_t last_connection_id{0};
    ~CryptoRuntime() {
        if (initialized) {
            mbedtls_psa_crypto_free();
        }
    }
};

CryptoRuntime& crypto() {
    static CryptoRuntime runtime;
    return runtime;
}

Result<void> initialize_crypto() {
    if (!crypto().initialized) {
        if (psa_crypto_init() != PSA_SUCCESS) {
            return net_error(ErrorCode::entropy_unavailable, "net.tls.initialize",
                             "TLS cryptographic initialization failed");
        }
        crypto().initialized = true;
    }
    return {};
}

#if defined(_WIN32)
using Socket = SOCKET;
using SocketLength = int;
constexpr Socket invalid_socket = INVALID_SOCKET;
int socket_error() {
    return WSAGetLastError();
}
bool would_block(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
}
void release_socket(Socket socket) {
    closesocket(socket);
}
struct SocketRuntime {
    bool ready{false};
    SocketRuntime() {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketRuntime() {
        if (ready) {
            WSACleanup();
        }
    }
};
bool initialize_sockets() {
    static SocketRuntime runtime;
    return runtime.ready;
}
#else
using Socket = int;
using SocketLength = socklen_t;
constexpr Socket invalid_socket = -1;
int socket_error() {
    return errno;
}
bool would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS || error == EINTR;
}
void release_socket(Socket socket) {
    ::close(socket);
}
bool initialize_sockets() {
    return true;
}
#endif

struct OwnedSocket {
    Socket value{invalid_socket};
    OwnedSocket() = default;
    explicit OwnedSocket(Socket socket) : value(socket) {}
    ~OwnedSocket() {
        close();
    }
    OwnedSocket(const OwnedSocket&) = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;
    OwnedSocket(OwnedSocket&& other) noexcept : value(std::exchange(other.value, invalid_socket)) {}
    OwnedSocket& operator=(OwnedSocket&& other) noexcept {
        if (this != &other) {
            close();
            value = std::exchange(other.value, invalid_socket);
        }
        return *this;
    }
    void close() {
        if (value != invalid_socket) {
            release_socket(std::exchange(value, invalid_socket));
        }
    }
};

bool socket_options(Socket socket) {
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        return false;
    }
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0 ||
        fcntl(socket, F_SETFD, FD_CLOEXEC) != 0) {
        return false;
    }
#endif
    const int enabled = 1;
    const int buffer_bytes = 32 * 1024;
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                      sizeof(enabled)) == 0 &&
           setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer_bytes),
                      sizeof(buffer_bytes)) == 0 &&
           setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer_bytes),
                      sizeof(buffer_bytes)) == 0;
}

Result<OwnedSocket> create_socket() {
    if (!initialize_sockets()) {
        return net_error(ErrorCode::io_failure, "net.socket", "Socket initialization failed");
    }
    OwnedSocket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket.value == invalid_socket || !socket_options(socket.value)) {
        return net_error(ErrorCode::io_failure, "net.socket", "Nonblocking socket creation failed");
    }
    return socket;
}

Result<void> validate_limits(const TransportLimits& limits) {
    if (limits.queue_frames == 0 || limits.queue_frames > 64 ||
        limits.queue_bytes < wire_header_bytes + wire_max_payload ||
        limits.queue_bytes > 256 * 1024 || limits.handshake_timeout <= MonotonicTime::zero() ||
        limits.handshake_timeout > std::chrono::seconds{60} ||
        limits.idle_timeout <= MonotonicTime::zero() ||
        limits.idle_timeout > std::chrono::seconds{300} ||
        limits.write_timeout <= MonotonicTime::zero() ||
        limits.write_timeout > std::chrono::seconds{60}) {
        return net_error(ErrorCode::invalid_configuration, "net.transport.limits",
                         "Transport limits exceed the supported bounded profile");
    }
    return {};
}

bool valid_pem(const std::string& pem) {
    return !pem.empty() && pem.size() <= 64 * 1024 && pem.find('\0') == std::string::npos;
}

struct Configuration {
    mbedtls_ssl_config tls;
    mbedtls_x509_crt certificates;
    mbedtls_x509_crt authorities;
    mbedtls_pk_context key;
    bool server{false};
    bool authenticated{false};
    std::size_t live_connections{0};
    Configuration() {
        mbedtls_ssl_config_init(&tls);
        mbedtls_x509_crt_init(&certificates);
        mbedtls_x509_crt_init(&authorities);
        mbedtls_pk_init(&key);
    }
    ~Configuration() {
        mbedtls_ssl_config_free(&tls);
        mbedtls_x509_crt_free(&certificates);
        mbedtls_x509_crt_free(&authorities);
        mbedtls_pk_free(&key);
    }
};

Result<std::shared_ptr<Configuration>> make_configuration(bool server, const std::string& ca,
                                                          const std::string& pem,
                                                          const std::string& key,
                                                          bool authenticated) {
    auto initialized = initialize_crypto();
    if (!initialized) {
        return initialized.error();
    }
    auto config = std::make_shared<Configuration>();
    config->server = server;
    config->authenticated = authenticated;
    if (mbedtls_ssl_config_defaults(
            &config->tls, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        (!ca.empty() && mbedtls_x509_crt_parse(&config->authorities,
                                               reinterpret_cast<const unsigned char*>(ca.c_str()),
                                               ca.size() + 1) != 0) ||
        (!pem.empty() && mbedtls_x509_crt_parse(&config->certificates,
                                                reinterpret_cast<const unsigned char*>(pem.c_str()),
                                                pem.size() + 1) != 0)) {
        return net_error(ErrorCode::invalid_configuration, "net.tls.configure",
                         "Invalid TLS configuration or certificate input");
    }
    mbedtls_ssl_conf_min_tls_version(&config->tls, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&config->tls, MBEDTLS_SSL_VERSION_TLS1_3);
    static constexpr int ciphersuites[]{MBEDTLS_TLS1_3_AES_128_GCM_SHA256, 0};
    static constexpr std::uint16_t groups[]{MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, 0};
    static constexpr std::uint16_t signatures[]{MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256, 0};
    mbedtls_ssl_conf_ciphersuites(&config->tls, ciphersuites);
    mbedtls_ssl_conf_groups(&config->tls, groups);
    mbedtls_ssl_conf_sig_algs(&config->tls, signatures);
    mbedtls_ssl_conf_tls13_key_exchange_modes(&config->tls,
                                              MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
    if (!pem.empty()) {
        if (mbedtls_pk_parse_key(&config->key, reinterpret_cast<const unsigned char*>(key.c_str()),
                                 key.size() + 1, nullptr, 0) != 0 ||
            mbedtls_pk_check_pair(&config->certificates.pk, &config->key) != 0 ||
            mbedtls_ssl_conf_own_cert(&config->tls, &config->certificates, &config->key) != 0) {
            return net_error(ErrorCode::invalid_configuration, "net.tls.identity",
                             "Invalid TLS certificate and private key pair");
        }
    }
    if (!ca.empty()) {
        mbedtls_ssl_conf_ca_chain(&config->tls, &config->authorities, nullptr);
    }
    mbedtls_ssl_conf_authmode(&config->tls, !server || authenticated ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                                     : MBEDTLS_SSL_VERIFY_NONE);
    if (server) {
        mbedtls_ssl_conf_new_session_tickets(&config->tls, 0);
    } else {
        mbedtls_ssl_conf_session_tickets(&config->tls, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
    }
    return config;
}

Result<CertificateFingerprint> fingerprint(const mbedtls_x509_crt& certificate) {
    CertificateFingerprint result{};
    std::size_t written = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, certificate.raw.p, certificate.raw.len,
                         reinterpret_cast<unsigned char*>(result.data()), result.size(),
                         &written) != PSA_SUCCESS ||
        written != result.size()) {
        return net_error(ErrorCode::execution_failed, "net.tls.fingerprint",
                         "Certificate fingerprint computation failed");
    }
    return result;
}

bool client_certificate_profile(const mbedtls_x509_crt& certificate) {
    if (mbedtls_x509_crt_get_ca_istrue(&certificate) != 0 ||
        !mbedtls_x509_crt_has_ext_type(&certificate, MBEDTLS_X509_EXT_KEY_USAGE) ||
        !mbedtls_x509_crt_has_ext_type(&certificate, MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE) ||
        mbedtls_x509_crt_check_key_usage(&certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0) {
        return false;
    }
    // Missing EKU and anyExtendedKeyUsage are deliberately insufficient.
    for (auto* usage = &certificate.ext_key_usage; usage != nullptr; usage = usage->next) {
        if (usage->buf.len == MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH) &&
            std::memcmp(usage->buf.p, MBEDTLS_OID_CLIENT_AUTH, usage->buf.len) == 0) {
            return true;
        }
    }
    return false;
}

void add_bytes(std::uint64_t& destination, std::size_t amount) {
    const auto increment = static_cast<std::uint64_t>(amount);
    destination += std::min(increment, std::numeric_limits<std::uint64_t>::max() - destination);
}

} // namespace

Result<CertificateFingerprint> certificate_fingerprint(const std::string& certificate_pem) {
    if (!valid_pem(certificate_pem)) {
        return net_error(ErrorCode::invalid_argument, "net.tls.fingerprint",
                         "Invalid bounded certificate input");
    }
    std::lock_guard lock(crypto().mutex);
    if (auto initialized = initialize_crypto(); !initialized) {
        return initialized.error();
    }
    Configuration temporary;
    if (mbedtls_x509_crt_parse(&temporary.certificates,
                               reinterpret_cast<const unsigned char*>(certificate_pem.c_str()),
                               certificate_pem.size() + 1) != 0 ||
        temporary.certificates.next != nullptr) {
        return net_error(ErrorCode::invalid_argument, "net.tls.fingerprint",
                         "Enrollment requires exactly one certificate");
    }
    return fingerprint(temporary.certificates);
}

struct TlsTransport::Impl {
    struct Pending {
        std::vector<std::byte> bytes;
        std::size_t offset{0};
        MonotonicTime admitted{};
    };
    std::thread::id owner{std::this_thread::get_id()};
    OwnedSocket socket;
    std::shared_ptr<Configuration> config;
    mbedtls_ssl_context tls;
    TransportLimits limits;
    TransportStats counters;
    MonotonicTime started{};
    MonotonicTime now{};
    MonotonicTime last_io{};
    std::uint64_t send_sequence{0};
    std::uint64_t receive_sequence{0};
    std::deque<Pending> outgoing;
    std::deque<Frame> incoming;
    FrameDecoder decoder;
    std::array<std::byte, wire_header_bytes + wire_max_payload> read_buffer{};
    std::size_t read_size{0};
    std::size_t read_offset{0};
    bool counted{false};
    bool clean_peer_closed{false};
    std::uint64_t connection_id{0};
    std::optional<CertificateFingerprint> verified_client{};
    mbedtls_x509_time client_valid_from{};
    mbedtls_x509_time client_valid_to{};

    Impl() {
        mbedtls_ssl_init(&tls);
    }
    ~Impl() {
        mbedtls_ssl_free(&tls);
        release();
    }
    bool owned() const {
        return owner == std::this_thread::get_id();
    }
    void release() {
        socket.close();
        if (counted) {
            --config->live_connections;
            counted = false;
        }
    }
    void terminate(TransportState state) {
        clean_peer_closed = false;
        release();
        outgoing.clear();
        incoming.clear();
        counters.send_queue_bytes = 0;
        counters.receive_queue_bytes = 0;
        read_size = 0;
        read_offset = 0;
        counters.state = state;
    }
    Error fail(Error error) {
        terminate(TransportState::failed);
        return error;
    }
    TransportStats snapshot() const {
        auto result = counters;
        result.send_queue_frames = outgoing.size();
        result.receive_queue_frames = incoming.size();
        return result;
    }
    static int write_socket(void* context, const unsigned char* bytes, std::size_t count) {
        auto& self = *static_cast<Impl*>(context);
#if defined(_WIN32)
        const auto result = ::send(self.socket.value, reinterpret_cast<const char*>(bytes),
                                   static_cast<int>(count), 0);
#else
        const auto result = ::send(self.socket.value, bytes, count, MSG_NOSIGNAL);
#endif
        if (result < 0) {
            return would_block(socket_error()) ? MBEDTLS_ERR_SSL_WANT_WRITE
                                               : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }
        if (result == 0) {
            return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }
        add_bytes(self.counters.socket_bytes_sent, static_cast<std::size_t>(result));
        self.last_io = self.now;
        return static_cast<int>(result);
    }
    static int read_socket(void* context, unsigned char* bytes, std::size_t count) {
        auto& self = *static_cast<Impl*>(context);
#if defined(_WIN32)
        const auto result =
            ::recv(self.socket.value, reinterpret_cast<char*>(bytes), static_cast<int>(count), 0);
#else
        const auto result = ::recv(self.socket.value, bytes, count, 0);
#endif
        if (result < 0) {
            return would_block(socket_error()) ? MBEDTLS_ERR_SSL_WANT_READ
                                               : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        }
        if (result > 0) {
            add_bytes(self.counters.socket_bytes_received, static_cast<std::size_t>(result));
            self.last_io = self.now;
        }
        return static_cast<int>(result);
    }
    Result<void> setup() {
        if (crypto().last_connection_id == std::numeric_limits<std::uint64_t>::max()) {
            return net_error(ErrorCode::capability_unavailable, "net.tls.setup",
                             "Connection identity domain exhausted");
        }
        if (mbedtls_ssl_setup(&tls, &config->tls) != 0) {
            return net_error(ErrorCode::execution_failed, "net.tls.setup", "TLS setup failed");
        }
        mbedtls_ssl_set_bio(&tls, this, write_socket, read_socket, nullptr);
        connection_id = ++crypto().last_connection_id;
        ++config->live_connections;
        counted = true;
        return {};
    }
    Result<void> enqueue(MessageKind kind, std::span<const std::byte> payload) {
        if (payload.size() > wire_max_payload) {
            return net_error(ErrorCode::invalid_argument, "net.transport.send",
                             "Message payload exceeds its limit");
        }
        const auto size = wire_header_bytes + payload.size();
        if (outgoing.size() >= limits.queue_frames ||
            size > limits.queue_bytes - counters.send_queue_bytes) {
            return net_error(ErrorCode::conflict, "net.transport.send",
                             "Transport send queue is full");
        }
        auto sequence = next_sequence(send_sequence);
        if (!sequence) {
            return sequence.error();
        }
        auto encoded =
            encode_frame(Frame{kind, sequence.value(), {payload.begin(), payload.end()}});
        if (!encoded) {
            return encoded.error();
        }
        outgoing.push_back(Pending{std::move(encoded).value(), 0, now});
        counters.send_queue_bytes += size;
        send_sequence = sequence.value();
        return {};
    }
    Result<void> deliver(Frame frame) {
        auto checked = check_sequence(receive_sequence, frame.sequence);
        if (!checked) {
            return checked.error();
        }
        if (counters.state == TransportState::negotiating) {
            const auto expected = config->server ? MessageKind::hello : MessageKind::hello_ack;
            if (frame.kind != expected) {
                return net_error(ErrorCode::invalid_argument, "net.transport.negotiate",
                                 "Expected initial wire negotiation message");
            }
            if (config->server) {
                auto queued = enqueue(MessageKind::hello_ack, {});
                if (!queued) {
                    return queued.error();
                }
            }
            counters.state = TransportState::ready;
        } else {
            const auto expected =
                config->authenticated
                    ? (config->server ? MessageKind::session_request : MessageKind::session_reply)
                    : (config->server ? MessageKind::probe : MessageKind::probe_reply);
            const auto replication =
                config->server ? MessageKind::replication_control : MessageKind::replication_state;
            if (frame.kind != expected && !(config->authenticated && frame.kind == replication)) {
                return net_error(ErrorCode::invalid_argument, "net.transport.receive",
                                 "Message kind is unavailable for this connection role");
            }
            counters.receive_queue_bytes += wire_header_bytes + frame.payload.size();
            incoming.push_back(std::move(frame));
        }
        receive_sequence = checked.value();
        add_bytes(counters.frames_received, 1);
        return {};
    }
};

TlsTransport::TlsTransport(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TlsTransport::~TlsTransport() {
    std::lock_guard lock(crypto().mutex);
    impl_.reset();
}

Result<std::unique_ptr<TlsTransport>>
TlsTransport::connect_loopback(std::uint16_t port, const ClientTrust& trust, MonotonicTime now,
                               const TransportLimits& limits) {
    return connect_impl(port, trust, nullptr, now, limits);
}

Result<std::unique_ptr<TlsTransport>>
TlsTransport::connect_loopback_authenticated(std::uint16_t port, const ClientTrust& trust,
                                             const ClientIdentity& identity, MonotonicTime now,
                                             const TransportLimits& limits) {
    return connect_impl(port, trust, &identity, now, limits);
}

Result<std::unique_ptr<TlsTransport>> TlsTransport::connect_impl(std::uint16_t port,
                                                                 const ClientTrust& trust,
                                                                 const ClientIdentity* identity,
                                                                 MonotonicTime now,
                                                                 const TransportLimits& limits) {
    auto valid = validate_limits(limits);
    if (!valid) {
        return valid.error();
    }
    if (port == 0 || now < MonotonicTime::zero() || !valid_pem(trust.ca_pem) ||
        (identity != nullptr &&
         (!valid_pem(identity->certificate_pem) || !valid_pem(identity->private_key_pem))) ||
        trust.server_name.empty() || trust.server_name.size() > 253 ||
        !std::all_of(trust.server_name.begin(), trust.server_name.end(), [](char value) {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                   (value >= '0' && value <= '9') || value == '.' || value == '-';
        })) {
        return net_error(ErrorCode::invalid_configuration, "net.transport.connect",
                         "Invalid loopback endpoint, trust configuration or initial time");
    }
    std::lock_guard lock(crypto().mutex);
    auto config = make_configuration(
        false, trust.ca_pem, identity != nullptr ? identity->certificate_pem : "",
        identity != nullptr ? identity->private_key_pem : "", identity != nullptr);
    if (!config) {
        return config.error();
    }
    auto socket = create_socket();
    if (!socket) {
        return socket.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->socket = std::move(socket).value();
    impl->config = std::move(config).value();
    impl->limits = limits;
    impl->started = now;
    impl->now = now;
    impl->last_io = now;
    auto setup = impl->setup();
    if (!setup) {
        return setup.error();
    }
    if (mbedtls_ssl_set_hostname(&impl->tls, trust.server_name.c_str()) != 0) {
        return net_error(ErrorCode::invalid_configuration, "net.tls.hostname",
                         "Invalid server hostname");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int connected =
        ::connect(impl->socket.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected == 0) {
        impl->counters.state = TransportState::handshaking;
    } else if (!would_block(socket_error())) {
        return net_error(ErrorCode::io_failure, "net.transport.connect",
                         "Loopback connection failed");
    }
    return std::unique_ptr<TlsTransport>{new TlsTransport{std::move(impl)}};
}

Result<TransportStats> TlsTransport::poll(MonotonicTime now, std::size_t budget) {
    auto& self = *impl_;
    if (!self.owned()) {
        return net_error(ErrorCode::conflict, "net.transport.poll",
                         "Transport has another owner thread");
    }
    if (now < self.now || now < MonotonicTime::zero() || budget == 0 || budget > 64) {
        return net_error(ErrorCode::invalid_argument, "net.transport.poll",
                         "Invalid monotonic time or poll budget");
    }
    std::lock_guard lock(crypto().mutex);
    self.now = now;
    self.counters.operations = 0;
    if (self.counters.state == TransportState::closed ||
        self.counters.state == TransportState::failed) {
        return net_error(ErrorCode::conflict, "net.transport.poll",
                         "Transport is closed or failed");
    }
    if ((self.counters.state != TransportState::ready &&
         now - self.started >= self.limits.handshake_timeout) ||
        now - self.last_io >= self.limits.idle_timeout ||
        (!self.outgoing.empty() &&
         now - self.outgoing.front().admitted >= self.limits.write_timeout)) {
        return self.fail(net_error(ErrorCode::io_failure, "net.transport.timeout",
                                   "Transport deadline expired"));
    }
    while (self.counters.operations < budget) {
        if (self.counters.state == TransportState::connecting) {
            ++self.counters.operations;
            fd_set writable;
            fd_set exceptional;
            FD_ZERO(&writable);
            FD_ZERO(&exceptional);
#if !defined(_WIN32)
            if (self.socket.value >= FD_SETSIZE) {
                return self.fail(net_error(ErrorCode::io_failure, "net.transport.connect",
                                           "Socket exceeds polling descriptor limit"));
            }
#endif
            FD_SET(self.socket.value, &writable);
            FD_SET(self.socket.value, &exceptional);
            timeval timeout{};
#if defined(_WIN32)
            const int selected = select(0, nullptr, &writable, &exceptional, &timeout);
#else
            const int selected =
                select(self.socket.value + 1, nullptr, &writable, &exceptional, &timeout);
#endif
            if (selected == 0) {
                break;
            }
            int error = 0;
            SocketLength size = sizeof(error);
            if (selected < 0 ||
                getsockopt(self.socket.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error),
                           &size) != 0 ||
                error != 0) {
                return self.fail(net_error(ErrorCode::io_failure, "net.transport.connect",
                                           "Loopback connection failed"));
            }
            self.counters.state = TransportState::handshaking;
            continue;
        }
        if (self.counters.state == TransportState::handshaking) {
            ++self.counters.operations;
            const int result = mbedtls_ssl_handshake_step(&self.tls);
            if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                break;
            }
            if (result != 0) {
                return self.fail(tls_error("net.tls.handshake",
                                           "TLS handshake or certificate verification failed",
                                           result));
            }
            if (mbedtls_ssl_is_handshake_over(&self.tls)) {
                if ((!self.config->server || self.config->authenticated) &&
                    mbedtls_ssl_get_verify_result(&self.tls) != 0) {
                    return self.fail(net_error(ErrorCode::io_failure, "net.tls.verify",
                                               "Peer certificate verification failed"));
                }
                if (self.config->server && self.config->authenticated) {
                    const auto* peer = mbedtls_ssl_get_peer_cert(&self.tls);
                    if (peer == nullptr || !client_certificate_profile(*peer)) {
                        return self.fail(net_error(ErrorCode::io_failure, "net.tls.client-profile",
                                                   "Client certificate profile rejected"));
                    }
                    auto digest = fingerprint(*peer);
                    if (!digest) {
                        return self.fail(digest.error());
                    }
                    self.verified_client = digest.value();
                    self.client_valid_from = peer->valid_from;
                    self.client_valid_to = peer->valid_to;
                }
                self.counters.state = TransportState::negotiating;
                if (!self.config->server) {
                    auto sent = self.enqueue(MessageKind::hello, {});
                    if (!sent) {
                        return self.fail(sent.error());
                    }
                }
            }
            continue;
        }
        const bool receive_room = self.incoming.size() < self.limits.queue_frames &&
                                  self.limits.queue_bytes - self.counters.receive_queue_bytes >=
                                      wire_header_bytes + wire_max_payload;
        if (self.read_offset < self.read_size && receive_room) {
            ++self.counters.operations;
            auto decoded = self.decoder.consume(std::span{self.read_buffer}.subspan(
                self.read_offset, self.read_size - self.read_offset));
            if (!decoded) {
                return self.fail(decoded.error());
            }
            self.read_offset += decoded.value().consumed;
            if (decoded.value().frame) {
                auto delivered = self.deliver(std::move(*decoded.value().frame));
                if (!delivered) {
                    return self.fail(delivered.error());
                }
            }
            continue;
        }
        bool progress = false;
        if (!self.outgoing.empty()) {
            ++self.counters.operations;
            auto& pending = self.outgoing.front();
            const int result = mbedtls_ssl_write(
                &self.tls,
                reinterpret_cast<const unsigned char*>(pending.bytes.data() + pending.offset),
                pending.bytes.size() - pending.offset);
            if (result > 0) {
                pending.offset += static_cast<std::size_t>(result);
                progress = true;
                if (pending.offset == pending.bytes.size()) {
                    self.counters.send_queue_bytes -= pending.bytes.size();
                    self.outgoing.pop_front();
                    add_bytes(self.counters.frames_sent, 1);
                }
            } else if (result != MBEDTLS_ERR_SSL_WANT_WRITE &&
                       result != MBEDTLS_ERR_SSL_WANT_READ) {
                return self.fail(tls_error("net.tls.write", "TLS write failed", result));
            }
        }
        if (self.counters.operations < budget && receive_room &&
            self.read_offset == self.read_size) {
            ++self.counters.operations;
            const int result = mbedtls_ssl_read(
                &self.tls, reinterpret_cast<unsigned char*>(self.read_buffer.data()),
                self.read_buffer.size());
            if (result > 0) {
                self.read_offset = 0;
                self.read_size = static_cast<std::size_t>(result);
                progress = true;
            } else if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                // TLS 1.3 peers may issue tickets even when this client never
                // resumes sessions. Discard the notification, charging this
                // read against the poll budget; never export or reuse a ticket.
                progress = true;
            } else if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                auto finished = self.decoder.finish();
                if (!finished) {
                    return self.fail(finished.error());
                }
                // A complete authenticated stream may end in the same poll as
                // its final frames. Preserve those owned values for the caller.
                self.release();
                self.outgoing.clear();
                self.counters.send_queue_bytes = 0;
                self.counters.state = TransportState::closed;
                self.clean_peer_closed = true;
                break;
            } else if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                       result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return self.fail(tls_error(
                    "net.tls.read",
                    "TLS read failed or peer disconnected without close notification", result));
            }
        }
        if (!progress) {
            break;
        }
    }
    return self.snapshot();
}

Result<void> TlsTransport::send(MessageKind kind, std::span<const std::byte> payload) {
    auto& self = *impl_;
    if (!self.owned()) {
        return net_error(ErrorCode::conflict, "net.transport.send",
                         "Transport has another owner thread");
    }
    if (self.counters.state != TransportState::ready) {
        return net_error(ErrorCode::conflict, "net.transport.send",
                         "Wire negotiation is not ready");
    }
    const auto expected =
        self.config->authenticated
            ? (self.config->server ? MessageKind::session_reply : MessageKind::session_request)
            : (self.config->server ? MessageKind::probe_reply : MessageKind::probe);
    const auto replication =
        self.config->server ? MessageKind::replication_state : MessageKind::replication_control;
    if (kind != expected && !(self.config->authenticated && kind == replication)) {
        return net_error(ErrorCode::invalid_argument, "net.transport.send",
                         "Message kind is unavailable for this connection role");
    }
    return self.enqueue(kind, payload);
}

Result<std::optional<Frame>> TlsTransport::receive() {
    auto& self = *impl_;
    if (!self.owned()) {
        return net_error(ErrorCode::conflict, "net.transport.receive",
                         "Transport has another owner thread");
    }
    if (self.counters.state == TransportState::failed ||
        (self.counters.state == TransportState::closed && !self.clean_peer_closed)) {
        return net_error(ErrorCode::conflict, "net.transport.receive",
                         "Transport is closed or failed");
    }
    if (self.incoming.empty()) {
        return std::optional<Frame>{};
    }
    Frame frame = std::move(self.incoming.front());
    self.incoming.pop_front();
    self.counters.receive_queue_bytes -= wire_header_bytes + frame.payload.size();
    return std::optional<Frame>{std::move(frame)};
}

Result<TransportStats> TlsTransport::stats() const {
    if (!impl_->owned()) {
        return net_error(ErrorCode::conflict, "net.transport.stats",
                         "Transport has another owner thread");
    }
    return impl_->snapshot();
}

Result<PeerIdentity> TlsTransport::authenticated_peer() const {
    const auto& self = *impl_;
    if (!self.owned() || !self.config->server || !self.config->authenticated ||
        self.counters.state != TransportState::ready || !self.verified_client) {
        return net_error(ErrorCode::conflict, "net.tls.authenticated-peer",
                         "A live authenticated server-side transport is required");
    }
    std::lock_guard lock(crypto().mutex);
    if (mbedtls_x509_time_is_past(&self.client_valid_to) ||
        mbedtls_x509_time_is_future(&self.client_valid_from)) {
        return net_error(ErrorCode::conflict, "net.tls.authenticated-peer",
                         "Client certificate is outside its validity interval");
    }
    return PeerIdentity{self.connection_id, *self.verified_client};
}

Result<void> TlsTransport::close() {
    if (!impl_->owned()) {
        return net_error(ErrorCode::conflict, "net.transport.close",
                         "Transport has another owner thread");
    }
    std::lock_guard lock(crypto().mutex);
    impl_->terminate(TransportState::closed);
    return {};
}

struct TlsListener::Impl {
    std::thread::id owner{std::this_thread::get_id()};
    OwnedSocket socket;
    std::shared_ptr<Configuration> config;
    TransportLimits limits;
    std::uint16_t bound_port{0};
    std::size_t max_connections{16};
    MonotonicTime last_now{};
    bool owned() const {
        return owner == std::this_thread::get_id();
    }
};

TlsListener::TlsListener(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TlsListener::~TlsListener() {
    std::lock_guard lock(crypto().mutex);
    impl_.reset();
}

Result<std::unique_ptr<TlsListener>> TlsListener::listen_loopback(std::uint16_t port,
                                                                  const ServerIdentity& identity,
                                                                  const TransportLimits& limits,
                                                                  std::size_t max_connections) {
    return listen_impl(port, identity, nullptr, limits, max_connections);
}

Result<std::unique_ptr<TlsListener>> TlsListener::listen_loopback_authenticated(
    std::uint16_t port, const ServerIdentity& identity, const std::string& client_ca_pem,
    const TransportLimits& limits, std::size_t max_connections) {
    return listen_impl(port, identity, &client_ca_pem, limits, max_connections);
}

Result<std::unique_ptr<TlsListener>> TlsListener::listen_impl(std::uint16_t port,
                                                              const ServerIdentity& identity,
                                                              const std::string* client_ca_pem,
                                                              const TransportLimits& limits,
                                                              std::size_t max_connections) {
    auto valid = validate_limits(limits);
    if (!valid) {
        return valid.error();
    }
    if (!valid_pem(identity.certificate_pem) || !valid_pem(identity.private_key_pem) ||
        (client_ca_pem != nullptr && !valid_pem(*client_ca_pem)) || max_connections == 0 ||
        max_connections > 16) {
        return net_error(ErrorCode::invalid_configuration, "net.transport.listen",
                         "Invalid server identity or connection limit");
    }
    std::lock_guard lock(crypto().mutex);
    auto config = make_configuration(true, client_ca_pem != nullptr ? *client_ca_pem : "",
                                     identity.certificate_pem, identity.private_key_pem,
                                     client_ca_pem != nullptr);
    if (!config) {
        return config.error();
    }
    auto socket = create_socket();
    if (!socket) {
        return socket.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->socket = std::move(socket).value();
    impl->config = std::move(config).value();
    impl->limits = limits;
    impl->max_connections = max_connections;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(impl->socket.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
            0 ||
        ::listen(impl->socket.value, static_cast<int>(max_connections)) != 0) {
        return net_error(ErrorCode::io_failure, "net.transport.listen", "Loopback listen failed");
    }
    SocketLength size = sizeof(address);
    if (getsockname(impl->socket.value, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        return net_error(ErrorCode::io_failure, "net.transport.listen",
                         "Cannot inspect bound loopback port");
    }
    impl->bound_port = ntohs(address.sin_port);
    return std::unique_ptr<TlsListener>{new TlsListener{std::move(impl)}};
}

Result<std::uint16_t> TlsListener::port() const {
    if (!impl_->owned() || impl_->socket.value == invalid_socket) {
        return net_error(ErrorCode::conflict, "net.transport.port",
                         "Listener is closed or has another owner thread");
    }
    return impl_->bound_port;
}

Result<std::optional<std::unique_ptr<TlsTransport>>> TlsListener::accept(MonotonicTime now) {
    auto& self = *impl_;
    if (!self.owned() || self.socket.value == invalid_socket) {
        return net_error(ErrorCode::conflict, "net.transport.accept",
                         "Listener is closed or has another owner thread");
    }
    if (now < MonotonicTime::zero() || now < self.last_now) {
        return net_error(ErrorCode::invalid_argument, "net.transport.accept",
                         "Listener time must be monotonic and nonnegative");
    }
    std::lock_guard lock(crypto().mutex);
    self.last_now = now;
    if (self.config->live_connections >= self.max_connections) {
        return net_error(ErrorCode::conflict, "net.transport.accept",
                         "Listener connection limit reached");
    }
    sockaddr_in address{};
    SocketLength size = sizeof(address);
    OwnedSocket socket{::accept(self.socket.value, reinterpret_cast<sockaddr*>(&address), &size)};
    if (socket.value == invalid_socket) {
        if (would_block(socket_error())) {
            return std::optional<std::unique_ptr<TlsTransport>>{};
        }
        return net_error(ErrorCode::io_failure, "net.transport.accept", "Loopback accept failed");
    }
    if (address.sin_family != AF_INET || address.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
        !socket_options(socket.value)) {
        return net_error(ErrorCode::io_failure, "net.transport.accept",
                         "Accepted socket is outside the loopback profile");
    }
    auto impl = std::make_unique<TlsTransport::Impl>();
    impl->socket = std::move(socket);
    impl->config = self.config;
    impl->limits = self.limits;
    impl->started = now;
    impl->now = now;
    impl->last_io = now;
    impl->counters.state = TransportState::handshaking;
    auto setup = impl->setup();
    if (!setup) {
        return setup.error();
    }
    return std::optional<std::unique_ptr<TlsTransport>>{
        std::unique_ptr<TlsTransport>{new TlsTransport{std::move(impl)}}};
}

Result<void> TlsListener::close() {
    if (!impl_->owned()) {
        return net_error(ErrorCode::conflict, "net.transport.close-listener",
                         "Listener has another owner thread");
    }
    impl_->socket.close();
    return {};
}

} // namespace shark::net
