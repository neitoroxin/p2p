// client.cpp
// P2P client with hole punching and direct confirmation
// Usage: compile and run; set SERVER_IP to your server public IP
// Requires nlohmann::json single-header "json.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// include nlohmann json header (place json.hpp in same dir)
#include "json.hpp"
using json = nlohmann::json;

using namespace std::chrono;

const char* SERVER_IP = "89.169.159.66"; // <-- change this if needed
const int SERVER_PORT = 55555;

int sockfd = -1;
bool running = true;
std::mutex peers_mutex;

struct PeerInfo {
    std::pair<std::string,int> public_addr;   // ip, port
    std::pair<std::string,int> direct_addr;   // ip, port (candidate)
    bool has_public = false;
    bool has_direct = false;
    bool direct_confirmed = false;
    std::string status;
    double last_seen = 0;
};

std::map<std::string, PeerInfo> peers;
std::string NODE_ID;
sockaddr_in server_sockaddr;

double now_seconds() {
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

std::string make_node_id() {
    // generate 8-char hex id
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t v = gen();
    char buf[32];
    snprintf(buf, sizeof(buf), "%08x", (unsigned int)(v & 0xffffffff));
    return std::string(buf);
}

sockaddr_in make_sockaddr(const std::string &ip, int port) {
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
    return sa;
}

std::pair<std::string,int> sockaddr_to_pair(const sockaddr_in &sa) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf));
    return {std::string(buf), ntohs(sa.sin_port)};
}

void send_json_to(const sockaddr_in &addr, const json &obj) {
    std::string s = obj.dump();
    ssize_t sent = sendto(sockfd, s.c_str(), s.size(), 0, (const sockaddr*)&addr, sizeof(addr));
    if (sent < 0) {
        std::cerr << "sendto error: " << strerror(errno) << "\n";
    }
}

void send_raw_to(const sockaddr_in &addr, const json &obj) {
    send_json_to(addr, obj);
}

void send_raw_tuple(const std::pair<std::string,int> &target, const json &obj) {
    sockaddr_in sa = make_sockaddr(target.first, target.second);
    send_json_to(sa, obj);
}

void send_raw_server(const json &obj) {
    send_json_to(server_sockaddr, obj);
}

void register_and_stun(int local_port) {
    // STUN
    json stun = {{"type","stun"}, {"node_id", NODE_ID}};
    send_raw_server(stun);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // register with local addr
    json reg = {{"type","register"}, {"node_id", NODE_ID}, {"local_addr", { "0.0.0.0", local_port }}};
    send_raw_server(reg);
}

void send_direct_test_ping(const std::string &peer_id);
// forward declarations
void send_direct_test_ping(const std::string &peer_id);
bool send_message(const std::string &peer_id, const std::string &text);


