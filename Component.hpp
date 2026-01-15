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
    static constexpr int BLS_PORT = 4000;
    using json = nlohmann::ordered_json;

    // -----------------------------------------------------------------------------
    // Component (UDP control + belief commit capable)
    // -----------------------------------------------------------------------------
    template <typename Derived>
    class Component
    {
    public:
        explicit Component(int sba)
            : sba_(sba),
              running_(true),
              udp_fd_(-1)
        {
            setup_udp();
        }

        virtual ~Component()
        {
            running_ = false;
            if (udp_fd_ >= 0)
                close(udp_fd_);
        }

        void run()
        {
            std::cout << "[MPP] running " << component_name()
                      << " on sba=" << sba_ << std::endl;

            while (running_)
            {
                poll_socket();
                usleep(1000);
            }
        }

    protected:
        // ---- identity ----
        virtual const char* component_name() const = 0;

        // ---- belief commit (write-only to BLS) ----
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
            for (const auto& b : committed_) {
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
        bool send_json(const json& j, int port)
        {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(port);
            dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            const std::string payload = j.dump() + "\n";

            const ssize_t sent = sendto(
                udp_fd_,
                payload.data(),
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
        std::atomic<bool> running_;
        std::vector<mpp::Belief> committed_;

        sockaddr_in last_sender_{};
        bool has_sender_ = false;

    private:
        int udp_fd_;

        void setup_udp()
        {
            udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp_fd_ < 0) {
                running_ = false;
                return;
            }

            int yes = 1;
            setsockopt(udp_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            fcntl(udp_fd_, F_SETFL, O_NONBLOCK);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(sba_);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);

            if (bind(udp_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
                close(udp_fd_);
                udp_fd_ = -1;
                running_ = false;
            }
        }

        void poll_socket()
        {
            char buffer[65536]{};
            sockaddr_in sender{};
            socklen_t sender_len = sizeof(sender);

            ssize_t len = recvfrom(
                udp_fd_,
                buffer,
                sizeof(buffer) - 1,
                0,
                (sockaddr*)&sender,
                &sender_len
            );

            if (len <= 0)
                return;

            last_sender_ = sender;
            has_sender_ = true;

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
