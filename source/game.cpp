#include "game.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>

#include "dataio/sgf.h"

static const char* COLUMNS = "ABCDEFGHJKLMNOPQRST";  // pas de I, comme sur un vrai goban

// Points de handicap traditionnels, dans l'ordre où on les ajoute.
static std::vector<std::pair<int, int>> handicapPoints(int size, int count)
{
    int e = size >= 13 ? 3 : 2;
    int m = size / 2, f = size - 1 - e;
    std::pair<int, int> ur{f, e}, ll{e, f}, lr{f, f}, ul{e, e};
    std::pair<int, int> left{e, m}, right{f, m}, top{m, e}, bottom{m, f}, center{m, m};
    std::vector<std::pair<int, int>> pts;
    if (count < 2)
        return pts;
    pts = {ur, ll};
    if (count >= 3)
        pts.push_back(lr);
    if (count >= 4)
        pts.push_back(ul);
    if (count == 5 || count == 7 || count == 9)
        pts.push_back(center);
    if (count >= 6) {
        pts.push_back(left);
        pts.push_back(right);
    }
    if (count >= 8) {
        pts.push_back(top);
        pts.push_back(bottom);
    }
    return pts;
}

Rules Game::makeRules() const
{
    Rules r = Rules::parseRules(m_setup.rules == RulesKind::Chinese ? "chinese" : "japanese");
    r.komi = m_setup.komi;
    return r;
}

void Game::start(const GameSetup& setup)
{
    m_setup = setup;
    m_initialBoard = Board(setup.size, setup.size);
    m_initialPla = P_BLACK;
    if (setup.handicap >= 2) {
        for (auto& p : handicapPoints(setup.size, setup.handicap))
            m_initialBoard.setStone(Location::getLoc(p.first, p.second, setup.size), P_BLACK);
        m_initialPla = P_WHITE;
    }
    m_moves.clear();
    m_cursor = 0;
    m_end = EndReason::None;
    m_winner = C_EMPTY;
    m_score = ScoreResult();
    m_resultText.clear();
    rebuild();
}

void Game::startFrom(const GameSetup& setup, const Board& initial, Player first)
{
    m_setup = setup;
    m_initialBoard = initial;
    m_initialPla = first;
    m_moves.clear();
    m_cursor = 0;
    m_end = EndReason::None;
    m_winner = C_EMPTY;
    m_score = ScoreResult();
    m_resultText.clear();
    rebuild();
}

void Game::appendForced(Loc loc, Player pla)
{
    bool follow = atEnd();
    m_moves.push_back(Move(loc, pla));
    if (!follow)
        return;
    m_cursor++;
    if (loc != Board::PASS_LOC && !m_board.isOnBoard(loc))
        loc = Board::PASS_LOC;
    if (loc != Board::PASS_LOC && m_board.colors[loc] != C_EMPTY) {
        rebuild();  // position différente de celle du serveur : on rejoue tout
        return;
    }
    m_hist.makeBoardMoveAssumeLegal(m_board, loc, pla, NULL, true);
    m_toPlay = getOpp(pla);
    m_positionId++;
}

void Game::setExternalResult(EndReason reason, Player winner, const ScoreResult& score, const std::string& text)
{
    m_end = reason;
    m_winner = winner;
    m_score = score;
    m_resultText = text;
}

void Game::rebuild()
{
    m_board = m_initialBoard;
    m_hist = BoardHistory(m_board, m_initialPla, makeRules(), 0, BoardHistoryModes());
    m_toPlay = m_initialPla;
    for (int i = 0; i < m_cursor; i++) {
        const Move& mv = m_moves[i];
        if (mv.loc != Board::PASS_LOC && m_board.colors[mv.loc] != C_EMPTY) {
            m_toPlay = getOpp(mv.pla);  // coup impossible ici (règles différentes) : ignoré
            continue;
        }
        // preventEncore : la partie s'arrête aux deux passes, le décompte est fait par nous
        m_hist.makeBoardMoveAssumeLegal(m_board, mv.loc, mv.pla, NULL, true);
        m_toPlay = getOpp(mv.pla);
    }
    m_positionId++;
}

