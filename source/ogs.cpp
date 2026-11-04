#include "ogs.h"

#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include <switch.h>

#include "log.h"
#include "net/b64.h"
#include "net/net.h"
#include "net/ws.h"
#include "settings.h"

using json = nlohmann::json;

static const char* OGS_HOST = "online-go.com";
static const char* OGS_URL = "https://online-go.com";
static const char* AUTH_FILE = "sdmc:/switch/nxgo/ogs_auth.txt";
static const size_t MAX_EVENTS = 2000;

// Identifiant public de l'application OAuth enregistrée sur OGS (type "public",
// grant "password" : aucun secret). Remplaçable dans config.txt (ogs_client_id=).
#ifndef OGS_CLIENT_PASSWORD
#define OGS_CLIENT_PASSWORD ""
#endif

static uint64_t monoMs()
{
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static int64_t epochMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static std::string esc(const std::string& s)
{
    char* e = net_escape(s.c_str());
    std::string out = e ? e : "";
    free(e);
    return out;
}

static std::string base64url(const uint8_t* data, size_t len)
{
    std::string out(B64_ENCODED_LEN(len) + 1, '\0');
    b64_encode(data, len, &out[0]);
    out.resize(strlen(out.c_str()));
    for (char& c : out)
        c = c == '+' ? '-' : (c == '/' ? '_' : c);
    while (!out.empty() && out.back() == '=')
        out.pop_back();
    return out;
}

static std::string randomToken(size_t bytes)
{
    std::vector<uint8_t> buf(bytes);
    randomGet(buf.data(), bytes);
    return base64url(buf.data(), bytes);
}

static std::string clientPassword()
{
    return !g_settings.ogsClientPassword.empty() ? g_settings.ogsClientPassword : OGS_CLIENT_PASSWORD;
}

// Requête HTTPS vers OGS ; `body` non vide = POST de formulaire.
static bool http(const std::string& path, const std::string& body, const std::string& bearer, long& status, json& out)
{
    std::string url = path.rfind("https://", 0) == 0 ? path : std::string(OGS_URL) + path;
    std::string auth = "Authorization: Bearer " + bearer;
    std::vector<const char*> headers = {"Accept: application/json"};
    if (!bearer.empty())
        headers.push_back(auth.c_str());
    if (!body.empty())
        headers.push_back("Content-Type: application/x-www-form-urlencoded");
    headers.push_back(nullptr);

    NetResponse resp;
    bool ok = net_send(body.empty() ? "GET" : nullptr, url.c_str(), body.empty() ? nullptr : body.data(), body.size(),
                       headers.data(), &resp);
    status = resp.status;
    out = json();
    if (ok && resp.body && resp.len)
        out = json::parse(resp.body, resp.body + resp.len, nullptr, false);  // invalide : discarded
    if (!ok)
        log_printf("OGS : %s injoignable\n", path.c_str());
    else if (status >= 400)
        log_printf("OGS : %s -> HTTP %ld %.200s\n", path.c_str(), status, resp.body ? resp.body : "");
    net_response_free(&resp);
    return ok;
}

// ------------------------------------------------------------------ cycle de vie

Ogs::~Ogs()
{
    stop();
}

void Ogs::start()
{
    if (m_thread.joinable())
        return;
    net_init();
    tls_init();
    m_quit = false;
    m_thread = std::thread([this] { run(); });
    post([this] {
        if (loadAuth())
            openSession();
    });
}

void Ogs::stop()
{
    if (!m_thread.joinable())
        return;
    m_quit = true;
    m_thread.join();
    tls_exit();
    net_exit();
}

void Ogs::post(std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tasks.push_back(std::move(task));
}

void Ogs::send(const std::string& cmd, const json& data)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outgoing.push_back(json::array({cmd, data}).dump());
}

void Ogs::setError(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_error = msg;
}

std::string Ogs::error() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

std::string Ogs::username() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_username;
}

std::vector<OgsGameInfo> Ogs::games() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_games;
}

std::vector<OgsGameInfo> Ogs::history() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_history;
}

int64_t Ogs::serverNowMs() const
{
    return epochMs() + m_clockOffset;
}

bool Ogs::pollEvent(OgsEvent& ev)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_events.empty())
        return false;
    ev = std::move(m_events.front());
    m_events.pop_front();
    return true;
}