void handle_msg(const json &msg, const sockaddr_in &addr) {
    std::string t = msg.value("type", "");
    if (t == "stun_response") {
        std::cout << "STUN response: " << msg.value("public_ip","") << " " << msg.value("public_port",0) << "\n";
    } else if (t == "register_response") {
        std::cout << "Registered on server. Active peers: " << msg.value("active", 0) << "\n";
    } else if (t == "peer_info") {
        std::string pid = msg.value("peer_id", "");
        auto pub = msg.value("public", json::array());
        if (pub.is_array() && pub.size() >= 2) {
            std::string ip = pub[0].get<std::string>();
            int port = pub[1].get<int>();
            std::lock_guard<std::mutex> g(peers_mutex);
            PeerInfo &p = peers[pid];
            p.public_addr = {ip, port};
            p.has_public = true;
            p.status = "known";
            p.last_seen = now_seconds();
            std::cout << "Peer info: " << pid << " -> " << ip << ":" << port << "\n";
        }
    } else if (t == "incoming_punch") {
        std::string from_node = msg.value("from_node", "");
        auto from_public = msg.value("from_public", json::array());
        if (from_public.is_array() && from_public.size() >= 2) {
            std::string ip = from_public[0].get<std::string>();
            int port = from_public[1].get<int>();
            std::cout << "Incoming punch from " << from_node << " at " << ip << ":" << port << "\n";
            // reply immediately to public address to help hole punching
            json reply = {{"type","hole_punch"}, {"node_id", NODE_ID}, {"peer_id", from_node}, {"timestamp", now_seconds()}};
            sockaddr_in target = make_sockaddr(ip, port);
            send_json_to(target, reply);
        }
    } else if (t == "punch_coordinates") {
        std::string peer_id = msg.value("peer_id", "");
        auto peer_public = msg.value("peer_public", json::array());
        if (peer_public.is_array() && peer_public.size() >= 2) {
            std::string ip = peer_public[0].get<std::string>();
            int port = peer_public[1].get<int>();
            std::cout << "Punch coordinates for " << peer_id << ": " << ip << ":" << port << "\n";
            // start active hole punching attempts
            std::thread([peer_id, ip, port]() {
                // perform_hole_punch
                std::cout << "Starting hole punch to " << peer_id << " at " << ip << ":" << port << "\n";
                {
                    std::lock_guard<std::mutex> g(peers_mutex);
                    PeerInfo &p = peers[peer_id];
                    p.public_addr = {ip, port};
                    p.has_public = true;
                    p.status = "punching";
                }
                json encoded = {{"type","hole_punch"}, {"node_id", NODE_ID}, {"peer_id", peer_id}, {"timestamp", now_seconds()}};
                sockaddr_in target = make_sockaddr(ip, port);
                for (int i = 0; i < 20; ++i) {
                    send_json_to(target, encoded);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    std::lock_guard<std::mutex> g(peers_mutex);
                    if (peers[peer_id].direct_confirmed) return;
                }
                std::cout << "Hole punch attempts finished for " << peer_id << "\n";
            }).detach();
        }
    } else if (t == "hole_punch") {
        std::string peer_id = msg.value("node_id", "");
        auto addr_pair = sockaddr_to_pair(addr);
        std::cout << "Received hole_punch from " << peer_id << " via " << addr_pair.first << ":" << addr_pair.second << "\n";
        // reply directly to sender address to establish mapping
        json resp = {{"type","hole_punch_response"}, {"node_id", NODE_ID}, {"peer_id", peer_id}, {"timestamp", now_seconds()}};
        send_json_to(addr, resp);
        // record direct_addr candidate
        {
            std::lock_guard<std::mutex> g(peers_mutex);
            PeerInfo &p = peers[peer_id];
            p.direct_addr = addr_pair;
            p.has_direct = true;
            p.status = "connected";
            p.last_seen = now_seconds();
        }
    } else if (t == "hole_punch_response") {
        auto addr_pair = sockaddr_to_pair(addr);
        std::cout << "Received hole_punch_response from " << addr_pair.first << ":" << addr_pair.second << "\n";
        // match by addr to known peer public or direct
        std::string matched;
        {
            std::lock_guard<std::mutex> g(peers_mutex);
            for (auto &kv : peers) {
                const std::string &pid = kv.first;
                PeerInfo &info = kv.second;
                if (info.has_public && info.public_addr == addr_pair) {
                    matched = pid; break;
                }
                if (info.has_direct && info.direct_addr == addr_pair) {
                    matched = pid; break;
                }
            }
            if (!matched.empty()) {
                PeerInfo &p = peers[matched];
                p.direct_addr = addr_pair;
                p.has_direct = true;
                p.status = "connected";
                p.last_seen = now_seconds();
            }
        }
        if (!matched.empty()) {
            send_direct_test_ping(matched);
        }
    } else if (t == "message") {
        std::string sender = msg.value("from", "");
        std::string content = msg.value("content", "");
        auto addr_pair = sockaddr_to_pair(addr);
        std::cout << "\nMessage from " << sender << ": " << content << " (addr " << addr_pair.first << ":" << addr_pair.second << ")\n";
        if (content == "__direct_test_ping__") {
            // reply with pong
            send_message(sender, "__direct_test_pong__");
        } else if (content == "__direct_test_pong__") {
            std::lock_guard<std::mutex> g(peers_mutex);
            if (peers.find(sender) != peers.end()) {
                peers[sender].direct_confirmed = true;
                peers[sender].last_seen = now_seconds();
                std::cout << "✅ Direct confirmed with " << sender << "\n";
            }
        }
    } else if (t == "relayed") {
        std::cout << "Relayed message from " << msg.value("from","") << ": " << msg.value("content","") << "\n";
    } else if (t == "pong") {
        // server pong - ignore
    } else {
        // ignore unknown
    }
}

