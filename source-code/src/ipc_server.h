#ifndef IPC_SERVER_H
#define IPC_SERVER_H

#include <string>
#include <thread>
#include <atomic>
#include <set>
#include <mutex>
#include <functional>

class IPCServer {
public:
    IPCServer();
    ~IPCServer();

    void start();
    void stop();
    void broadcast(const std::string& json_message);
    void set_command_handler(std::function<void(const std::string&)> handler);

private:
    std::string socket_path = "/tmp/hackerland.sock";
    int server_fd = -1;
    std::atomic<bool> running{false};
    std::thread server_thread;
    std::recursive_mutex mutex;
    std::set<int> clients;
    std::function<void(const std::string&)> command_handler;
};

#endif // IPC_SERVER_H