void Ogs::run()
{
    while (!m_quit) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_tasks.empty()) {
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
        }
        if (task) {
            task();
            continue;
        }

        uint64_t now = monoMs();
        if (m_wantSocket && !m_wsOpen && now >= m_nextConnectMs)
            connectSocket();

        if (!m_wsOpen) {
            svcSleepThread(30000000ULL);
            continue;
        }

        WsConn* ws = (WsConn*)m_ws;
        std::vector<std::string> out;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            out.swap(m_outgoing);
        }
        if (now - m_lastPingMs >= 10000) {
            out.push_back(json::array({"net/ping", {{"client", epochMs()}}}).dump());
            m_lastPingMs = now;
        }
        bool ok = true;
        for (const std::string& msg : out)
            ok = ok && ws_send_text(ws, msg.c_str());

        while (ok && ws_readable(ws, 30)) {
            u8* data = nullptr;
            size_t len = 0;
            if (!ws_recv(ws, &data, &len)) {
                ok = false;
                break;
            }
            handleMessage((const char*)data, len);
            free(data);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_outgoing.empty() || !m_tasks.empty())
                    break;  // répondre vite à l'interface
            }
        }
        if (!ok) {
            log_printf("OGS : websocket coupé, reconnexion dans %d s\n", m_reconnectDelay);
            closeSocket();
            m_nextConnectMs = monoMs() + m_reconnectDelay * 1000ULL;
            m_reconnectDelay = std::min(m_reconnectDelay * 2, 30);
        }
    }
    closeSocket();
}

// ------------------------------------------------------------------ jetons

bool Ogs::loadAuth()
{
    FILE* f = fopen(AUTH_FILE, "r");
    if (!f)
        return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        std::string key = line, val = eq + 1;
        if (key == "client")
            m_clientId = val;
        else if (key == "access")
            m_access = val;
        else if (key == "refresh")
            m_refresh = val;
        else if (key == "expires")
            m_expires = atoll(val.c_str());
    }
    fclose(f);
    return !m_access.empty();
}

void Ogs::saveAuth()
{
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    FILE* f = fopen(AUTH_FILE, "w");
    if (!f)
        return;
    fprintf(f, "client=%s\naccess=%s\nrefresh=%s\nexpires=%lld\n", m_clientId.c_str(), m_access.c_str(),
            m_refresh.c_str(), (long long)m_expires);
    fclose(f);
}

// POST /oauth2/token/ ; garde les jetons reçus.
bool Ogs::requestToken(const std::string& form)
{
    long status = 0;
    json r;
    if (!http("/oauth2/token/", form, "", status, r)) {
        setError(L("Cannot reach online-go.com (network or console date?)",
                   "online-go.com injoignable (réseau ou date de la console ?)"));
        return false;
    }
    if (status != 200 || !r.is_object() || !r.contains("access_token")) {
        std::string why = r.is_object() ? r.value("error_description", r.value("error", "")) : "";
        if (why == "invalid_grant" || why.find("credentials") != std::string::npos)
            why = L("wrong username or password", "identifiant ou mot de passe incorrect");
        setError(strf("%s (HTTP %ld%s%s)", L("Sign-in refused", "Connexion refusée"), status, why.empty() ? "" : ", ",
                      why.c_str()));
        return false;
    }
    m_access = r.value("access_token", "");
    m_refresh = r.value("refresh_token", "");
    m_expires = time(nullptr) + r.value("expires_in", 2592000);
    saveAuth();
    return true;
}

bool Ogs::refreshToken()
{
    if (m_refresh.empty() || m_clientId.empty())
        return false;
    log_printf("OGS : renouvellement du jeton\n");
    return requestToken("grant_type=refresh_token&refresh_token=" + esc(m_refresh) + "&client_id=" + esc(m_clientId));
}

// Profil et jeton du websocket (user_jwt) : /api/v1/ui/config avec le jeton OAuth.
bool Ogs::fetchConfig()
{
    long status = 0;
    json r;
    if (!http("/api/v1/ui/config", "", m_access, status, r))
        return false;
    if (status == 401 || status == 403)
        return false;
    if (status != 200 || !r.is_object() || !r.contains("user"))
        return false;
    const json& u = r["user"];
    if (u.value("anonymous", false) || !r.contains("user_jwt")) {
        log_printf("OGS : ui/config renvoie un invité (jeton OAuth non accepté)\n");
        return false;
    }
    m_jwt = r.value("user_jwt", "");
    m_userId = u.value("id", (int64_t)0);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_username = u.value("username", "");
    return true;
}