void recv_loop() {
    while (running) {
        char buf[4096];
        sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf)-1, 0, (sockaddr*)&cliaddr, &len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            } else {
                if (running) std::cerr << "recvfrom error: " << strerror(errno) << "\n";
                break;
            }
        }
        buf[n] = '\0';
        try {
            json msg = json::parse(buf);
            handle_msg(msg, cliaddr);
        } catch (...) {
            // ignore invalid json
        }
    }
}

void send_direct_test_ping(const std::string &peer_id) {
    PeerInfo info;
    {
        std::lock_guard<std::mutex> g(peers_mutex);
        if (peers.find(peer_id) == peers.end()) return;
        info = peers[peer_id];
    }
    std::pair<std::string,int> target;
    if (info.direct_confirmed && info.has_direct) {
        target = info.direct_addr;
    } else if (info.has_direct) {
        target = info.direct_addr;
    } else if (info.has_public) {
        target = info.public_addr;
    } else {
        return;
    }
    json msg = {{"type","message"}, {"from", NODE_ID}, {"to", peer_id}, {"content","__direct_test_ping__"}, {"timestamp", now_seconds()}};
    send_raw_tuple(target, msg);
}

bool send_message(const std::string &peer_id, const std::string &text) {
    PeerInfo info;
    {
        std::lock_guard<std::mutex> g(peers_mutex);
        if (peers.find(peer_id) == peers.end()) {
            std::cout << "Peer unknown. Use connect <peer_id> first.\n";
            return false;
        }
        info = peers[peer_id];
    }

    // prefer direct confirmed
    if (info.direct_confirmed && info.has_direct) {
        json msg = {{"type","message"}, {"from", NODE_ID}, {"to", peer_id}, {"content", text}, {"timestamp", now_seconds()}};
        send_raw_tuple(info.direct_addr, msg);
        std::cout << "Sent direct to " << peer_id << " -> " << info.direct_addr.first << ":" << info.direct_addr.second << "\n";
        return true;
    }

    // try direct candidate
    if (info.has_direct) {
        json msg = {{"type","message"}, {"from", NODE_ID}, {"to", peer_id}, {"content", text}, {"timestamp", now_seconds()}};
        send_raw_tuple(info.direct_addr, msg);
        std::cout << "Sent to candidate direct " << peer_id << " -> " << info.direct_addr.first << ":" << info.direct_addr.second << "\n";
        return true;
    }

    // fallback: relay via server
    json relay = {{"type","relay"}, {"node_id", NODE_ID}, {"to", peer_id}, {"content", text}};
    send_raw_server(relay);
    std::cout << "Sent via server relay to " << peer_id << "\n";
    return true;
}

