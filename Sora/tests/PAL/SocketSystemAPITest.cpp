/**
 * @file SocketSystemAPITest.cpp
 * @brief Verify TCP and UDP loopback semantics through dynamically resolved native socket tables.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/SystemAPI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstring>
#include <string_view>

namespace PAL = Sora::PAL;

namespace {

#if defined(PLATFORM_WINDOWS)
    using NativeSocket = PAL::WindowsSystem::Socket;
    using PollDescriptor = PAL::WindowsSystem::PollDescriptor;
    using SocketLength = int;
    inline constexpr NativeSocket kInvalidSocket = static_cast<NativeSocket>(-1);
#else
    using NativeSocket = PAL::PosixSystem::Socket;
    using PollDescriptor = PAL::PosixSystem::PollDescriptor;
    using SocketLength = PAL::PosixSystem::SocketLength;
    inline constexpr NativeSocket kInvalidSocket = -1;
#endif

    struct IPv4Address {
#if defined(PLATFORM_MACOS)
        uint8_t length = sizeof(IPv4Address);
        uint8_t family = static_cast<uint8_t>(PAL::SocketSystemAPI::Family::IPv4);
#else
        uint16_t family = static_cast<uint16_t>(PAL::SocketSystemAPI::Family::IPv4);
#endif
        uint16_t port = 0;
        uint32_t address = 0;
        std::array<std::byte, 8> padding{};
    };
    static_assert(sizeof(IPv4Address) == 16);

    class SocketRuntime {
    public:
        explicit SocketRuntime(const PAL::SocketSystemAPI& api) : api_{api} {
#if defined(PLATFORM_WINDOWS)
            PAL::WindowsSystem::SocketRuntimeDataStorage runtimeData{};
            initialized_ = api_.startup(0x0202, reinterpret_cast<WSAData*>(&runtimeData)) == 0;
#else
            initialized_ = true;
#endif
        }

        ~SocketRuntime() {
#if defined(PLATFORM_WINDOWS)
            if (initialized_) {
                api_.cleanup();
            }
#endif
        }

        [[nodiscard]] bool Initialized() const noexcept { return initialized_; }

    private:
        const PAL::SocketSystemAPI& api_;
        bool initialized_ = false;
    };

    class SocketOwner {
    public:
        SocketOwner(const PAL::SocketSystemAPI& api, NativeSocket socket) : api_{&api}, socket_{socket} {}
        SocketOwner(const SocketOwner&) = delete;
        SocketOwner& operator=(const SocketOwner&) = delete;
        SocketOwner(SocketOwner&&) = delete;
        SocketOwner& operator=(SocketOwner&&) = delete;
        ~SocketOwner() {
            if (socket_ != kInvalidSocket) {
                api_->close(socket_);
            }
        }

        [[nodiscard]] NativeSocket Get() const noexcept { return socket_; }

    private:
        const PAL::SocketSystemAPI* api_;
        NativeSocket socket_;
    };

    class AddressInfoOwner {
    public:
        AddressInfoOwner(const PAL::SocketSystemAPI& api, addrinfo* head) : api_{api}, head_{head} {}
        AddressInfoOwner(const AddressInfoOwner&) = delete;
        AddressInfoOwner& operator=(const AddressInfoOwner&) = delete;
        ~AddressInfoOwner() { api_.freeAddressInfo(head_); }

    private:
        const PAL::SocketSystemAPI& api_;
        addrinfo* head_;
    };

    [[nodiscard]] IPv4Address LoopbackAddress(uint16_t networkPort = 0) noexcept {
        IPv4Address address{};
        address.port = networkPort;
        constexpr uint32_t loopback = 0x7f000001;
        address.address = std::endian::native == std::endian::little ? std::byteswap(loopback) : loopback;
        return address;
    }

} // namespace

TEST_CASE("TCP system API transfers an ordered loopback byte stream", "[Sora.PAL.SystemAPI][TCP]") {
    const PAL::SocketSystemAPI& socket = PAL::LoadSocketSystemAPI();
    const PAL::TcpSystemAPI& tcp = PAL::LoadTcpSystemAPI();
    SocketRuntime runtime{socket};
    REQUIRE(runtime.Initialized());

    SocketOwner listener{socket, socket.create(static_cast<int>(PAL::SocketSystemAPI::Family::IPv4),
                                                static_cast<int>(PAL::SocketSystemAPI::Type::Stream),
                                                static_cast<int>(PAL::SocketSystemAPI::Protocol::Tcp))};
    REQUIRE(listener.Get() != kInvalidSocket);
    int reuseAddress = 1;
    REQUIRE(socket.setOption(listener.Get(), PAL::SocketSystemAPI::kOptionSocket,
                             PAL::SocketSystemAPI::kOptionReuseAddress,
                             reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress)) == 0);
    int queriedReuseAddress = 0;
    SocketLength optionSize = sizeof(queriedReuseAddress);
    REQUIRE(socket.getOption(listener.Get(), PAL::SocketSystemAPI::kOptionSocket,
                             PAL::SocketSystemAPI::kOptionReuseAddress,
                             reinterpret_cast<char*>(&queriedReuseAddress), &optionSize) == 0);
    REQUIRE(queriedReuseAddress != 0);
    IPv4Address endpoint = LoopbackAddress();
    REQUIRE(socket.bind(listener.Get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
    REQUIRE(tcp.listen(listener.Get(), 1) == 0);
    SocketLength endpointSize = sizeof(endpoint);
    REQUIRE(socket.getSocketName(listener.Get(), reinterpret_cast<sockaddr*>(&endpoint), &endpointSize) == 0);
    REQUIRE(endpoint.port != 0);

    SocketOwner client{socket, socket.create(static_cast<int>(PAL::SocketSystemAPI::Family::IPv4),
                                              static_cast<int>(PAL::SocketSystemAPI::Type::Stream),
                                              static_cast<int>(PAL::SocketSystemAPI::Protocol::Tcp))};
    REQUIRE(client.Get() != kInvalidSocket);
    REQUIRE(tcp.connect(client.Get(), reinterpret_cast<const sockaddr*>(&endpoint), endpointSize) == 0);
    IPv4Address peer{};
    SocketLength peerSize = sizeof(peer);
    REQUIRE(socket.getPeerName(client.Get(), reinterpret_cast<sockaddr*>(&peer), &peerSize) == 0);
    REQUIRE(peer.port == endpoint.port);
    SocketOwner server{socket, tcp.accept(listener.Get(), nullptr, nullptr)};
    REQUIRE(server.Get() != kInvalidSocket);

    constexpr std::string_view payload = "Sora TCP loopback";
    REQUIRE(tcp.send(client.Get(), payload.data(), payload.size(), 0) ==
            static_cast<decltype(payload.size())>(payload.size()));
    std::array<char, 64> received{};
    const auto bytes = tcp.receive(server.Get(), received.data(), received.size(), 0);
    REQUIRE(bytes == static_cast<decltype(bytes)>(payload.size()));
    REQUIRE(std::string_view{received.data(), static_cast<size_t>(bytes)} == payload);
    REQUIRE(tcp.shutdown(client.Get(), static_cast<int>(PAL::SocketSystemAPI::Shutdown::Both)) == 0);
}

TEST_CASE("Socket system API resolves and formats numeric endpoints", "[Sora.PAL.SystemAPI][Address]") {
    const PAL::SocketSystemAPI& socket = PAL::LoadSocketSystemAPI();
    SocketRuntime runtime{socket};
    REQUIRE(runtime.Initialized());

    addrinfo* resolved = nullptr;
    REQUIRE(socket.getAddressInfo("127.0.0.1", "443", nullptr, &resolved) == 0);
    REQUIRE(resolved != nullptr);
    const AddressInfoOwner addresses{socket, resolved};
    const IPv4Address endpoint = LoopbackAddress(std::endian::native == std::endian::little
                                                     ? std::byteswap(uint16_t{443})
                                                     : uint16_t{443});
    std::array<char, 64> host{};
    std::array<char, 16> service{};
    REQUIRE(socket.getNameInfo(reinterpret_cast<const sockaddr*>(&endpoint),
                               static_cast<SocketLength>(sizeof(endpoint)), host.data(), host.size(), service.data(),
                               service.size(),
                               PAL::SocketSystemAPI::kNameInfoNumericHost |
                                   PAL::SocketSystemAPI::kNameInfoNumericService) == 0);
    REQUIRE(std::string_view{host.data()} == "127.0.0.1");
    REQUIRE(std::string_view{service.data()} == "443");

#if defined(PLATFORM_WINDOWS)
    const int previousError = socket.getLastError();
    socket.setLastError(PAL::SocketSystemAPI::kInvalidArgumentError);
    REQUIRE(socket.getLastError() == PAL::SocketSystemAPI::kInvalidArgumentError);
    socket.setLastError(previousError);
#endif
}

TEST_CASE("UDP system API preserves one loopback datagram boundary", "[Sora.PAL.SystemAPI][UDP]") {
    const PAL::SocketSystemAPI& socket = PAL::LoadSocketSystemAPI();
    const PAL::UdpSystemAPI& udp = PAL::LoadUdpSystemAPI();
    SocketRuntime runtime{socket};
    REQUIRE(runtime.Initialized());

    SocketOwner receiver{socket, socket.create(static_cast<int>(PAL::SocketSystemAPI::Family::IPv4),
                                                static_cast<int>(PAL::SocketSystemAPI::Type::Datagram),
                                                static_cast<int>(PAL::SocketSystemAPI::Protocol::Udp))};
    SocketOwner sender{socket, socket.create(static_cast<int>(PAL::SocketSystemAPI::Family::IPv4),
                                              static_cast<int>(PAL::SocketSystemAPI::Type::Datagram),
                                              static_cast<int>(PAL::SocketSystemAPI::Protocol::Udp))};
    REQUIRE(receiver.Get() != kInvalidSocket);
    REQUIRE(sender.Get() != kInvalidSocket);
    IPv4Address endpoint = LoopbackAddress();
    REQUIRE(socket.bind(receiver.Get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
    SocketLength endpointSize = sizeof(endpoint);
    REQUIRE(socket.getSocketName(receiver.Get(), reinterpret_cast<sockaddr*>(&endpoint), &endpointSize) == 0);

    constexpr std::string_view payload = "Sora UDP loopback";
    REQUIRE(udp.sendTo(sender.Get(), payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr*>(&endpoint),
                       endpointSize) == static_cast<decltype(payload.size())>(payload.size()));
    PollDescriptor readiness{.descriptor = receiver.Get(), .events = PAL::SocketSystemAPI::kPollReadable};
    REQUIRE(socket.poll(&readiness, 1, 1000) == 1);
    REQUIRE((readiness.revents & PAL::SocketSystemAPI::kPollReadable) != 0);

    std::array<char, 64> received{};
    IPv4Address source{};
    SocketLength sourceSize = sizeof(source);
    const auto bytes = udp.receiveFrom(receiver.Get(), received.data(), received.size(), 0,
                                       reinterpret_cast<sockaddr*>(&source), &sourceSize);
    REQUIRE(bytes == static_cast<decltype(bytes)>(payload.size()));
    REQUIRE(std::string_view{received.data(), static_cast<size_t>(bytes)} == payload);
    REQUIRE(source.family == static_cast<decltype(source.family)>(PAL::SocketSystemAPI::Family::IPv4));
}
