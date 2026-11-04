#pragma once

// Client online-go.com (OGS) : connexion au compte, liste des parties, recherche
// d'adversaire et échanges en temps réel (websocket) pendant une partie.
//
// Deux fils réseau : l'un pour les requêtes HTTP (connexion, listes, SGF), l'autre pour
// le websocket ; un websocket bloqué (Wi-Fi coupé, mise en veille) ne retarde donc
// jamais le reste, et un chien de garde le coupe s'il ne répond plus aux pings.
// L'interface appelle les méthodes publiques (rapides, sans attente) et lit les
// événements reçus avec pollEvent().
//
// Connexion : identifiant + mot de passe tapés sur la Switch, échangés contre un jeton
// OAuth2 d'OGS (grant "password"). Le mot de passe n'est jamais enregistré : seuls les
// jetons sont gardés dans sdmc:/switch/nxgo/ogs_auth.txt.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "external/nlohmann_json/json.hpp"

struct OgsGameInfo {
    int64_t id = 0;
    std::string name;
    std::string black, white;
    int64_t blackId = 0, whiteId = 0;
    int width = 19, height = 19;
    bool myTurn = false;
    std::string speed;  // "blitz", "rapid", "live", "correspondence"
    int moves = 0;
    // Parties terminées (historique)
    std::string result;  // "B+R", "W+3.5"…
    std::string date;    // AAAA-MM-JJ
    int won = -1;        // 1 gagnée, 0 perdue, -1 sans vainqueur
};

// Événement reçu du serveur, ex. {"game/123/move", {...}} ou {"automatch/start", {...}}.
struct OgsEvent {
    std::string name;
    nlohmann::json data;
};

enum class OgsState {
    LoggedOut,   // pas de compte (éventuellement avec un message d'erreur)
    LoggingIn,   // échange du mot de passe contre un jeton, lecture du profil
    Online,      // connecté (le websocket peut être en reconnexion : voir socketConnected)
};

class Ogs {
public:
    ~Ogs();
    void start();  // lance le fil réseau et reprend la session enregistrée
    void stop();

    OgsState state() const { return m_state; }
    std::string error() const;
    std::string username() const;
    int64_t userId() const { return m_userId; }
    bool socketConnected() const { return m_wsOpen; }

    // Connexion
    bool available() const;  // identifiant d'application OAuth connu
    void loginPassword(const std::string& user, const std::string& pass);
    void cancelLogin();
    void logout();

    // Parties en cours du joueur
    void refreshGames();
    bool gamesLoading() const { return m_gamesLoading; }
    std::vector<OgsGameInfo> games() const;
    std::vector<OgsGameInfo> history() const;  // dernières parties terminées
    // Télécharge le SGF d'une partie dans sdmc:/switch/nxgo/sgf/ ; événement
    // "sgf/ready" {"path"} ou "sgf/error" {"message"} à la fin.
    void downloadSgf(int64_t id);

    // Recherche automatique d'adversaire. size : "9x9"/"13x13"/"19x19",
    // speed : "blitz"/"rapid"/"live"/"correspondence".
    void findMatch(const std::string& size, const std::string& speed);
    void cancelMatch();
    bool searching() const { return m_searching; }

    // Partie
    void gameConnect(int64_t id);
    void gameDisconnect(int64_t id);
    void gameMove(int64_t id, int x, int y);  // x < 0 : passe
    void gameResign(int64_t id);
    void removedSet(int64_t id, bool removed, const std::string& stones);
    void removedAccept(int64_t id, const std::string& stones);
    void removedReject(int64_t id);

    bool pollEvent(OgsEvent& ev);
    // Heure du serveur (ms depuis 1970), corrigée du décalage d'horloge mesuré.
    int64_t serverNowMs() const;

private:
    void run();       // fil HTTP : tâches + chien de garde du websocket
    void runSocket();  // fil websocket
    void post(std::function<void()> task);
    void send(const std::string& cmd, const nlohmann::json& data);
    void setError(const std::string& msg);
    void setState(OgsState s) { m_state = s; }

    // Fil réseau uniquement
    bool loadAuth();
    void saveAuth();
    bool requestToken(const std::string& form);
    bool refreshToken();
    bool fetchConfig();
    void openSession();
    void loadGames();
    void loadHistory();
    void connectSocket();
    void closeSocket();
    void handleMessage(const char* text, size_t len);

    std::thread m_thread, m_wsThread;
    std::atomic<bool> m_quit{false};
    std::atomic<OgsState> m_state{OgsState::LoggedOut};
    std::atomic<bool> m_wsOpen{false};
    std::atomic<bool> m_searching{false};
    std::atomic<bool> m_gamesLoading{false};
    std::atomic<int64_t> m_userId{0};
    std::atomic<int64_t> m_clockOffset{0};
    std::atomic<int> m_loginGen{0};  // change à chaque annulation : les tâches périmées s'arrêtent

    mutable std::mutex m_mutex;  // protège tout ce qui suit
    std::deque<std::function<void()>> m_tasks;
    std::vector<std::string> m_outgoing;
    std::deque<OgsEvent> m_events;
    std::vector<OgsGameInfo> m_games, m_history;
    std::set<int64_t> m_connectedGames;
    std::string m_error, m_username;
    std::string m_automatchUuid;

    // Fil HTTP uniquement
    std::string m_clientId, m_access, m_refresh;
    int64_t m_expires = 0;
    std::string m_jwt;  // écrit par le fil HTTP, lu par le fil websocket (sous m_mutex)

    // Fil websocket
    std::atomic<bool> m_wantSocket{false};
    std::atomic<bool> m_reconnectNow{false};
    std::atomic<int> m_wsFd{-1};             // socket TCP du websocket (pour le couper)
    std::atomic<uint64_t> m_lastRecvMs{0};   // dernier message reçu (le serveur répond aux pings)
    uint64_t m_nextConnectMs = 0, m_lastPingMs = 0;
    int m_reconnectDelay = 1;
    void* m_ws = nullptr;  // WsConn
};
