#pragma once

// Dessin du goban : bois, lignes, pierres, marqueurs d'analyse et de décompte.
// Les images (bois, pierres) sont calculées une fois par taille puis recopiées.

#include "engine.h"
#include "game.h"
#include "gfx.h"

struct BoardDrawOptions {
    bool showCoords = true;
    Loc cursor = Board::NULL_LOC;   // curseur manette
    Loc ghost = Board::NULL_LOC;    // pierre fantôme (aperçu du coup)
    bool ghostLegal = true;
    const AnalysisSnapshot* analysis = nullptr;  // meilleurs coups à afficher
    bool showOwnership = false;
    const ScoreResult* score = nullptr;           // territoires et pierres mortes
};

class BoardView {
public:
    ~BoardView();
    // Zone carrée (x, y, côté) réservée au goban, pour un plateau de `size` lignes.
    void layout(int x, int y, int side, int size, bool coords);
    void draw(const Game& game, const BoardDrawOptions& opt);
    // Intersection sous le point (px, py), ou NULL_LOC hors du goban.
    Loc hitTest(int px, int py) const;
    bool contains(int px, int py) const;
    float cell() const { return m_cell; }

    // Petite pierre pour l'interface (cartes des joueurs).
    static void drawStoneIcon(int cx, int cy, int radius, Player pla);

private:
    void rebuildImages();
    float px(int i) const { return m_gridX + i * m_cell; }
    float py(int j) const { return m_gridY + j * m_cell; }

    int m_x = 0, m_y = 0, m_side = 0, m_size = 0;
    bool m_coords = true;
    float m_cell = 0, m_gridX = 0, m_gridY = 0;
    int m_stoneRadius = 0;
    GfxImage m_wood = {};
    GfxImage m_black = {}, m_white = {};
    bool m_dirty = true;
};
