/*
 * CC3064 Sistemas Operativos - Proyecto 1
 * Cliente de Chat
 *
 * Uso: ./client <username> <server_ip> <server_port>
 *
 * Comandos:
 *   <mensaje>                  Envía al chat general
 *   /dm <usuario> <mensaje>    Envía un mensaje directo
 *   /status <ESTADO>           Cambia estado (Activo, Ocupado, AFK)
 *   /list                      Lista los usuarios conectados
 *   /info <usuario>            Ver info de un usuario
 *   /help                      Muestra esta ayuda
 *   /quit                      Desconectarse y salir
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

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

static std::atomic<bool> g_running{true};
static std::mutex         g_print_mtx;
static std::mutex         g_send_mtx;

static int         g_sockfd   = -1;
static std::string g_username;
static std::string g_my_ip;

// ─── Colores ANSI ─────────────────────────────────────────────────────────────

#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_DIM     "\033[2m"
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[37m"
#define CLR_BWHITE  "\033[97m"

// ─── Utilidades ───────────────────────────────────────────────────────────────

static std::string timestamp() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    std::ostringstream ss;
    ss << CLR_DIM << "["
       << std::setw(2) << std::setfill('0') << t->tm_hour << ":"
       << std::setw(2) << std::setfill('0') << t->tm_min
       << "]" << CLR_RESET;
    return ss.str();
}

static void print(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_print_mtx);
    std::cout << msg << "\n";
}

static std::string status_str(chat::StatusEnum s) {
    switch (s) {
        case chat::ACTIVE:          return "Activo";
        case chat::DO_NOT_DISTURB:  return "Ocupado";
        case chat::INVISIBLE:       return "AFK";
        default:                    return "Desconocido";
    }
}

static std::string status_badge(chat::StatusEnum s) {
    switch (s) {
        case chat::ACTIVE:         return CLR_GREEN  "[+]" CLR_RESET;
        case chat::DO_NOT_DISTURB: return CLR_YELLOW "[-]" CLR_RESET;
        case chat::INVISIBLE:      return CLR_DIM    "[ ]" CLR_RESET;
        default:                   return             "[?]";
    }
}

static bool client_send(uint8_t type, const google::protobuf::Message& msg) {
    std::lock_guard<std::mutex> lk(g_send_mtx);
    return send_message(g_sockfd, type, msg);
}

// Obtiene la IP local leyendo el socket después de conectar
static std::string local_ip_from_socket(int sockfd) {
    sockaddr_in local{};
    socklen_t   len = sizeof(local);
    if (getsockname(sockfd, reinterpret_cast<sockaddr*>(&local), &len) == 0)
        return inet_ntoa(local.sin_addr);
    return "0.0.0.0";
}

static void print_banner() {
    std::string line(44, '-');
    print(CLR_BLUE CLR_BOLD
          "\n  CC3064 Sistemas Operativos -- Chat\n" CLR_RESET
          CLR_BLUE "  " + line + CLR_RESET "\n"
          "  Usuario : " CLR_BOLD + g_username + CLR_RESET "\n"
          "  IP      : " CLR_DIM  + g_my_ip   + CLR_RESET "\n"
          CLR_BLUE "  " + line + CLR_RESET);
}

static void print_help() {
    std::string sep(38, '-');
    print(CLR_BOLD "\n  Comandos disponibles\n" CLR_RESET
          "  " + sep + "\n"
          "  " CLR_YELLOW "<mensaje>" CLR_RESET "              Chat general\n"
          "  " CLR_YELLOW "/dm <user> <msg>" CLR_RESET "       Mensaje directo\n"
          "  " CLR_YELLOW "/status <estado>" CLR_RESET "       Activo | Ocupado | AFK\n"
          "  " CLR_YELLOW "/list" CLR_RESET "                  Ver usuarios conectados\n"
          "  " CLR_YELLOW "/info <usuario>" CLR_RESET "        Info de un usuario\n"
          "  " CLR_YELLOW "/help" CLR_RESET "                  Mostrar esta ayuda\n"
          "  " CLR_YELLOW "/quit" CLR_RESET "                  Salir\n"
          "  " + sep);
}

// ─── Hilo receptor ────────────────────────────────────────────────────────────

static void receiver_thread(int sockfd) {
    while (g_running) {
        RecvResult res = recv_message(sockfd);
        if (!res.ok) {
            if (g_running) print(CLR_RED "\n  Conexion perdida con el servidor." CLR_RESET);
            g_running = false;
            return;
        }

        switch (res.type) {

            case 10: { // ServerResponse
                chat::ServerResponse r;
                if (r.ParseFromString(res.payload)) {
                    std::string color = r.is_successful() ? CLR_GREEN : CLR_RED;
                    print(timestamp() + " " + color + "* " + r.message() + CLR_RESET);
                }
                break;
            }

            case 11: { // AllUsers
                chat::AllUsers au;
                if (!au.ParseFromString(res.payload)) break;
                std::string sep(30, '-');
                std::string out = CLR_BOLD "\n  Usuarios conectados (" +
                                  std::to_string(au.usernames_size()) + ")\n" CLR_RESET
                                  "  " + sep;
                for (int i = 0; i < au.usernames_size(); i++) {
                    std::string name = au.usernames(i);
                    // pad username to 18 chars for alignment
                    if (name.size() < 18) name += std::string(18 - name.size(), ' ');
                    out += "\n  " + status_badge(au.status(i)) + " " +
                           CLR_BWHITE + name + CLR_RESET +
                           CLR_DIM + status_str(au.status(i)) + CLR_RESET;
                }
                out += "\n  " + sep;
                print(out);
                break;
            }

            case 12: { // ForDm — username_des = nombre del remitente
                chat::ForDm dm;
                if (dm.ParseFromString(res.payload))
                    print(timestamp() + CLR_CYAN CLR_BOLD " >> DM de " +
                          dm.username_des() + ": " CLR_RESET CLR_CYAN +
                          dm.message() + CLR_RESET);
                break;
            }

            case 13: { // BroadcastDelivery
                chat::BroadcastDelivery bd;
                if (bd.ParseFromString(res.payload))
                    print(timestamp() + " " CLR_YELLOW CLR_BOLD +
                          bd.username_origin() + CLR_RESET ": " + bd.message());
                break;
            }

            case 14: { // GetUserInfoResponse
                chat::GetUserInfoResponse info;
                if (!info.ParseFromString(res.payload)) break;
                std::string sep(30, '-');
                print(CLR_BOLD "\n  Informacion de usuario\n" CLR_RESET
                      "  " + sep + "\n"
                      "  Usuario  " CLR_BWHITE + info.username()   + CLR_RESET "\n"
                      "  IP       " CLR_DIM    + info.ip_address() + CLR_RESET "\n"
                      "  Estado   " + status_badge(info.status()) + " " + status_str(info.status()) + "\n"
                      "  " + sep);
                break;
            }

            default:
                print(timestamp() + CLR_RED " [?] tipo desconocido: " +
                      std::to_string(res.type) + CLR_RESET);
        }
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Uso: " << argv[0]
                  << " <username> <server_ip> <server_port>\n";
        return 1;
    }

    GOOGLE_PROTOBUF_VERIFY_VERSION;

    g_username          = argv[1];
    std::string srv_ip  = argv[2];
    int         srv_port = std::stoi(argv[3]);

    // ── Conexión ──────────────────────────────────────────────────────────────
    g_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sockfd < 0) { perror("socket"); return 1; }
#ifdef SO_NOSIGPIPE
    { int nosig = 1; setsockopt(g_sockfd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig)); }
#endif

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(static_cast<uint16_t>(srv_port));
    if (inet_pton(AF_INET, srv_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "IP del servidor inválida: " << srv_ip << "\n";
        return 1;
    }
    if (connect(g_sockfd,
                reinterpret_cast<sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        perror("connect"); return 1;
    }

    g_my_ip = local_ip_from_socket(g_sockfd);

    // ── Registro ──────────────────────────────────────────────────────────────
    chat::Register reg;
    reg.set_username(g_username);
    reg.set_ip(g_my_ip);
    client_send(1, reg);

    RecvResult reg_res = recv_message(g_sockfd);
    if (!reg_res.ok) {
        std::cerr << "Sin respuesta del servidor\n"; return 1;
    }
    chat::ServerResponse reg_resp;
    if (!reg_resp.ParseFromString(reg_res.payload) || !reg_resp.is_successful()) {
        std::cerr << "Registro fallido: " << reg_resp.message() << "\n";
        close(g_sockfd); return 1;
    }

    print_banner();
    print_help();

    // ── Hilo receptor ─────────────────────────────────────────────────────────
    std::thread recv_th(receiver_thread, g_sockfd);
    recv_th.detach();

    // ── Loop de entrada ───────────────────────────────────────────────────────
    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        if (!g_running) break;
        if (line.empty()) continue;

        if (line == "/help") {
            print_help();

        } else if (line == "/list") {
            chat::ListUsers lu;
            lu.set_username(g_username);
            lu.set_ip(g_my_ip);
            client_send(5, lu);

        } else if (line == "/quit") {
            chat::Quit q;
            q.set_quit(true);
            q.set_ip(g_my_ip);
            client_send(7, q);
            g_running = false;
            break;

        } else if (line.rfind("/dm ", 0) == 0) {
            std::string rest  = line.substr(4);
            size_t      space = rest.find(' ');
            if (space == std::string::npos) {
                print(CLR_RED "  Uso: /dm <usuario> <mensaje>" CLR_RESET);
                continue;
            }
            std::string target = rest.substr(0, space);
            std::string msg    = rest.substr(space + 1);
            chat::MessageDM dm;
            dm.set_message(msg);
            dm.set_status(chat::ACTIVE);
            dm.set_username_des(target);
            dm.set_ip(g_my_ip);
            client_send(3, dm);

        } else if (line.rfind("/status ", 0) == 0) {
            std::string      s = line.substr(8);
            chat::StatusEnum new_status;
            if      (s == "Activo")   new_status = chat::ACTIVE;
            else if (s == "Ocupado")  new_status = chat::DO_NOT_DISTURB;
            else if (s == "AFK")      new_status = chat::INVISIBLE;
            else {
                print(CLR_RED "  Estado invalido. Opciones: Activo, Ocupado, AFK" CLR_RESET);
                continue;
            }
            chat::ChangeStatus cs;
            cs.set_status(new_status);
            cs.set_username(g_username);
            cs.set_ip(g_my_ip);
            client_send(4, cs);

        } else if (line.rfind("/info ", 0) == 0) {
            std::string target = line.substr(6);
            chat::GetUserInfo gui;
            gui.set_username_des(target);
            gui.set_username(g_username);
            gui.set_ip(g_my_ip);
            client_send(6, gui);

        } else if (!line.empty() && line[0] == '/') {
            print(CLR_RED "  Comando desconocido. Escribe /help para ver los comandos." CLR_RESET);

        } else {
            // Enviar al chat general
            chat::MessageGeneral mg;
            mg.set_message(line);
            mg.set_status(chat::ACTIVE);
            mg.set_username_origin(g_username);
            mg.set_ip(g_my_ip);
            client_send(2, mg);
        }
    }

    close(g_sockfd);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