// Après obtention (ou relecture) des jetons : profil, websocket, liste des parties.
void Ogs::openSession()
{
    int gen = m_loginGen;
    setState(OgsState::LoggingIn);
    if (m_expires && m_expires - (int64_t)time(nullptr) < 24 * 3600)
        refreshToken();
    bool ok = fetchConfig();
    if (!ok && refreshToken())
        ok = fetchConfig();
    if (gen != m_loginGen)
        return;
    if (!ok) {
        if (error().empty())
            setError(L("Session expired or refused: please sign in again.",
                       "Session expirée ou refusée : reconnecte-toi."));
        m_access.clear();
        remove(AUTH_FILE);
        setState(OgsState::LoggedOut);
        return;
    }
    setError("");
    setState(OgsState::Online);
    log_printf("OGS : connecté en tant que %s (%lld)\n", username().c_str(), (long long)m_userId.load());
    m_wantSocket = true;
    m_nextConnectMs = 0;
    m_reconnectDelay = 1;
    loadGames();
}

// ------------------------------------------------------------------ connexion (interface)

bool Ogs::available() const
{
    return !clientPassword().empty();
}

void Ogs::loginPassword(const std::string& user, const std::string& pass)
{
    setError("");
    int gen = ++m_loginGen;
    setState(OgsState::LoggingIn);
    post([this, gen, user, pass] {
        if (gen != m_loginGen)
            return;
        m_clientId = clientPassword();
        bool ok = requestToken("grant_type=password&username=" + esc(user) + "&password=" + esc(pass) +
                               "&client_id=" + esc(m_clientId) + "&scope=" + esc("read write"));
        if (gen != m_loginGen)
            return;
        if (ok)
            openSession();
        else
            setState(OgsState::LoggedOut);
    });
}

void Ogs::cancelLogin()
{
    ++m_loginGen;
    setState(OgsState::LoggedOut);
}

void Ogs::logout()
{
    ++m_loginGen;
    m_searching = false;
    setState(OgsState::LoggedOut);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_games.clear();
        m_connectedGames.clear();
        m_events.clear();
        m_outgoing.clear();
        m_username.clear();
        m_error.clear();
    }
    post([this] {
        m_wantSocket = false;
        closeSocket();
        if (!m_access.empty()) {
            long status;
            json r;
            http("/oauth2/revoke_token/", "token=" + esc(m_access) + "&client_id=" + esc(m_clientId), "", status, r);
        }
        m_access.clear();
        m_refresh.clear();
        m_jwt.clear();
        m_userId = 0;
        remove(AUTH_FILE);
    });
}

// ------------------------------------------------------------------ parties

void Ogs::refreshGames()
{
    if (m_state != OgsState::Online)
        return;
    m_gamesLoading = true;
    post([this] { loadGames(); });
}

void Ogs::loadGames()
{
    m_gamesLoading = true;
    long status = 0;
    json r;
    std::vector<OgsGameInfo> list;
    if (http("/api/v1/ui/overview", "", m_access, status, r) && status == 200 && r.is_object() &&
        r.contains("active_games") && r["active_games"].is_array()) {
        for (const json& g : r["active_games"]) {
            try {
                OgsGameInfo info;
                info.id = g.value("id", (int64_t)0);
                info.name = g.value("name", "");
                info.width = g.value("width", 19);
                info.height = g.value("height", 19);
                if (g.contains("black") && g["black"].is_object()) {
                    info.black = g["black"].value("username", "");
                    info.blackId = g["black"].value("id", (int64_t)0);
                }
                if (g.contains("white") && g["white"].is_object()) {
                    info.white = g["white"].value("username", "");
                    info.whiteId = g["white"].value("id", (int64_t)0);
                }
                if (g.contains("json") && g["json"].is_object()) {
                    const json& d = g["json"];
                    if (d.contains("clock") && d["clock"].is_object())
                        info.myTurn = d["clock"].value("current_player", (int64_t)0) == m_userId;
                    if (d.contains("time_control") && d["time_control"].is_object())
                        info.speed = d["time_control"].value("speed", "");
                    if (d.contains("moves") && d["moves"].is_array())
                        info.moves = (int)d["moves"].size();
                }
                if (info.id)
                    list.push_back(info);
            } catch (const std::exception& e) {
                log_printf("OGS : partie illisible (%s)\n", e.what());
            }
        }
    } else if (status == 401) {
        refreshToken();
    }
    // À moi de jouer d'abord
    std::stable_sort(list.begin(), list.end(), [](const OgsGameInfo& a, const OgsGameInfo& b) { return a.myTurn > b.myTurn; });
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_games = list;
    }
    loadHistory();
    m_gamesLoading = false;
}

