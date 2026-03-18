/*
 * CC3064 Sistemas Operativos - Proyecto 1
 * Servidor de Chat
 *
 * Uso: ./server <puerto>
 *
 * Protocolo: Simple-Chat-Protocol (protobuf + framing TCP de 5 bytes)
 * Hilos: un hilo por cliente + un hilo para detectar inactividad
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../common/framing.h"

// Includes de protobuf (se resuelven con -I generated/)
#include "common.pb.h"
#include "cliente-side/change_status.pb.h"
#include "cliente-side/get_user_info.pb.h"
#include "cliente-side/list_users.pb.h"
#include "cliente-side/message_dm.pb.h"
#include "cliente-side/message_general.pb.h"
#include "cliente-side/quit.pb.h"
#include "cliente-side/register.pb.h"
#include "server-side/all_users.pb.h"
#include "server-side/broadcast_messages.pb.h"
#include "server-side/for_dm.pb.h"
#include "server-side/get_user_info_response.pb.h"
#include "server-side/server_response.pb.h"

// Segundos sin actividad antes de poner el estado en INVISIBLE
#define INACTIVITY_TIMEOUT 60

// ─── Estado del cliente ───────────────────────────────────────────────────────

struct ClientInfo {
    std::string      username;
    std::string      ip;
    int              sockfd;
    chat::StatusEnum status;
    time_t           last_activity;
};

// username → ClientInfo  (unordered_map = O(1) lookup vs O(log n) de map)
static std::unordered_map<std::string, ClientInfo> g_users;
static std::mutex                                   g_users_mtx;

// Mutex por socket (evita escrituras simultáneas en el mismo stream TCP)
static std::unordered_map<int, std::shared_ptr<std::mutex>> g_sock_mtxs;
static std::mutex                                            g_sock_mtxs_lock;

// ─── Helpers para el mutex por socket ────────────────────────────────────────

static std::shared_ptr<std::mutex> get_send_mtx(int fd) {
    std::lock_guard<std::mutex> lk(g_sock_mtxs_lock);
    auto it = g_sock_mtxs.find(fd);
    if (it != g_sock_mtxs.end()) return it->second;
    auto m = std::make_shared<std::mutex>();
    g_sock_mtxs[fd] = m;
    return m;
}

static void remove_send_mtx(int fd) {
    std::lock_guard<std::mutex> lk(g_sock_mtxs_lock);
    g_sock_mtxs.erase(fd);
}

// ─── Helpers para envío seguro ────────────────────────────────────────────────

static bool safe_send(int fd, uint8_t type, const google::protobuf::Message& msg) {
    auto m = get_send_mtx(fd);
    std::lock_guard<std::mutex> lk(*m);
    return send_message(fd, type, msg);
}

// Envía bytes ya serializados — para broadcasts (serializar una vez, enviar N veces).
static bool safe_send_raw(int fd, uint8_t type, const std::string& payload) {
    auto m = get_send_mtx(fd);
    std::lock_guard<std::mutex> lk(*m);
    return send_raw(fd, type, payload);
}

static void send_response(int fd, int code, const std::string& text, bool ok) {
    chat::ServerResponse r;
    r.set_status_code(code);
    r.set_message(text);
    r.set_is_successful(ok);
    safe_send(fd, 10, r);
}

// Devuelve una copia de (sockfd, username) de todos los usuarios conectados.
// Se puede excluir un fd con skip_fd.
static std::vector<std::pair<int, std::string>> snapshot(int skip_fd = -1) {
    std::lock_guard<std::mutex> lk(g_users_mtx);
    std::vector<std::pair<int, std::string>> v;
    for (auto& [u, info] : g_users)
        if (info.sockfd != skip_fd)
            v.emplace_back(info.sockfd, u);
    return v;
}

// ─── Hilo por cliente ─────────────────────────────────────────────────────────

static void handle_client(int sockfd, std::string client_ip) {
    bool        registered = false;
    std::string username;

    while (true) {
        RecvResult res = recv_message(sockfd);

        if (!res.ok) {
            if (registered) {
                std::cout << "[SERVER] " << username << " desconectado\n";
                {
                    std::lock_guard<std::mutex> lk(g_users_mtx);
                    g_users.erase(username);
                }
                remove_send_mtx(sockfd);
            }
            close(sockfd);
            return;
        }

        // Actualiza last_activity con cada mensaje del usuario registrado.
        // No cambia el estado automáticamente, eso lo decide el usuario.
        if (registered) {
            std::lock_guard<std::mutex> lk(g_users_mtx);
            auto it = g_users.find(username);
            if (it != g_users.end())
                it->second.last_activity = time(nullptr);
        }

        switch (res.type) {

            // ── Tipo 1: Registro ──────────────────────────────────────────────
            case 1: {
                // Bug fix: rechazar doble registro en la misma conexión
                if (registered) {
                    send_response(sockfd, 400, "Already registered as " + username, false);
                    break;
                }

                chat::Register reg;
                if (!reg.ParseFromString(res.payload)) { close(sockfd); return; }

                // Validar username no vacío
                if (reg.username().empty()) {
                    send_response(sockfd, 400, "Username cannot be empty", false);
                    break;
                }

                bool name_taken = false;
                {
                    std::lock_guard<std::mutex> lk(g_users_mtx);
                    name_taken = g_users.count(reg.username()) > 0;
                }

                if (name_taken) {
                    send_response(sockfd, 400, "Username already taken", false);
                } else {
                    ClientInfo ci;
                    ci.username      = reg.username();
                    ci.ip            = reg.ip();
                    ci.sockfd        = sockfd;
                    ci.status        = chat::ACTIVE;
                    ci.last_activity = time(nullptr);
                    {
                        std::lock_guard<std::mutex> lk(g_users_mtx);
                        g_users[reg.username()] = ci;
                    }
                    registered = true;
                    username   = reg.username();
                    send_response(sockfd, 200, "Registered successfully", true);
                    std::cout << "[SERVER] Registrado: " << username
                              << " @ " << reg.ip() << "\n";
                }
                break;
            }

            // ── Tipo 2: MessageGeneral (broadcast) ────────────────────────────
            case 2: {
                if (!registered) { send_response(sockfd, 403, "Not registered", false); break; }
                chat::MessageGeneral mg;
                if (!mg.ParseFromString(res.payload)) break;

                chat::BroadcastDelivery bd;
                bd.set_message(mg.message());
                bd.set_username_origin(mg.username_origin());

                // Serializar UNA sola vez y enviar bytes crudos a todos (no re-serializar por cliente)
                std::string serialized;
                bd.SerializeToString(&serialized);
                for (auto& [fd, u] : snapshot())
                    safe_send_raw(fd, 13, serialized);
                break;
            }

            // ── Tipo 3: MessageDM ─────────────────────────────────────────────
            case 3: {
                if (!registered) { send_response(sockfd, 403, "Not registered", false); break; }
                chat::MessageDM dm;
                if (!dm.ParseFromString(res.payload)) break;

                int target_fd = -1;
                {
                    std::lock_guard<std::mutex> lk(g_users_mtx);
                    auto it = g_users.find(dm.username_des());
                    if (it != g_users.end()) target_fd = it->second.sockfd;
                }

                if (target_fd < 0) {
                    send_response(sockfd, 404, "User not found: " + dm.username_des(), false);
                } else {
                    // username_des en ForDm = quien envió el mensaje, para que el receptor sepa
                    chat::ForDm fwd;
                    fwd.set_username_des(username);
                    fwd.set_message(dm.message());
                    safe_send(target_fd, 12, fwd);
                    send_response(sockfd, 200, "DM sent to " + dm.username_des(), true);
                }
                break;
            }

            // ── Tipo 4: ChangeStatus ──────────────────────────────────────────
            case 4: {
                if (!registered) { send_response(sockfd, 403, "Not registered", false); break; }
                chat::ChangeStatus cs;
                if (!cs.ParseFromString(res.payload)) break;

                {
                    std::lock_guard<std::mutex> lk(g_users_mtx);
                    auto it = g_users.find(username);
                    if (it != g_users.end()) it->second.status = cs.status();
                }
                send_response(sockfd, 200, "Status updated", true);
                break;
            }

            // ── Tipo 5: ListUsers ─────────────────────────────────────────────
            case 5: {
                if (!registered) { send_response(sockfd, 403, "Not registered", false); break; }

                chat::AllUsers au;
                {
                    std::lock_guard<std::mutex> lk(g_users_mtx);
                    for (auto& [u, info] : g_users) {
                        au.add_usernames(u);
                        au.add_status(info.status);
                    }
                }
                safe_send(sockfd, 11, au);
                break;
            }

            // ── Tipo 6: GetUserInfo ───────────────────────────────────────────
            case 6: {
                if (!registered) { send_response(sockfd, 403, "Not registered", false); break; }
                chat::GetUserInfo gui;
                if (!gui.ParseFromString(res.payload)) break;

                bool found = false;
                chat::GetUserInfoResponse resp;
                {
                    std::lock_guard<std::mutex> lk(g_users_mtx);
                    auto it = g_users.find(gui.username_des());
                    if (it != g_users.end()) {
                        resp.set_ip_address(it->second.ip);
                        resp.set_username(it->second.username);
                        resp.set_status(it->second.status);
                        found = true;
                    }
                }

                if (found)
                    safe_send(sockfd, 14, resp);
                else
                    send_response(sockfd, 404, "User not found: " + gui.username_des(), false);
                break;
            }

            // ── Tipo 7: Quit ──────────────────────────────────────────────────
            case 7: {
                if (registered) {
                    std::cout << "[SERVER] " << username << " salió correctamente\n";
                    {
                        std::lock_guard<std::mutex> lk(g_users_mtx);
                        g_users.erase(username);
                    }
                    remove_send_mtx(sockfd);
                }
                close(sockfd);
                return;
            }

            default:
                std::cerr << "[SERVER] Tipo de mensaje desconocido: " << static_cast<int>(res.type) << "\n";
        }
    }
}

// ─── Hilo de monitoreo de inactividad ────────────────────────────────────────

static void inactivity_monitor() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        time_t now = time(nullptr);

        std::lock_guard<std::mutex> lk(g_users_mtx);
        for (auto& [u, info] : g_users) {
            if (info.status != chat::INVISIBLE &&
                (now - info.last_activity) > INACTIVITY_TIMEOUT) {
                info.status = chat::INVISIBLE;
                std::cout << "[SERVER] " << u << " pasó a INVISIBLE por inactividad\n";
            }
        }
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Uso: " << argv[0] << " <puerto>\n";
        return 1;
    }

    GOOGLE_PROTOBUF_VERIFY_VERSION;

    int port = std::stoi(argv[1]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
    setsockopt(server_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    // Bug fix: backlog 10 rechazaba conexiones con muchos clientes simultáneos
    if (listen(server_fd, SOMAXCONN) < 0) { perror("listen"); return 1; }

    std::cout << "[SERVER] Escuchando en puerto " << port
              << " | timeout de inactividad = " << INACTIVITY_TIMEOUT << "s\n";

    std::thread(inactivity_monitor).detach();

    while (true) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int client_fd = accept(server_fd,
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &client_len);
        if (client_fd < 0) { perror("accept"); continue; }

        std::string client_ip = inet_ntoa(client_addr.sin_addr);
        std::cout << "[SERVER] Nueva conexión desde " << client_ip << "\n";

#ifdef SO_NOSIGPIPE
        // macOS: evitar SIGPIPE en sockets de clientes
        int nosig = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
        // Se crea el mutex antes de lanzar el hilo
        get_send_mtx(client_fd);

        std::thread(handle_client, client_fd, client_ip).detach();
    }

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
