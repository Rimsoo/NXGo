#pragma once

// Une partie de go : réglages, liste des coups et position courante.
//
// Les règles (prises, ko, suicide) viennent de KataGo (Board / BoardHistory).
// La liste des coups est conservée en entier ; `cursor` indique jusqu'où on la rejoue,
// ce qui permet de revenir en arrière pour revoir une partie sans perdre la suite.

#include <string>
#include <vector>

#include "game/board.h"
#include "game/boardhistory.h"
#include "game/rules.h"

// Qui joue une couleur. `Remote` sera un adversaire en ligne (OGS) : ses coups
// arrivent du réseau, et l'aide de l'IA est alors interdite (règle d'OGS).
enum class PlayerKind { Human, Engine, Remote };

enum class RulesKind { Japanese, Chinese };

struct GameSetup {
    int size = 19;
    RulesKind rules = RulesKind::Japanese;
    float komi = 6.5f;
    int handicap = 0;
    PlayerKind black = PlayerKind::Human;
    PlayerKind white = PlayerKind::Engine;
    int aiLevel = 5;
    std::string blackName, whiteName;
    // Faux pour une partie classée en ligne : pas d'analyse ni d'indice tant qu'elle dure.
    bool aiAssistAllowed = true;
};

// Décompte final.
struct ScoreResult {
    bool valid = false;
    float blackScore = 0, whiteScore = 0;  // komi compris pour Blanc
    int blackTerritory = 0, whiteTerritory = 0;
    int blackPrisoners = 0, whitePrisoners = 0;  // pierres adverses capturées ou mortes
    // Par intersection (y * taille + x) : 0 neutre, 1 Noir, 2 Blanc.
    std::vector<uint8_t> owner;
    std::vector<bool> dead;
};

enum class EndReason { None, Score, Resign, Other };  // Other : temps, annulation… (en ligne)

class Game {
public:
    void start(const GameSetup& setup);
    // Position de départ quelconque (partie en ligne : pierres posées, handicap libre).
    void startFrom(const GameSetup& setup, const Board& initial, Player first);
    bool loadSgf(const std::string& path, std::string& error);
    bool saveSgf(const std::string& path) const;

    const GameSetup& setup() const { return m_setup; }
    GameSetup& setup() { return m_setup; }
    int size() const { return m_setup.size; }
    const Board& board() const { return m_board; }
    const Board& initialBoard() const { return m_initialBoard; }
    Player initialPlayer() const { return m_initialPla; }
    const std::vector<Move>& moves() const { return m_moves; }
    Rules rules() const { return makeRules(); }
    const BoardHistory& history() const { return m_hist; }
    Player toPlay() const { return m_toPlay; }
    PlayerKind kindToPlay() const { return m_toPlay == P_BLACK ? m_setup.black : m_setup.white; }
    PlayerKind kindOf(Player p) const { return p == P_BLACK ? m_setup.black : m_setup.white; }

    int moveCount() const { return (int)m_moves.size(); }
    int cursor() const { return m_cursor; }
    bool atEnd() const { return m_cursor == (int)m_moves.size(); }
    // Dernier coup joué dans la position courante (NULL_LOC si aucun).
    Loc lastMove() const;
    Player lastMovePlayer() const;
    // Identifiant qui change à chaque modification de la position (pour l'IA et l'analyse).
    uint64_t positionId() const { return m_positionId; }

    bool isLegal(Loc loc) const;
    // Joue au curseur ; s'il y avait une suite, elle est remplacée.
    bool play(Loc loc);
    void pass() { play(Board::PASS_LOC); }
    // Ajoute en fin de partie un coup décidé ailleurs (serveur OGS), sans vérifier
    // la légalité ni l'alternance (pierres de handicap). Le curseur suit s'il était au bout.
    void appendForced(Loc loc, Player pla);
    // Deux passes consécutifs : la partie attend le décompte.
    bool bothPassed() const;

    // Navigation (revue) : ne modifie pas la liste des coups.
    void goTo(int index);
    void back() { goTo(m_cursor - 1); }
    void forward() { goTo(m_cursor + 1); }
    // Retire les coups après le curseur (après une annulation en partie).
    void truncate();

    // Fin de partie.
    EndReason endReason() const { return m_end; }
    Player winner() const { return m_winner; }
    const ScoreResult& score() const { return m_score; }
    void resign(Player loser);
    // Décompte à partir de l'estimation de territoire de KataGo (+1 Blanc, -1 Noir).
    void scoreWithOwnership(const std::vector<float>& ownership);
    // Décompte par simple remplissage, à partir des pierres marquées mortes par les joueurs
    // (phase de retrait des pierres d'OGS) : aucune IA. `dead` : (y * taille + x).
    // Une intersection vide marquée compte comme dame (neutre).
    ScoreResult scoreWithDead(const std::vector<bool>& dead) const;
    void resumePlay();  // reprend après un décompte contesté
    // Résultat décidé ailleurs (OGS). `score` peut être invalide ; `text` ex. "W+T".
    void setExternalResult(EndReason reason, Player winner, const ScoreResult& score, const std::string& text);
    bool finished() const { return m_end != EndReason::None; }
    std::string resultString() const;  // "B+3.5", "W+R"...

    static std::string coordString(Loc loc, int size);

private:
    void rebuild();
    Rules makeRules() const;

    GameSetup m_setup;
    Board m_initialBoard;
    Player m_initialPla = P_BLACK;
    std::vector<Move> m_moves;
    int m_cursor = 0;

    Board m_board;
    BoardHistory m_hist;
    Player m_toPlay = P_BLACK;
    uint64_t m_positionId = 1;

    EndReason m_end = EndReason::None;
    Player m_winner = C_EMPTY;
    ScoreResult m_score;
    std::string m_resultText;  // résultat imposé (OGS), prioritaire
};