void Ogs::loadHistory()
{
    long status = 0;
    json r;
    std::string path = strf("/api/v1/players/%lld/games/?ended__isnull=false&annulled=false&ordering=-ended&page_size=40",
                            (long long)m_userId.load());
    if (!http(path, "", m_access, status, r) || status != 200 || !r.is_object() || !r.contains("results") ||
        !r["results"].is_array())
        return;
    std::vector<OgsGameInfo> list;
    for (const json& g : r["results"]) {
        try {
            OgsGameInfo info;
            info.id = g.value("id", (int64_t)0);
            info.name = g.value("name", "");
            info.width = g.value("width", 19);
            info.height = g.value("height", 19);
            const json& p = g.contains("players") && g["players"].is_object() ? g["players"] : json::object();
            for (int i = 0; i < 2; i++) {
                const char* key = i == 0 ? "black" : "white";
                if (!p.contains(key) || !p[key].is_object())
                    continue;
                (i == 0 ? info.black : info.white) = p[key].value("username", "");
                (i == 0 ? info.blackId : info.whiteId) = p[key].value("id", (int64_t)0);
            }
            bool blackLost = g.value("black_lost", false), whiteLost = g.value("white_lost", false);
            std::string outcome = g.value("outcome", "");
            const char* w = !blackLost && whiteLost ? "B" : (blackLost && !whiteLost ? "W" : "");
            if (!*w)
                info.result = outcome.empty() ? "-" : outcome;
            else if (outcome.find("esign") != std::string::npos)
                info.result = strf("%s+R", w);
            else if (outcome.find("imeout") != std::string::npos)
                info.result = strf("%s+T", w);
            else if (outcome.find("point") != std::string::npos)
                info.result = strf("%s+%g", w, atof(outcome.c_str()));
            else
                info.result = strf("%s+F", w);  // décision du serveur, forfait…
            bool meBlack = info.blackId == m_userId;
            if (*w)
                info.won = (meBlack && *w == 'B') || (!meBlack && *w == 'W') ? 1 : 0;
            info.date = g.value("ended", "").substr(0, 10);
            if (g.contains("time_control_parameters") && g["time_control_parameters"].is_string()) {
                json tc = json::parse(g["time_control_parameters"].get<std::string>(), nullptr, false);
                if (tc.is_object())
                    info.speed = tc.value("speed", "");
            }
            if (info.id)
                list.push_back(info);
        } catch (const std::exception& e) {
            log_printf("OGS : partie terminée illisible (%s)\n", e.what());
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_history = list;
}

void Ogs::downloadSgf(int64_t id)
{
    post([this, id] {
        uint64_t t0 = monoMs();
        log_printf("OGS : téléchargement du SGF %lld\n", (long long)id);
        std::string url = strf("%s/api/v1/games/%lld/sgf", OGS_URL, (long long)id);
        std::string auth = "Authorization: Bearer " + m_access;
        const char* headers[] = {auth.c_str(), nullptr};
        NetResponse resp;
        bool ok = net_send("GET", url.c_str(), nullptr, 0, headers, &resp) && resp.status == 200 && resp.body &&
                  resp.len > 10 && resp.body[0] == '(';
        std::string path = strf("%s/ogs-%lld.sgf", SGF_DIR, (long long)id);
        if (ok) {
            mkdir("sdmc:/switch", 0777);
            mkdir(APP_DIR, 0777);
            mkdir(SGF_DIR, 0777);
            FILE* f = fopen(path.c_str(), "wb");
            ok = f && fwrite(resp.body, 1, resp.len, f) == resp.len;
            if (f)
                fclose(f);
        }
        long status = resp.status;
        log_printf("OGS : SGF %lld -> HTTP %ld, %zu o, %llu ms\n", (long long)id, status, resp.len,
                   (unsigned long long)(monoMs() - t0));
        net_response_free(&resp);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ok)
            m_events.push_back({"sgf/ready", {{"path", path}, {"id", id}}});
        else
            m_events.push_back({"sgf/error", {{"message", strf("HTTP %ld", status)}}});
    });
}

void Ogs::findMatch(const std::string& size, const std::string& speed)
{
    std::string uuid = randomToken(16);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_automatchUuid = uuid;
    }
    m_searching = true;
    json prefs = {
        {"uuid", uuid},
        {"size_speed_options", json::array({{{"size", size}, {"speed", speed}, {"system", "byoyomi"}}})},
        {"lower_rank_diff", 3},
        {"upper_rank_diff", 3},
        {"rules", {{"condition", "no-preference"}, {"value", "japanese"}}},
        {"handicap", {{"condition", "no-preference"}, {"value", "enabled"}}},
    };
    send("automatch/find_match", prefs);
}

void Ogs::cancelMatch()
{
    std::string uuid;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uuid = m_automatchUuid;
    }
    m_searching = false;
    if (!uuid.empty())
        send("automatch/cancel", {{"uuid", uuid}});
}

