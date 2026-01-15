// Component.hpp
#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <nlohmann/json.hpp>

#include "Belief.hpp"

namespace mpp
{
    static constexpr int BUS_PORT = 3999;
    static constexpr int BLS_PORT = 4000;
    using json = nlohmann::ordered_json;

    // -----------------------------------------------------------------------------
    // Component (BUS-enabled, belief-capable)
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
            if (udp_fd_ >= 0) close(udp_fd_);
            if (bus_fd_ >= 0) close(bus_fd_);
        }

        void run()
        {
            std::cout << "[MPP] running on sba=" << sba_;
            if (listen_bus_ && bus_fd_ >= 0) std::cout << " (listening BUS)";
            std::cout << std::endl;

            while (running_)
            {
                poll_socket(udp_fd_, true);
                if (bus_fd_ >= 0)
                    poll_socket(bus_fd_, false);

                maybe_publish();
                usleep(1000);
            }
        }

    protected:
        // ---- identity ----
        virtual const char* component_name() const = 0;

        // ---- belief commit ----
        void commit(const char* subject,
                    bool polarity,
                    const json& context = json::object())
        {
            const std::string full_subject(subject);
            const std::string prefix =
                std::string(component_name()) + ".";

            // Enforce ownership
            if (full_subject.rfind(prefix, 0) != 0)
                return;

            // Enforce monotonicity
            for (const auto& b : committed_)
            {
                if (b.subject == full_subject &&
                    b.polarity == polarity)
                    return;
            }

            mpp::Belief belief{
                component_name(),
                full_subject,
                polarity,
                context
            };

            committed_.push_back(belief);

            json msg;
            msg["belief"] = {
                {"component", belief.component},
                {"subject",   belief.subject},
                {"polarity",  belief.polarity},
                {"context",   belief.context}
            };

            send_json(msg, BLS_PORT);
        }

        // ---- networking helpers ----
        bool send_bus(const json& j)
        {
            return send_json(j, BUS_PORT);
        }

        virtual void publish_snapshot() {}

        static uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();
        }

        void maybe_publish()
        {
            if (publish_period_ms_ == 0) return;

            uint64_t now = now_ms();
            if (now - last_publish_ts_ >= publish_period_ms_)
            {
                static_cast<Derived*>(this)->publish_snapshot();
                last_publish_ts_ = now;
            }
        }

        bool send_json(const json& j, int port)
        {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(port);
            dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            const std::string payload = j.dump() + "\n";

            const ssize_t sent = sendto(
                udp_fd_,
                payload.data(),     // <-- use data(), not c_str()
                payload.size(),
                0,
                (sockaddr*)&dest,
                sizeof(dest)
            );

            return sent == static_cast<ssize_t>(payload.size());
        }

        bool reply_json(const json& j)
        {
            if (!has_sender_)
                return false;

            const std::string payload = j.dump() + "\n";

            const ssize_t sent = sendto(
                udp_fd_,
                payload.data(),
                payload.size(),
                0,
                (sockaddr*)&last_sender_,
                sizeof(last_sender_)
            );

            return sent == static_cast<ssize_t>(payload.size());
        }

    protected:
        int sba_;
        uint64_t publish_period_ms_;
        bool listen_bus_;
        std::atomic<bool> running_;
        std::vector<mpp::Belief> committed_;
        sockaddr_in last_sender_{};
        bool has_sender_ = false;

    private:
        int udp_fd_;
        int bus_fd_;
        uint64_t last_publish_ts_;

        // ---- socket plumbing ----
        void setup_udp()
        {
            udp_fd_ = make_socket(sba_);
            if (udp_fd_ < 0) running_ = false;
        }

        void setup_bus()
        {
            bus_fd_ = make_socket(BUS_PORT);
            if (bus_fd_ < 0)
            {
                std::cerr << "[MPP] failed to bind BUS\n";
                running_ = false;
            }
        }

        int make_socket(int port)
        {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return -1;

            int yes = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

            fcntl(fd, F_SETFL, O_NONBLOCK);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);

            if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
            {
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
                (sockaddr*)&sender,
                &sender_len
            );

            if (len <= 0) return;

            if (allow_reply) {
                last_sender_ = sender;
                has_sender_ = true;
            }

            json j;
            try {
                j = json::parse(buffer, buffer + len);
            } catch (...) {
                return;
            }

            static_cast<Derived*>(this)->apply_snapshot(j);
            static_cast<Derived*>(this)->on_message(j);
        }

    };

// -----------------------------------------------------------------------------
// One-line main()
// -----------------------------------------------------------------------------
#define MPP_MAIN(ComponentType)                     \
int main(int argc, char** argv)                     \
{                                                    \
    if (argc < 2) {                                 \
        std::cerr << "usage: " << argv[0]           \
                  << " <sba>" << std::endl;         \
        return 1;                                   \
    }                                                \
    int sba = std::stoi(argv[1]);                    \
    ComponentType comp(sba);                         \
    comp.run();                                      \
    return 0;                                        \
}

} // namespace mpp
