#include "engine/client.h"

#include "common/log.h"
#include "engine/engine.h"

#include <SDL.h>
#include <SDL_net.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm0 {

Client::Client(Engine *engine, const config::RuntimeOptions &options)
    : engine_(engine), options_(options) {}

Client::~Client() {
    disconnect();
}

bool Client::connect() {
    pending_.clear();

    IPaddress addr;
    if (SDLNet_ResolveHost(&addr, const_cast<char *>(options_.server.c_str()), options_.port) < 0) {
        LOG_INFO_NONFULLSPEED("Client::connect: SDLNet_ResolveHost failed: " << SDLNet_GetError());
        return false;
    }
    sock_ = SDLNet_TCP_Open(&addr);
    if (sock_ == nullptr) {
        LOG_INFO_NONFULLSPEED("Client::connect: SDLNet_TCP_Open failed: " << SDLNet_GetError());
        return false;
    }

    const int got = SDLNet_TCP_Recv(sock_, recv_buf_, int(sizeof(recv_buf_)) - 1);
    if (got <= 0) {
        LOG_INFO_NONFULLSPEED("Client::connect: no welcome message from server");
        SDLNet_TCP_Close(sock_);
        sock_ = nullptr;
        return false;
    }

    bool welcome_seen = false;
    pending_.append(recv_buf_, got);
    std::size_t newline = pending_.find('\n');
    while (newline != std::string::npos) {
        std::string line = pending_.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pending_.erase(0, newline + 1);

        if (!welcome_seen) {
            if (std::strncmp(line.c_str(), "/welcome", 8) != 0) {
                LOG_INFO_NONFULLSPEED("Client::connect: expected /welcome, got: " << line);
                SDLNet_TCP_Close(sock_);
                sock_ = nullptr;
                pending_.clear();
                return false;
            }
            const char *mapname = line.c_str() + 9; // "/welcome X"
            engine_->on_welcome(mapname);
            welcome_seen = true;
        } else {
            parse_line(line.c_str());
        }

        newline = pending_.find('\n');
    }
    if (!welcome_seen) {
        LOG_INFO_NONFULLSPEED("Client::connect: no complete welcome line");
        SDLNet_TCP_Close(sock_);
        sock_ = nullptr;
        pending_.clear();
        return false;
    }

    set_ = SDLNet_AllocSocketSet(1);
    SDLNet_TCP_AddSocket(set_, sock_);
    connected_ = true;

    // Nom de l'équipe
    char name[128];
    const int n = std::snprintf(name, sizeof(name), "/name %s\n", options_.teamname.c_str());
    SDLNet_TCP_Send(sock_, name, n);

    LOG_INFO_NONFULLSPEED("Client::connect: connected to " << options_.server << ":" << options_.port << " as \"" << options_.teamname << "\"");
    return true;
}

void Client::disconnect() {
    if (set_ != nullptr && sock_ != nullptr) {
        SDLNet_TCP_DelSocket(set_, sock_);
    }
    if (sock_ != nullptr) {
        SDLNet_TCP_Close(sock_);
        sock_ = nullptr;
    }
    if (set_ != nullptr) {
        SDLNet_FreeSocketSet(set_);
        set_ = nullptr;
    }
    pending_.clear();
    connected_ = false;
}

void Client::poll() {
    if (!connected_ || sock_ == nullptr)
        return;

    const int count = SDLNet_CheckSockets(set_, 0);
    if (count <= 0)
        return;
    if (!SDLNet_SocketReady(sock_))
        return;

    const int got = SDLNet_TCP_Recv(sock_, recv_buf_, int(sizeof(recv_buf_)) - 1);
    if (got <= 0) {
        LOG_INFO_NONFULLSPEED("Client::poll: server disconnected");
        connected_ = false;
        engine_->on_disconnect();
        return;
    }

    pending_.append(recv_buf_, got);
    std::size_t newline = pending_.find('\n');
    while (newline != std::string::npos) {
        std::string line = pending_.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pending_.erase(0, newline + 1);
        parse_line(line.c_str());
        newline = pending_.find('\n');
    }

    if (pending_.size() >= sizeof(recv_buf_)) {
        LOG_INFO_NONFULLSPEED("Client::poll: dropping oversized partial line");
        pending_.clear();
    }
}

void Client::send_move(Move m) {
    if (!connected_ || !m.is_valid())
        return;

    const int ox = sq_x(m.from);
    const int oy = sq_y(m.from);
    const int nx = sq_x(m.to);
    const int ny = sq_y(m.to);
    const int n = std::snprintf(send_buf_, sizeof(send_buf_),
                                "/do_move (%d,%d) to (%d,%d)\n", ox, oy, nx, ny);
    SDLNet_TCP_Send(sock_, send_buf_, n);
    LOG_INFO_NONFULLSPEED("sent /do_move (" << ox << "," << oy << ") to (" << nx << "," << ny << ")");
}

void Client::send_map(const std::string &map_name) {
    if (!connected_ || map_name.empty())
        return;
    const int n = std::snprintf(send_buf_, sizeof(send_buf_), "/map %s\n", map_name.c_str());
    SDLNet_TCP_Send(sock_, send_buf_, n);
    LOG_INFO_NONFULLSPEED("sent /map " << map_name);
}

void Client::send_start() {
    if (!connected_)
        return;
    send_line("/start\n");
    LOG_INFO_NONFULLSPEED("sent /start");
}

void Client::send_line(const char *line) {
    if (!connected_ || line == nullptr || line[0] == 0)
        return;
    SDLNet_TCP_Send(sock_, const_cast<char *>(line), int(std::strlen(line)));
}

void Client::parse_line(const char *line) {
    if (line == nullptr || line[0] == 0)
        return;

    if (std::strncmp(line, "/map", 4) == 0) {
        engine_->on_map_change(line + 5);
        return;
    }
    if (std::strncmp(line, "/start", 6) == 0) {
        engine_->on_start(std::atoi(line + 7));
        return;
    }
    if (std::strncmp(line, "/do_move", 8) == 0) {
        int ox, oy, nx, ny;
        if (std::sscanf(line + 9, "(%d,%d) to (%d,%d)", &ox, &oy, &nx, &ny) == 4) {
            engine_->on_opp_move(move_from_coords(ox, oy, nx, ny));
        }
        return;
    }
    LOG_DEBUG("Client::parse_line: ignoring \"" << line << "\"");
}

} // namespace mm0
