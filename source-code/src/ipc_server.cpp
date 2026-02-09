#include "ipc_server.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <vector>

IPCServer::IPCServer() {}

IPCServer::~IPCServer() {
    stop();
}

void IPCServer::set_command_handler(std::function<void(const std::string&)> handler) {
    command_handler = handler;
}

void IPCServer::stop() {
    running = false;
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
        unlink(socket_path.c_str());
    }
    if (server_thread.joinable()) {
        server_thread.join();
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (int fd : clients) close(fd);
    clients.clear();
}

void IPCServer::start() {
    if (running) return;

    unlink(socket_path.c_str());
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path)-1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) return;
    listen(server_fd, 5);

    // Non-blocking
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    running = true;
    server_thread = std::thread([this] {
        while (running) {
            // Accept new clients
            int client = accept(server_fd, NULL, NULL);
            if (client != -1) {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                clients.insert(client);
            }

            // Read from clients
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                for (auto it = clients.begin(); it != clients.end(); ) {
                    char buf[1024];
                    ssize_t n = recv(*it, buf, sizeof(buf)-1, MSG_DONTWAIT);
                    if (n > 0) {
                        buf[n] = 0;
                        if (command_handler) command_handler(std::string(buf));
                        ++it;
                    } else if (n == 0 || (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        close(*it);
                        it = clients.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void IPCServer::broadcast(const std::string& msg) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (auto it = clients.begin(); it != clients.end(); ) {
        // Simple send, ideally should handle partial writes
        if (send(*it, msg.c_str(), msg.length(), MSG_NOSIGNAL) == -1 && errno == EPIPE) {
            close(*it);
            it = clients.erase(it);
        } else {
            ++it;
        }
    }
}