void Ogs::gameConnect(int64_t id)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectedGames.insert(id);
        // Événements d'une autre partie restés en file : inutiles
        m_events.clear();
    }
    send("game/connect", {{"game_id", id}, {"chat", false}});
}

void Ogs::gameDisconnect(int64_t id)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectedGames.erase(id);
    }
    send("game/disconnect", {{"game_id", id}});
}

void Ogs::gameMove(int64_t id, int x, int y)
{
    std::string mv = x < 0 ? ".." : std::string(1, (char)('a' + x)) + (char)('a' + y);
    send("game/move", {{"game_id", id}, {"move", mv}});
}

void Ogs::gameResign(int64_t id)
{
    send("game/resign", {{"game_id", id}});
}

void Ogs::removedSet(int64_t id, bool removed, const std::string& stones)
{
    send("game/removed_stones/set", {{"game_id", id}, {"removed", removed}, {"stones", stones}});
}

void Ogs::removedAccept(int64_t id, const std::string& stones)
{
    send("game/removed_stones/accept", {{"game_id", id}, {"stones", stones}, {"strict_seki_mode", false}});
}

void Ogs::removedReject(int64_t id)
{
    send("game/removed_stones/reject", {{"game_id", id}});
}

// ------------------------------------------------------------------ websocket

void Ogs::connectSocket()
{
    if (m_jwt.empty())
        return;
    WsConn* ws = (WsConn*)calloc(1, sizeof(WsConn));
    if (!ws_connect(ws, OGS_HOST, 443, "/")) {
        free(ws);
        m_nextConnectMs = monoMs() + m_reconnectDelay * 1000ULL;
        m_reconnectDelay = std::min(m_reconnectDelay * 2, 30);
        return;
    }
    m_ws = ws;
    m_wsOpen = true;
    m_reconnectDelay = 1;
    log_printf("OGS : websocket ouvert\n");

    std::vector<std::string> first;
    first.push_back(json::array({"authenticate",
                                 {{"jwt", m_jwt},
                                  {"client", "nxgo"},
                                  {"client_version", "0.2"},
                                  {"user_agent", "NXGo (Nintendo Switch)"},
                                  {"language", g_settings.lang == Lang::FR ? "fr" : "en"}}})
                        .dump());
    first.push_back(json::array({"net/ping", {{"client", epochMs()}}}).dump());
    {
        // Après une coupure : on se réabonne aux parties ouvertes (le serveur renvoie gamedata)
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int64_t id : m_connectedGames)
            first.push_back(json::array({"game/connect", {{"game_id", id}, {"chat", false}}}).dump());
        if (m_searching) {
            m_searching = false;  // la recherche ne survit pas à la coupure
            m_events.push_back({"automatch/cancel", json::object()});
        }
    }
    m_lastPingMs = monoMs();
    for (const std::string& msg : first)
        ws_send_text(ws, msg.c_str());
}

void Ogs::closeSocket()
{
    if (m_ws) {
        ws_close((WsConn*)m_ws);
        free(m_ws);
        m_ws = nullptr;
    }
    m_wsOpen = false;
}

void Ogs::handleMessage(const char* text, size_t len)
{
    json msg = json::parse(text, text + len, nullptr, false);
    if (!msg.is_array() || msg.empty())
        return;
    if (!msg[0].is_string()) {
        // Réponse à une commande numérotée : seules les erreurs nous intéressent
        if (msg.size() >= 3 && msg[2].is_object())
            log_printf("OGS : erreur %s\n", msg[2].dump().c_str());
        return;
    }
    std::string name = msg[0];
    json data = msg.size() > 1 ? msg[1] : json();

    if (name == "net/pong") {
        if (data.is_object() && data.contains("client") && data.contains("server")) {
            int64_t sent = data["client"].get<int64_t>(), server = data["server"].get<int64_t>(), now = epochMs();
            m_clockOffset = server + (now - sent) / 2 - now;
        }
        return;
    }
    if (name == "automatch/start" || name == "automatch/cancel") {
        std::string uuid = data.is_object() ? data.value("uuid", "") : "";
        std::lock_guard<std::mutex> lock(m_mutex);
        if (uuid != m_automatchUuid)
            return;
        m_searching = false;
        m_events.push_back({name, data});
        return;
    }
    if (name == "active_game") {
        post([this] { loadGames(); });
        return;
    }
    if (name.compare(0, 5, "game/") == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_events.size() >= MAX_EVENTS)
            m_events.pop_front();
        m_events.push_back({name, std::move(data)});
    }
}