Loc Game::lastMove() const
{
    return m_cursor > 0 ? m_moves[m_cursor - 1].loc : Board::NULL_LOC;
}

Player Game::lastMovePlayer() const
{
    return m_cursor > 0 ? m_moves[m_cursor - 1].pla : C_EMPTY;
}

bool Game::isLegal(Loc loc) const
{
    if (loc == Board::PASS_LOC)
        return true;
    return m_hist.isLegal(m_board, loc, m_toPlay);
}

bool Game::play(Loc loc)
{
    if (!isLegal(loc))
        return false;
    m_moves.resize(m_cursor);
    m_moves.push_back(Move(loc, m_toPlay));
    m_cursor++;
    m_hist.makeBoardMoveAssumeLegal(m_board, loc, m_toPlay, NULL, true);
    m_toPlay = getOpp(m_toPlay);
    m_end = EndReason::None;
    m_resultText.clear();
    m_score = ScoreResult();
    m_positionId++;
    return true;
}

bool Game::bothPassed() const
{
    return m_cursor >= 2 && m_moves[m_cursor - 1].loc == Board::PASS_LOC && m_moves[m_cursor - 2].loc == Board::PASS_LOC;
}

void Game::goTo(int index)
{
    if (index < 0)
        index = 0;
    if (index > (int)m_moves.size())
        index = (int)m_moves.size();
    if (index == m_cursor)
        return;
    m_cursor = index;
    rebuild();
}

void Game::truncate()
{
    m_moves.resize(m_cursor);
    m_end = EndReason::None;
    m_score = ScoreResult();
}

void Game::resign(Player loser)
{
    m_end = EndReason::Resign;
    m_winner = getOpp(loser);
}

void Game::resumePlay()
{
    m_resultText.clear();
    m_end = EndReason::None;
    m_score = ScoreResult();
}

void Game::scoreWithOwnership(const std::vector<float>& ownership)
{
    int n = m_setup.size;
    ScoreResult s;
    s.owner.assign(n * n, 0);
    s.dead.assign(n * n, false);
    if ((int)ownership.size() < n * n)
        return;

    int blackStones = 0, whiteStones = 0;
    int deadBlack = 0, deadWhite = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int i = y * n + x;
            float o = ownership[i];  // +1 Blanc, -1 Noir
            Color c = m_board.colors[Location::getLoc(x, y, n)];
            if (c == C_BLACK) {
                if (o > 0.4f) {  // pierre noire en territoire blanc : morte
                    s.dead[i] = true;
                    s.owner[i] = 2;
                    deadBlack++;
                } else {
                    s.owner[i] = 1;
                    blackStones++;
                }
            } else if (c == C_WHITE) {
                if (o < -0.4f) {
                    s.dead[i] = true;
                    s.owner[i] = 1;
                    deadWhite++;
                } else {
                    s.owner[i] = 2;
                    whiteStones++;
                }
            } else if (o > 0.5f) {
                s.owner[i] = 2;
            } else if (o < -0.5f) {
                s.owner[i] = 1;
            }
        }
    for (int i = 0; i < n * n; i++) {
        bool emptyOrDead = s.dead[i] || m_board.colors[Location::getLoc(i % n, i / n, n)] == C_EMPTY;
        if (emptyOrDead && s.owner[i] == 1)
            s.blackTerritory++;
        else if (emptyOrDead && s.owner[i] == 2)
            s.whiteTerritory++;
    }
    // numWhiteCaptures = pierres blanches capturées (donc prises par Noir)
    s.blackPrisoners = m_board.numWhiteCaptures + deadWhite;
    s.whitePrisoners = m_board.numBlackCaptures + deadBlack;

    if (m_setup.rules == RulesKind::Chinese) {
        s.blackScore = (float)(s.blackTerritory + blackStones);
        s.whiteScore = (float)(s.whiteTerritory + whiteStones) + m_setup.komi;
    } else {
        s.blackScore = (float)(s.blackTerritory + s.blackPrisoners);
        s.whiteScore = (float)(s.whiteTerritory + s.whitePrisoners) + m_setup.komi;
    }
    s.valid = true;
    m_score = s;
    m_end = EndReason::Score;
    m_winner = s.blackScore > s.whiteScore ? P_BLACK : (s.whiteScore > s.blackScore ? P_WHITE : C_EMPTY);
}

