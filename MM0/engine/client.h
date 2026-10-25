#ifndef MM0_ENGINE_CLIENT_H
#define MM0_ENGINE_CLIENT_H

#include "common/config.h"
#include "common/types.h"

#include <SDL_net.h>
#include <string>

namespace mm0 {

class Engine;

// Protocole
//   Server -> Client:  /welcome <map>, /map <name>, /start <id>, /do_move (ox,oy) to (nx,ny)
//   Client -> Server:  /name <team>, /map <name>, /start, /do_move (ox,oy) to (nx,ny)
class Client {
public:
    Client(Engine *engine, const config::RuntimeOptions &options);
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // On ouvre le socket, on attend le /welcome de bienvenue, et on envoie notre /name.
    bool connect();

    // Déconnexion propre en fin de partie
    void disconnect();

    bool is_connected() const { return connected_; }

    // Vérification non bloquante et dispatch
    void poll();

    // Envoie d'un coup au serveur
    void send_move(Move m);

    // Commandes du démarrage
    void send_map(const std::string &map_name);
    void send_start();

private:
    void parse_line(const char *line);
    void send_line(const char *line);

    Engine *engine_ = nullptr;
    config::RuntimeOptions options_;
    TCPsocket sock_ = nullptr;
    SDLNet_SocketSet set_ = nullptr;
    bool connected_ = false;
    std::string pending_;
    char recv_buf_[4096];
    char send_buf_[256];
};

} // namespace mm0

#endif
