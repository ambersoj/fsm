#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <nlohmann/json.hpp>

namespace mpp
{

    static constexpr int BUS_PORT = 3999;
    using json = nlohmann::ordered_json;

    // -----------------------------------------------------------------------------
    // Component (vNext, BUS-enabled)
    // -----------------------------------------------------------------------------
    template <typename Derived>
    class Component
    {
    public:
        explicit Component(int sba,
                           uint64_t publish_period_ms = 0,
                           bool listen_bus = false)
            : sba_(sba),
              publish_period_ms_(publish_period_ms),
              listen_bus_(listen_bus),
              running_(true),
              udp_fd_(-1),
              bus_fd_(-1),
              last_publish_ts_(now_ms())
        {
            setup_udp();
            if (listen_bus_)
                setup_bus();
        }

        virtual ~Component()
        {
            running_ = false;
            if (udp_fd_ >= 0)
                close(udp_fd_);
            if (bus_fd_ >= 0)
                close(bus_fd_);
        }

        void run()
        {
            std::cout << "[MPP] running on sba=" << sba_;
            if (listen_bus_)
                std::cout << " (listening BUS)";
            std::cout << std::endl;

            while (running_)
            {
                poll_socket(udp_fd_, /*allow_reply=*/true);
                if (bus_fd_ >= 0)
                    poll_socket(bus_fd_, /*allow_reply=*/false);

                maybe_publish();
                usleep(1000); // 1ms idle
            }
        }

    protected:
        bool send_bus(const json &j)
        {
            return send_json(j, BUS_PORT);
        }

        void publish_snapshot()
        {
            // Default NO-OP
        }

        static uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(
                       steady_clock::now().time_since_epoch())
                .count();
        }

        void maybe_publish()
        {
            if (publish_period_ms_ == 0)
                return;

            uint64_t now = now_ms();
            if (now - last_publish_ts_ >= publish_period_ms_)
            {
                static_cast<Derived *>(this)->publish_snapshot();
                last_publish_ts_ = now;
            }
        }

        bool send_json(const json &j, const sockaddr_in &dest)
        {
            std::string payload = j.dump();
            payload.push_back('\n');

            return sendto(
                       udp_fd_,
                       payload.c_str(),
                       payload.size(),
                       0,
                       (sockaddr *)&dest,
                       sizeof(dest)) >= 0;
        }

        bool send_json(const json &j, int port)
        {
            if (port <= 0)
                return false;

            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(port);
            dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            return send_json(j, dest);
        }

    protected:
        int sba_;
        uint64_t publish_period_ms_;
        bool listen_bus_;
        std::atomic<bool> running_;

    private:
        int udp_fd_;
        int bus_fd_;
        uint64_t last_publish_ts_;

        void setup_udp()
        {
            udp_fd_ = make_socket(sba_);
            if (udp_fd_ < 0)
                running_ = false;
        }

        void setup_bus()
        {
            bus_fd_ = make_socket(BUS_PORT);
            if (bus_fd_ < 0)
            {
                std::cerr << "[MPP] failed to bind BUS port\n";
                running_ = false;
            }
        }

        int make_socket(int port)
        {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) {
            perror("socket");
            return -1;
            }

            int yes = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        #ifdef SO_REUSEPORT
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
        #endif

            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);

            if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
                perror("bind");
                close(fd);
                return -1;
            }

            return fd;
        }

        void poll_socket(int fd, bool allow_reply)
        {
            char buffer[65536]{};
            sockaddr_in sender{};
            socklen_t sender_len = sizeof(sender);

            ssize_t len = recvfrom(
                fd,
                buffer,
                sizeof(buffer) - 1,
                0,
                (sockaddr *)&sender,
                &sender_len);

            if (len <= 0)
                return;

            std::string payload(buffer, buffer + len);

            json j;
            try
            {
                j = json::parse(payload);
            }
            catch (const json::parse_error &e)
            {
                static_cast<Derived *>(this)->on_parse_error(e);
                return;
            }
            catch (...)
            {
                static_cast<Derived *>(this)->on_unknown_parse_error();
                return;
            }

            if (allow_reply &&
                j.contains("read") &&
                j["read"].is_boolean() &&
                j["read"])
            {
                json out = static_cast<Derived *>(this)->serialize_registers();
                send_json(out, sender);
                return;
            }

            static_cast<Derived *>(this)->apply_snapshot(j);
            static_cast<Derived *>(this)->on_message(j);
        }
    };

// -----------------------------------------------------------------------------
// One-line main()
// -----------------------------------------------------------------------------
#define MPP_MAIN(ComponentType)                 \
    int main(int argc, char **argv)             \
    {                                           \
        if (argc < 2)                           \
        {                                       \
            std::cerr << "usage: " << argv[0]   \
                      << " <sba>" << std::endl; \
            return 1;                           \
        }                                       \
        int sba = std::stoi(argv[1]);           \
        ComponentType comp(sba);                \
        comp.run();                             \
        return 0;                               \
    }

} // namespace mpp