ScoreResult Game::scoreWithDead(const std::vector<bool>& dead) const
{
    int n = m_setup.size;
    ScoreResult s;
    s.owner.assign(n * n, 0);
    s.dead.assign(n * n, false);
    if ((int)dead.size() < n * n)
        return s;
    auto colorAt = [&](int i) { return m_board.colors[Location::getLoc(i % n, i / n, n)]; };

    int blackStones = 0, whiteStones = 0, deadBlack = 0, deadWhite = 0;
    for (int i = 0; i < n * n; i++) {
        Color c = colorAt(i);
        if (c != C_EMPTY && dead[i]) {
            s.dead[i] = true;
            c == C_BLACK ? deadBlack++ : deadWhite++;
        } else if (c == C_BLACK) {
            s.owner[i] = 1;
            blackStones++;
        } else if (c == C_WHITE) {
            s.owner[i] = 2;
            whiteStones++;
        }
    }

    // Zones vides (pierres mortes comprises) : à une couleur si elles ne touchent qu'elle
    std::vector<int> region(n * n, -1);
    std::vector<int> stack, cells;
    int regionId = 0;
    for (int start = 0; start < n * n; start++) {
        bool open = colorAt(start) == C_EMPTY || s.dead[start];
        if (!open || region[start] >= 0)
            continue;
        bool touchB = false, touchW = false;
        cells.clear();
        stack.assign(1, start);
        region[start] = regionId;
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            cells.push_back(i);
            int x = i % n, y = i / n;
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (auto& p : nb) {
                if (p[0] < 0 || p[0] >= n || p[1] < 0 || p[1] >= n)
                    continue;
                int j = p[1] * n + p[0];
                Color c = colorAt(j);
                if (c == C_EMPTY || s.dead[j]) {
                    if (region[j] < 0) {
                        region[j] = regionId;
                        stack.push_back(j);
                    }
                } else if (c == C_BLACK) {
                    touchB = true;
                } else {
                    touchW = true;
                }
            }
        }
        uint8_t owner = touchB && !touchW ? 1 : (touchW && !touchB ? 2 : 0);
        for (int i : cells) {
            bool dame = dead[i] && colorAt(i) == C_EMPTY;
            s.owner[i] = dame ? 0 : owner;
            if (owner == 1 && !dame)
                s.blackTerritory++;
            else if (owner == 2 && !dame)
                s.whiteTerritory++;
        }
        regionId++;
    }

    s.blackPrisoners = m_board.numWhiteCaptures + deadWhite;
    s.whitePrisoners = m_board.numBlackCaptures + deadBlack;
    if (m_setup.rules == RulesKind::Chinese) {
        s.blackScore = (float)(s.blackTerritory + blackStones);
        s.whiteScore = (float)(s.whiteTerritory + whiteStones) + m_setup.komi;
    } else {
        s.blackScore = (float)(s.blackTerritory + s.blackPrisoners);
        s.whiteScore = (float)(s.whiteTerritory + s.whitePrisoners) + m_setup.komi;
    }
    s.valid = true;
    return s;
}

std::string Game::resultString() const
{
    if (!m_resultText.empty())
        return m_resultText;
    if (m_end == EndReason::Resign)
        return m_winner == P_BLACK ? "B+R" : "W+R";
    if (m_end == EndReason::Score && m_score.valid) {
        float d = std::fabs(m_score.blackScore - m_score.whiteScore);
        if (m_winner == C_EMPTY)
            return "0";
        char buf[32];
        snprintf(buf, sizeof(buf), "%s+%g", m_winner == P_BLACK ? "B" : "W", d);
        return buf;
    }
    return "";
}

std::string Game::coordString(Loc loc, int size)
{
    if (loc == Board::PASS_LOC)
        return "pass";
    if (loc == Board::NULL_LOC)
        return "";
    int x = Location::getX(loc, size), y = Location::getY(loc, size);
    char buf[8];
    snprintf(buf, sizeof(buf), "%c%d", COLUMNS[x], size - y);
    return buf;
}

// ------------------------------------------------------------------ SGF

static std::string sgfEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == ']' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}

