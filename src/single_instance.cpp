//////////////////////////////////////////////////////////////////////

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "log.h"
#include "util.h"
#include "single_instance.h"

LOG_CONTEXT("single_instance", info);

namespace
{
#if defined(_WIN32)
    using socket_t = SOCKET;
    socket_t const no_socket = INVALID_SOCKET;

    void close_socket(socket_t s)
    {
        closesocket(s);
    }

    int poll_socket(socket_t s, int timeout_ms)
    {
        WSAPOLLFD fd{ s, POLLRDNORM, 0 };
        return WSAPoll(&fd, 1, timeout_ms);
    }

    bool init_sockets()
    {
        static bool const ok = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        return ok;
    }
#else
    using socket_t = int;
    socket_t const no_socket = -1;

    void close_socket(socket_t s)
    {
        close(s);
    }

    int poll_socket(socket_t s, int timeout_ms)
    {
        pollfd fd{ s, POLLIN, 0 };
        return poll(&fd, 1, timeout_ms);
    }

    bool init_sockets()
    {
        return true;
    }
#endif

    //////////////////////////////////////////////////////////////////////
    // per user: in the config directory, with the settings

    std::filesystem::path socket_path()
    {
        return config_path(app_name, "3D-Viewer-Active");
    }

    bool make_address(sockaddr_un &address)
    {
        std::u8string path = socket_path().u8string();
        if(path.size() >= sizeof(address.sun_path)) {
            LOG_WARNING("Socket path is too long: {}", std::string(path.begin(), path.end()));
            return false;
        }
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.data(), path.size());
        return true;
    }

    //////////////////////////////////////////////////////////////////////

    socket_t connect_to_running()
    {
        sockaddr_un address;
        if(!init_sockets() || !make_address(address)) {
            return no_socket;
        }
        socket_t s = socket(AF_UNIX, SOCK_STREAM, 0);
        if(s == no_socket) {
            return no_socket;
        }
        if(connect(s, (sockaddr const *)&address, sizeof(address)) != 0) {
            close_socket(s);
            return no_socket;
        }
        return s;
    }

    //////////////////////////////////////////////////////////////////////

    std::atomic<bool> stopping{ false };
    std::thread listener;
    socket_t listen_socket = no_socket;

}    // namespace

//////////////////////////////////////////////////////////////////////

bool single_instance::send_to_running(std::filesystem::path const &file)
{
    socket_t s = connect_to_running();
    if(s == no_socket) {
        return false;
    }

#if defined(_WIN32)
    // the running one can only bring itself to the front if we let it (we're in front, we were just launched)
    AllowSetForegroundWindow(ASFW_ANY);
#endif

    std::u8string path = file.empty() ? std::u8string() : std::filesystem::absolute(file).u8string();
    char const *data = (char const *)path.data();
    size_t remaining = path.size();
#if defined(MSG_NOSIGNAL)
    int const flags = MSG_NOSIGNAL;    // an error rather than SIGPIPE if it's gone
#else
    int const flags = 0;
#endif
    while(remaining != 0) {
        int sent = (int)send(s, data, (int)remaining, flags);
        if(sent <= 0) {
            break;
        }
        data += sent;
        remaining -= (size_t)sent;
    }
    close_socket(s);
    LOG_INFO("Sent {} to the running instance", file.empty() ? std::string("a wake up") : std::string(path.begin(), path.end()));
    return remaining == 0;
}

//////////////////////////////////////////////////////////////////////

bool single_instance::start_listening(std::function<void(std::filesystem::path const &)> on_request)
{
    sockaddr_un address;
    if(!init_sockets() || !make_address(address)) {
        return false;
    }

    socket_t s = socket(AF_UNIX, SOCK_STREAM, 0);
    if(s == no_socket) {
        return false;
    }

    if(bind(s, (sockaddr const *)&address, sizeof(address)) != 0) {
        // someone else is listening, or it's left over from one which didn't exit cleanly
        socket_t other = connect_to_running();
        if(other != no_socket) {
            close_socket(other);
            close_socket(s);
            return false;
        }
        std::error_code error;
        std::filesystem::remove(socket_path(), error);
        if(bind(s, (sockaddr const *)&address, sizeof(address)) != 0) {
            LOG_WARNING("Can't listen for other instances");
            close_socket(s);
            return false;
        }
    }
    if(listen(s, 4) != 0) {
        close_socket(s);
        return false;
    }

    listen_socket = s;
    stopping = false;

    // waits for connections with no timeout, so it costs nothing while it's idle: stop_listening()
    // wakes it up by connecting itself (closing the socket doesn't wake accept() everywhere)
    listener = std::thread([s, on_request] {
        while(true) {
            socket_t client = accept(s, nullptr, nullptr);
            if(stopping) {
                if(client != no_socket) {
                    close_socket(client);
                }
                break;
            }
            if(client == no_socket) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));    // shouldn't happen, but don't spin
                continue;
            }

            // a request is everything up to the other end closing the connection, if it doesn't
            // (or something goes wrong) it's ignored rather than acted on
            std::string request;
            bool complete = false;
            char buffer[1024];
            while(request.size() <= 65536 && poll_socket(client, 1000) > 0) {
                int got = (int)recv(client, buffer, (int)sizeof(buffer), 0);
                if(got <= 0) {
                    complete = got == 0;
                    break;
                }
                request.append(buffer, (size_t)got);
            }
            close_socket(client);
            if(complete) {
                on_request(std::filesystem::path(std::u8string(request.begin(), request.end())));
            } else {
                LOG_WARNING("Ignoring an incomplete request from another instance");
            }
        }
    });
    return true;
}

//////////////////////////////////////////////////////////////////////

void single_instance::stop_listening()
{
    if(listen_socket == no_socket) {
        return;
    }
    // wake the listener up so it sees it's stopping
    stopping = true;
    socket_t wake = connect_to_running();
    if(wake != no_socket) {
        close_socket(wake);
        if(listener.joinable()) {
            listener.join();
        }
        close_socket(listen_socket);
    } else {
        // can't connect (the socket file's gone?) so it might never wake up: don't wait for it
#if defined(_WIN32)
        shutdown(listen_socket, SD_BOTH);
#else
        shutdown(listen_socket, SHUT_RDWR);
#endif
        close_socket(listen_socket);
        if(listener.joinable()) {
            listener.detach();
        }
    }
    listen_socket = no_socket;
    std::error_code error;
    std::filesystem::remove(socket_path(), error);
}
