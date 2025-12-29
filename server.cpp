#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// nlohmann::json single-header library required:
// https://github.com/nlohmann/json (place json.hpp in include path)
#include "nlohmann/json.hpp"

using json = nlohmann::json;
using namespace std::chrono;

const char* HOST = "0.0.0.0";
const int PORT = 55555;

struct NodeInfo {
    sockaddr_in public_addr;
    std::vector<std::string> local_addr; // same shape as Python: [] or [ip, port]
    double last; // epoch seconds
};

std::unordered_map<std::string, NodeInfo> nodes;
std::mutex nodes_mutex;

int sockfd = -1;
bool running = true;

std::string sockaddr_to_ip(const sockaddr_in &sa) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

int sockaddr_to_port(const sockaddr_in &sa) {
    return ntohs(sa.sin_port);
}

void send_json(const sockaddr_in &addr, const json &obj) {
    try {
        std::string s = obj.dump();
        ssize_t sent = sendto(sockfd, s.c_str(), s.size(), 0,
                              reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (sent < 0) {
            std::cerr << "sendto error: " << strerror(errno) << "\n";
        }
    } catch (const std::exception &e) {
        std::cerr << "send error: " << e.what() << "\n";
    }
}

double now_seconds() {
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

void handle_message(const json &msg, const sockaddr_in &addr) {
    double now = now_seconds();
    std::string t;
    std::string nid;

    if (msg.contains("type") && msg["type"].is_string()) t = msg["type"].get<std::string>();
    if (msg.contains("node_id") && msg["node_id"].is_string()) nid = msg["node_id"].get<std::string>();

    // update registry if node_id present
    if (!nid.empty()) {
        NodeInfo ni;
        ni.public_addr = addr;
        ni.last = now;
        ni.local_addr.clear();
        if (msg.contains("local_addr") && msg["local_addr"].is_array()) {
            for (auto &el : msg["local_addr"]) {
                if (el.is_string()) ni.local_addr.push_back(el.get<std::string>());
                else if (el.is_number_integer()) ni.local_addr.push_back(std::to_string(el.get<int>()));
            }
        }
        std::lock_guard<std::mutex> g(nodes_mutex);
        nodes[nid] = std::move(ni);
    }

    char timebuf[9];
    time_t tt = time(nullptr);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", localtime(&tt));
    std::cout << "[" << timebuf << "] "
              << sockaddr_to_ip(addr) << ":" << sockaddr_to_port(addr)
              << " -> " << t << " (" << nid << ")\n";

    if (t == "stun") {
        json resp = {
            {"type", "stun_response"},
            {"node_id", nid},
            {"public_ip", sockaddr_to_ip(addr)},
            {"public_port", sockaddr_to_port(addr)}
        };
        send_json(addr, resp);
        return;
    }

    if (t == "register") {
        int active = 0;
        {
            std::lock_guard<std::mutex> g(nodes_mutex);
            active = std::max(0, (int)nodes.size() - 1);
        }
        json resp = {{"type", "register_response"}, {"status", "ok"}, {"active", active}};
        send_json(addr, resp);
        return;
    }

    if (t == "get_peer") {
        std::string peer;
        if (msg.contains("peer_id") && msg["peer_id"].is_string()) peer = msg["peer_id"].get<std::string>();
        std::lock_guard<std::mutex> g(nodes_mutex);
        if (!peer.empty() && nodes.find(peer) != nodes.end()) {
            const NodeInfo &p = nodes[peer];
            json resp = {
                {"type", "peer_info"},
                {"peer_id", peer},
                {"public", { sockaddr_to_ip(p.public_addr), sockaddr_to_port(p.public_addr) }},
                {"local", p.local_addr}
            };
            send_json(addr, resp);
        } else {
            std::vector<std::string> avail;
            for (const auto &kv : nodes) avail.push_back(kv.first);
            json resp = {{"type", "error"}, {"message", "peer not found"}, {"available", avail}};
            send_json(addr, resp);
        }
        return;
    }

    if (t == "request_punch") {
        std::string peer;
        if (msg.contains("peer_id") && msg["peer_id"].is_string()) peer = msg["peer_id"].get<std::string>();
        NodeInfo peerNode;
        bool found = false;
        {
            std::lock_guard<std::mutex> g(nodes_mutex);
            auto it = nodes.find(peer);
            if (it != nodes.end()) {
                peerNode = it->second;
                found = true;
            }
        }
        if (!found) {
            json resp = {{"type", "error"}, {"message", "peer not found"}};
            send_json(addr, resp);
            return;
        }
        // notify peer about incoming attempt
        json notify = {
            {"type", "incoming_punch"},
            {"from_node", nid},
            {"from_public", { sockaddr_to_ip(addr), sockaddr_to_port(addr) }},
            {"from_local", json::array()},
            {"timestamp", now_seconds()}
        };
        if (msg.contains("local_addr") && msg["local_addr"].is_array()) notify["from_local"] = msg["local_addr"];
        send_json(peerNode.public_addr, notify);

        // send coordinates to initiator
        json coords = {
            {"type", "punch_coordinates"},
            {"peer_id", peer},
            {"peer_public", { sockaddr_to_ip(peerNode.public_addr), sockaddr_to_port(peerNode.public_addr) }},
            {"timestamp", now_seconds()}
        };
        send_json(addr, coords);
        return;
    }

    if (t == "relay") {
        std::string to;
        if (msg.contains("to") && msg["to"].is_string()) to = msg["to"].get<std::string>();
        NodeInfo toNode;
        bool found = false;
        {
            std::lock_guard<std::mutex> g(nodes_mutex);
            auto it = nodes.find(to);
            if (it != nodes.end()) {
                toNode = it->second;
                found = true;
            }
        }
        if (found) {
            json relay = {
                {"type", "relayed"},
                {"from", nid},
                {"content", msg.value("content", json())},
                {"timestamp", now_seconds()}
            };
            send_json(toNode.public_addr, relay);
            json ack = {{"type", "relay_ack"}, {"status", "ok"}};
            send_json(addr, ack);
        } else {
            json resp = {{"type", "error"}, {"message", "recipient not found"}};
            send_json(addr, resp);
        }
        return;
    }

    if (t == "hole_punch" || t == "hole_punch_response") {
        std::string peer;
        if (msg.contains("peer_id") && msg["peer_id"].is_string()) peer = msg["peer_id"].get<std::string>();
        NodeInfo peerNode;
        bool found = false;
        {
            std::lock_guard<std::mutex> g(nodes_mutex);
            auto it = nodes.find(peer);
            if (it != nodes.end()) {
                peerNode = it->second;
                found = true;
            }
        }
        if (found) {
            json forward = msg;
            forward["relayed_by_server"] = true;
            send_json(peerNode.public_addr, forward);
        }
        return;
    }

    if (t == "ping") {
        json resp = {{"type", "pong"}, {"timestamp", now_seconds()}};
        send_json(addr, resp);
        return;
    }

    // unknown types are ignored
}

void cleanup_loop() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        double now = now_seconds();
        std::lock_guard<std::mutex> g(nodes_mutex);
        for (auto it = nodes.begin(); it != nodes.end(); ) {
            if (now - it->second.last > 180.0) {
                std::cout << "Removing inactive node " << it->first << "\n";
                it = nodes.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void signal_handler(int signum) {
    running = false;
    if (sockfd >= 0) close(sockfd);
    std::cout << "\nShutting down server\n";
    std::exit(0);
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "socket error: " << strerror(errno) << "\n";
        return 1;
    }

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt error: " << strerror(errno) << "\n";
    }

    sockaddr_in servaddr;
    std::memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&servaddr), sizeof(servaddr)) < 0) {
        std::cerr << "bind error: " << strerror(errno) << "\n";
        close(sockfd);
        return 1;
    }

    std::cout << "Server listening on " << HOST << ":" << PORT << "\n";

    std::thread(cleanup_loop).detach();

    while (running) {
        char buffer[4096];
        sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                             reinterpret_cast<sockaddr*>(&cliaddr), &len);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "recvfrom error: " << strerror(errno) << "\n";
            break;
        }
        buffer[n] = '\0';
        json msg;
        try {
            msg = json::parse(buffer);
        } catch (const std::exception &) {
            // ignore invalid JSON
            continue;
        }
        // spawn handler thread
        std::thread t(handle_message, msg, cliaddr);
        t.detach();
    }

    if (sockfd >= 0) close(sockfd);
    return 0;
}
