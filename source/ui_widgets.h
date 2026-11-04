#pragma once

// Thème et éléments d'interface communs aux écrans (définis dans ui.cpp).

#include <string>
#include <utility>
#include <vector>

#include "gfx.h"
#include "input.h"

static const GfxColor BG = RGB(24, 27, 34);
static const GfxColor PANEL = RGB(36, 40, 50);
static const GfxColor PANEL_HI = RGB(50, 56, 70);
static const GfxColor TEXT = RGB(236, 238, 241);
static const GfxColor DIM = RGB(150, 156, 168);
static const GfxColor ACCENT = RGB(74, 163, 255);
static const GfxColor WOOD = RGB(226, 186, 116);
static const GfxColor DANGER = RGB(235, 90, 80);
static const GfxColor GOOD = RGB(110, 200, 120);

static const int BOARD_SIDE = 704;
static const int BOARD_X = 8, BOARD_Y = 8;
static const int PANEL_X = 736, PANEL_W = 528;
static const int GRAPH_Y = 404, GRAPH_H = 134;  // courbe de score (revue)

struct Rect {
    int x, y, w, h;
    bool tapped(const Input& in) const { return in.tapIn(x, y, w, h); }
};

// Ligne "réglage" : libellé à gauche, valeur entre deux flèches à droite.
struct ValueRow {
    Rect row, left, right;
};

u64 nowMs();
int drawGlyph(int x, int y, const char* label, int size = 26);
void drawHints(const std::vector<std::pair<const char*, std::string>>& hints, int y = 684);
void drawButton(const Rect& r, const std::string& label, const char* glyph, bool focused, bool enabled = true,
                bool active = false);
void drawTitle(const char* title, const char* subtitle = nullptr);
ValueRow valueRowRect(int index, int top = 150);
void drawValueRow(const ValueRow& r, const char* label, const std::string& value, bool focused, bool enabled = true);
int valueRowDelta(const Input& in, const ValueRow& r, bool focused);