static std::string sgfPoint(Loc loc, int size)
{
    if (loc == Board::PASS_LOC)
        return "";
    int x = Location::getX(loc, size), y = Location::getY(loc, size);
    return std::string(1, (char)('a' + x)) + (char)('a' + y);
}

bool Game::saveSgf(const std::string& path) const
{
    std::ofstream out(path);
    if (!out)
        return false;
    int n = m_setup.size;
    char date[16];
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%Y-%m-%d", localtime(&now));
    out << "(;GM[1]FF[4]CA[UTF-8]AP[NXGo:0.1]SZ[" << n << "]KM[" << m_setup.komi << "]";
    out << "RU[" << (m_setup.rules == RulesKind::Chinese ? "Chinese" : "Japanese") << "]DT[" << date << "]";
    if (!m_setup.blackName.empty())
        out << "PB[" << sgfEscape(m_setup.blackName) << "]";
    if (!m_setup.whiteName.empty())
        out << "PW[" << sgfEscape(m_setup.whiteName) << "]";
    std::string stones;
    int hc = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            Loc loc = Location::getLoc(x, y, n);
            if (m_initialBoard.colors[loc] == C_BLACK) {
                stones += "[" + sgfPoint(loc, n) + "]";
                hc++;
            }
        }
    if (hc > 0)
        out << "HA[" << hc << "]AB" << stones;
    std::string result = resultString();
    if (!result.empty())
        out << "RE[" << result << "]";
    out << "\n";
    for (size_t i = 0; i < m_moves.size(); i++) {
        out << ";" << (m_moves[i].pla == P_BLACK ? "B" : "W") << "[" << sgfPoint(m_moves[i].loc, n) << "]";
        if (i % 10 == 9)
            out << "\n";
    }
    out << ")\n";
    return (bool)out;
}

bool Game::loadSgf(const std::string& path, std::string& error)
{
    std::unique_ptr<CompactSgf> sgf;
    try {
        sgf = CompactSgf::loadFile(path);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (sgf->xSize != sgf->ySize || sgf->xSize < 5 || sgf->xSize > 19) {
        error = "unsupported board size";
        return false;
    }

    Rules rules;
    try {
        rules = sgf->getRulesOrFailAllowUnspecified(Rules::parseRules("japanese"));
    } catch (const std::exception&) {
        rules = Rules::parseRules("japanese");
    }

    GameSetup setup;
    setup.size = sgf->xSize;
    setup.rules = rules.scoringRule == Rules::SCORING_AREA ? RulesKind::Chinese : RulesKind::Japanese;
    setup.komi = rules.komi;
    setup.black = setup.white = PlayerKind::Human;
    if (sgf->rootNode.hasProperty("PB"))
        setup.blackName = sgf->rootNode.getSingleProperty("PB");
    if (sgf->rootNode.hasProperty("PW"))
        setup.whiteName = sgf->rootNode.getSingleProperty("PW");
    std::string result;
    if (sgf->rootNode.hasProperty("RE"))
        result = sgf->rootNode.getSingleProperty("RE");

    Board board;
    BoardHistory hist;
    Player nextPla;
    try {
        sgf->setupInitialBoardAndHist(rules, board, nextPla, hist, BoardHistoryModes());
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }

    m_setup = setup;
    m_setup.handicap = BoardHistory::numHandicapStonesOnBoard(board);
    m_initialBoard = board;
    m_initialPla = nextPla;
    m_moves.clear();
    m_end = EndReason::None;
    m_resultText.clear();
    m_score = ScoreResult();

    // On garde les coups jusqu'au premier illégal (fichier abîmé ou règles différentes).
    BoardHistory check(board, nextPla, makeRules(), 0, BoardHistoryModes());
    for (const Move& mv : sgf->moves) {
        if (mv.loc != Board::PASS_LOC && !check.isLegal(board, mv.loc, mv.pla))
            break;
        check.makeBoardMoveAssumeLegal(board, mv.loc, mv.pla, NULL, true);
        m_moves.push_back(mv);
    }
    m_cursor = (int)m_moves.size();
    m_resultText = result;  // affiché en revue (ex. "W+R")
    rebuild();
    return true;
}