void peer_keepalive_loop() {
    while (running) {
        {
            std::lock_guard<std::mutex> g(peers_mutex);
            for (auto &kv : peers) {
                const std::string &pid = kv.first;
                PeerInfo &info = kv.second;
                if (info.direct_confirmed && info.has_direct) {
                    json ping = {{"type","message"}, {"from", NODE_ID}, {"to", pid}, {"content","__keepalive__"}, {"timestamp", now_seconds()}};
                    send_raw_tuple(info.direct_addr, ping);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(25));
    }
}

void server_ping_loop() {
    while (running) {
        json ping = {{"type","ping"}, {"node_id", NODE_ID}};
        send_raw_server(ping);
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

void input_loop() {
    std::string line;
    while (running) {
        std::cout << "\n> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line.rfind("connect ", 0) == 0) {
            std::string peer = line.substr(8);
            json getp = {{"type","get_peer"}, {"node_id", NODE_ID}, {"peer_id", peer}};
            send_raw_server(getp);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            json req = {{"type","request_punch"}, {"node_id", NODE_ID}, {"peer_id", peer}, {"local_addr", { "0.0.0.0", 0 }}};
            send_raw_server(req);
        } else if (line.rfind("send ", 0) == 0) {
            // send <peer_id> <message>
            size_t p1 = line.find(' ', 5);
            if (p1 == std::string::npos) {
                std::cout << "Usage: send <peer_id> <message>\n";
                continue;
            }
            std::string peer = line.substr(5, p1-5);
            std::string text = line.substr(p1+1);
            send_message(peer, text);
        } else if (line.rfind("send_relay ", 0) == 0) {
            size_t p1 = line.find(' ', 11);
            if (p1 == std::string::npos) {
                std::cout << "Usage: send_relay <peer_id> <message>\n";
                continue;
            }
            std::string peer = line.substr(11, p1-11);
            std::string text = line.substr(p1+1);
            json relay = {{"type","relay"}, {"node_id", NODE_ID}, {"to", peer}, {"content", text}};
            send_raw_server(relay);
        } else if (line == "list") {
            std::lock_guard<std::mutex> g(peers_mutex);
            if (peers.empty()) {
                std::cout << "No known peers\n";
            } else {
                for (auto &kv : peers) {
                    const std::string &pid = kv.first;
                    PeerInfo &info = kv.second;
                    std::cout << pid << ": status=" << info.status
                              << " direct_confirmed=" << (info.direct_confirmed ? "true":"false")
                              << " public=" << (info.has_public ? info.public_addr.first + ":" + std::to_string(info.public_addr.second) : "none")
                              << " direct=" << (info.has_direct ? info.direct_addr.first + ":" + std::to_string(info.direct_addr.second) : "none")
                              << "\n";
                }
            }
        } else if (line == "quit") {
            break;
        } else {
            std::cout << "Unknown command. Commands: connect, send, send_relay, list, quit\n";
        }
    }
    running = false;
}

int main() {
    if (std::string(SERVER_IP).find("YOUR_SERVER_IP") != std::string::npos) {
        std::cerr << "ERROR: please set SERVER_IP in client.cpp to your server public IP and restart.\n";
        return 1;
    }

    NODE_ID = make_node_id();
    std::cout << "P2P client\nYour id: " << NODE_ID << "\n";

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "socket error: " << strerror(errno) << "\n";
        return 1;
    }

    // bind to ephemeral port on all interfaces
    sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(0);
    if (bind(sockfd, (sockaddr*)&local, sizeof(local)) < 0) {
        std::cerr << "bind error: " << strerror(errno) << "\n";
        close(sockfd);
        return 1;
    }

    // get bound port
    socklen_t len = sizeof(local);
    getsockname(sockfd, (sockaddr*)&local, &len);
    int local_port = ntohs(local.sin_port);

    // set recv timeout (non-blocking style)
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // prepare server sockaddr
    std::memset(&server_sockaddr, 0, sizeof(server_sockaddr));
    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_sockaddr.sin_addr);

    register_and_stun(local_port);

    std::thread recv_thread(recv_loop);
    std::thread keepalive_thread(peer_keepalive_loop);
    std::thread ping_thread(server_ping_loop);

    input_loop();

    // cleanup
    running = false;
    json unreg = {{"type","unregister"}, {"node_id", NODE_ID}};
    send_raw_server(unreg);

    // close socket and join threads
    close(sockfd);
    if (recv_thread.joinable()) recv_thread.join();
    if (keepalive_thread.joinable()) keepalive_thread.join();
    if (ping_thread.joinable()) ping_thread.join();

    std::cout << "Client stopped\n";
    return 0;
}
