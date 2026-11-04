#include "board_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

static const char* COLUMNS = "ABCDEFGHJKLMNOPQRST";
static const GfxColor LINE = RGB(40, 28, 14);

static inline float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static inline uint32_t hashNoise(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static GfxColor lerpColor(int r0, int g0, int b0, int r1, int g1, int b1, float t, int alpha)
{
    return RGBA((int)(r0 + (r1 - r0) * t), (int)(g0 + (g1 - g0) * t), (int)(b0 + (b1 - b0) * t), alpha);
}

// Pierre ombrée : reflet en haut à gauche, bord antialiasé dans l'alpha.
static void makeStone(GfxImage& img, int r, bool black)
{
    int size = 2 * r + 2;
    gfx_image_free(&img);
    gfx_image_alloc(&img, size, size);
    float c = size / 2.0f;
    float hx = c - 0.35f * r, hy = c - 0.4f * r;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float cov = clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov <= 0)
                continue;
            float hdx = x + 0.5f - hx, hdy = y + 0.5f - hy;
            float d = clamp01(sqrtf(hdx * hdx + hdy * hdy) / (1.6f * r));
            GfxColor col = black ? lerpColor(110, 110, 116, 14, 14, 18, powf(d, 0.6f), (int)(cov * 255))
                              : lerpColor(255, 255, 252, 186, 184, 178, powf(d, 1.6f), (int)(cov * 255));
            img.px[y * size + x] = col;
        }
}

BoardView::~BoardView()
{
    gfx_image_free(&m_wood);
    gfx_image_free(&m_black);
    gfx_image_free(&m_white);
}

void BoardView::layout(int x, int y, int side, int size, bool coords)
{
    if (x == m_x && y == m_y && side == m_side && size == m_size && coords == m_coords && !m_dirty)
        return;
    m_x = x;
    m_y = y;
    m_side = side;
    m_size = size;
    m_coords = coords;
    float margin = coords ? 34.0f : 20.0f;
    m_cell = (side - 2 * margin) / (size - 1);
    float need = m_cell * 0.5f + (coords ? 26.0f : 6.0f);
    if (need > margin) {
        margin = need;
        m_cell = (side - 2 * margin) / (size - 1);
    }
    m_gridX = x + margin;
    m_gridY = y + margin;
    m_stoneRadius = (int)(m_cell * 0.48f);
    rebuildImages();
    m_dirty = false;
}

void BoardView::rebuildImages()
{
    gfx_image_free(&m_wood);
    gfx_image_alloc(&m_wood, m_side, m_side);
    for (int y = 0; y < m_side; y++)
        for (int x = 0; x < m_side; x++) {
            // Veinage : bandes ondulées + un peu de grain
            float t = x * 0.9f + 18.0f * sinf(y * 0.013f + x * 0.002f) + 6.0f * sinf(y * 0.05f);
            float g = sinf(t * 0.08f) * 0.5f + 0.5f;
            float n = (hashNoise(x, y) & 0xFF) / 255.0f - 0.5f;
            float k = 0.9f + 0.1f * g + n * 0.03f;
            int r = (int)(226 * k), gg = (int)(186 * k), b = (int)(116 * k);
            m_wood.px[y * m_side + x] = RGB(std::min(r, 255), std::min(gg, 255), std::min(b, 255));
        }
    makeStone(m_black, m_stoneRadius, true);
    makeStone(m_white, m_stoneRadius, false);
}

bool BoardView::contains(int px, int py) const
{
    return px >= m_x && px < m_x + m_side && py >= m_y && py < m_y + m_side;
}

Loc BoardView::hitTest(int x, int y) const
{
    if (!contains(x, y))
        return Board::NULL_LOC;
    int i = (int)lroundf((x - m_gridX) / m_cell);
    int j = (int)lroundf((y - m_gridY) / m_cell);
    if (i < 0 || j < 0 || i >= m_size || j >= m_size)
        return Board::NULL_LOC;
    return Location::getLoc(i, j, m_size);
}

void BoardView::drawStoneIcon(int cx, int cy, int radius, Player pla)
{
    gfx_disc(cx + 2, cy + 3, radius, RGBA(0, 0, 0, 70));
    float hx = cx - 0.35f * radius, hy = cy - 0.4f * radius;
    for (int y = cy - radius - 1; y <= cy + radius + 1; y++)
        for (int x = cx - radius - 1; x <= cx + radius + 1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = clamp01(radius - sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov <= 0)
                continue;
            float hdx = x + 0.5f - hx, hdy = y + 0.5f - hy;
            float d = clamp01(sqrtf(hdx * hdx + hdy * hdy) / (1.6f * radius));
            GfxColor col = pla == P_BLACK ? lerpColor(110, 110, 116, 14, 14, 18, powf(d, 0.6f), (int)(cov * 255))
                                       : lerpColor(255, 255, 252, 186, 184, 178, powf(d, 1.6f), (int)(cov * 255));
            gfx_rect(x, y, 1, 1, col);
        }
}

static std::string shortVisits(int64_t v)
{
    char buf[32];
    if (v >= 10000)
        snprintf(buf, sizeof(buf), "%lldk", (long long)(v / 1000));
    else if (v >= 1000)
        snprintf(buf, sizeof(buf), "%.1fk", v / 1000.0);
    else
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return buf;
}

void BoardView::draw(const Game& game, const BoardDrawOptions& opt)
{
    const Board& b = game.board();
    int n = m_size;
    // Ombre portée du goban puis le bois
    gfx_round_rect(m_x + 6, m_y + 8, m_side, m_side, 10, RGBA(0, 0, 0, 90));
    gfx_image_opaque(&m_wood, m_x, m_y);

    // Lignes : bord plus épais
    int x0 = (int)lroundf(px(0)), x1 = (int)lroundf(px(n - 1));
    int y0 = (int)lroundf(py(0)), y1 = (int)lroundf(py(n - 1));
    for (int i = 0; i < n; i++) {
        int t = (i == 0 || i == n - 1) ? 2 : 1;
        int lx = (int)lroundf(px(i)), ly = (int)lroundf(py(i));
        gfx_rect(lx - t / 2, y0, t, y1 - y0 + 1, LINE);
        gfx_rect(x0, ly - t / 2, x1 - x0 + 1, t, LINE);
    }
    // Hoshi
    std::vector<int> stars;
    if (n == 19)
        stars = {3, 9, 15};
    else if (n == 13)
        stars = {3, 6, 9};
    else if (n == 9)
        stars = {2, 4, 6};
    for (int sx : stars)
        for (int sy : stars) {
            if (n == 9 && (sx == 4) != (sy == 4))
                continue;  // 9x9 : 4 coins + centre seulement
            gfx_disc(px(sx), py(sy), std::max(2.5f, m_cell * 0.1f), LINE);
        }
    // Coordonnées
    if (m_coords) {
        int fs = 18;
        GfxColor cc = RGB(70, 50, 25);
        for (int i = 0; i < n; i++) {
            char col[2] = {COLUMNS[i], 0};
            int w = gfx_text_width(fs, col);
            gfx_text((int)px(i) - w / 2, m_y + 4, fs, cc, col);
            gfx_text((int)px(i) - w / 2, m_y + m_side - fs - 6, fs, cc, col);
            char row[12];
            snprintf(row, sizeof(row), "%d", n - i);
            w = gfx_text_width(fs, row);
            gfx_text(m_x + 15 - w / 2, (int)py(i) - fs / 2 - 1, fs, cc, row);
            gfx_text(m_x + m_side - 15 - w / 2, (int)py(i) - fs / 2 - 1, fs, cc, row);
        }
    }

    const ScoreResult* score = (opt.score && opt.score->valid) ? opt.score : nullptr;
    const std::vector<float>* own =
        (!score && opt.showOwnership && opt.analysis && opt.analysis->valid && (int)opt.analysis->ownership.size() == n * n)
            ? &opt.analysis->ownership
            : nullptr;

    // Territoire estimé sur les intersections vides
    if (own)
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                if (b.colors[Location::getLoc(x, y, n)] != C_EMPTY)
                    continue;
                float o = (*own)[y * n + x];
                float s = fabsf(o) * m_cell * 0.42f;
                if (s < 3)
                    continue;
                GfxColor c = o > 0 ? RGBA(255, 255, 255, 170) : RGBA(0, 0, 0, 150);
                gfx_rect((int)(px(x) - s / 2), (int)(py(y) - s / 2), (int)s, (int)s, c);
            }

    // Pierres
    int off = m_black.w / 2;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            Loc loc = Location::getLoc(x, y, n);
            Color c = b.colors[loc];
            if (c == C_EMPTY)
                continue;
            int cx = (int)lroundf(px(x)), cy = (int)lroundf(py(y));
            bool dead = score && score->dead[y * n + x];
            int alpha = dead ? 110 : 255;
            if (!dead)
                gfx_disc(cx + 1.5f, cy + 2.5f, (float)m_stoneRadius, RGBA(0, 0, 0, 60));
            gfx_image_alpha(c == C_BLACK ? &m_black : &m_white, cx - off, cy - off, alpha);
            if (own) {
                float o = (*own)[y * n + x];
                bool threatened = (c == C_BLACK && o > 0.3f) || (c == C_WHITE && o < -0.3f);
                if (threatened) {
                    float s = fabsf(o) * m_cell * 0.42f;
                    gfx_rect((int)(cx - s / 2), (int)(cy - s / 2), (int)s, (int)s,
                             c == C_BLACK ? RGBA(255, 255, 255, 190) : RGBA(0, 0, 0, 170));
                }
            }
        }

    // Décompte : territoires
    if (score)
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int i = y * n + x;
                Loc loc = Location::getLoc(x, y, n);
                if (b.colors[loc] != C_EMPTY && !score->dead[i])
                    continue;
                uint8_t ow = score->owner[i];
                if (!ow)
                    continue;
                float s = m_cell * 0.36f;
                gfx_rect((int)(px(x) - s / 2), (int)(py(y) - s / 2), (int)s, (int)s,
                         ow == 1 ? RGBA(10, 10, 14, 230) : RGBA(250, 250, 248, 240));
            }

    // Dernier coup
    Loc last = game.lastMove();
    if (last != Board::NULL_LOC && last != Board::PASS_LOC && b.colors[last] != C_EMPTY) {
        int lx = Location::getX(last, n), ly = Location::getY(last, n);
        GfxColor c = b.colors[last] == C_BLACK ? RGB(240, 240, 240) : RGB(30, 30, 30);
        gfx_ring(px(lx), py(ly), m_cell * 0.27f, std::max(2.0f, m_cell * 0.07f), c);
    }

    // Meilleurs coups de l'analyse
    if (opt.analysis && opt.analysis->valid && !score && !opt.analysis->moves.empty()) {
        const auto& moves = opt.analysis->moves;
        int64_t minVisits = std::max<int64_t>(2, opt.analysis->visits / 100);
        double best = moves[0].winrate;
        int shown = 0;
        int fs = std::max(12, (int)(m_cell * 0.36f));
        int fsSmall = std::max(10, (int)(m_cell * 0.26f));
        // Dessiné du moins bon au meilleur pour que le meilleur soit au-dessus
        std::vector<const AnalysisMove*> list;
        for (const AnalysisMove& m : moves) {
            if (m.loc == Board::PASS_LOC || m.loc == Board::NULL_LOC)
                continue;
            if (m.order > 0 && m.visits < minVisits)
                continue;
            list.push_back(&m);
            if (++shown >= 10)
                break;
        }
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            const AnalysisMove& m = **it;
            int x = Location::getX(m.loc, n), y = Location::getY(m.loc, n);
            double loss = best - m.winrate;
            GfxColor c;
            if (m.order == 0)
                c = RGBA(70, 160, 255, 235);
            else if (loss < 0.02)
                c = RGBA(90, 200, 100, 225);
            else if (loss < 0.05)
                c = RGBA(235, 205, 70, 225);
            else if (loss < 0.12)
                c = RGBA(240, 145, 55, 225);
            else
                c = RGBA(220, 75, 65, 225);
            float r = m_cell * 0.46f;
            gfx_disc(px(x), py(y), r, c);
            if (m.order == 0)
                gfx_ring(px(x), py(y), r, 2.5f, RGB(20, 60, 140));
            char wr[8];
            snprintf(wr, sizeof(wr), "%d", (int)lround(m.winrate * 100));
            std::string vs = shortVisits(m.visits);
            int w = gfx_text_width(fs, wr);
            gfx_text((int)px(x) - w / 2, (int)py(y) - fs + 2, fs, RGB(15, 15, 20), wr);
            w = gfx_text_width(fsSmall, vs.c_str());
            gfx_text((int)px(x) - w / 2, (int)py(y) + 1, fsSmall, RGB(25, 25, 30), vs.c_str());
        }
    }

    // Aperçu du coup (tactile ou manette)
    if (opt.ghost != Board::NULL_LOC && opt.ghost != Board::PASS_LOC && b.colors[opt.ghost] == C_EMPTY) {
        int gx = Location::getX(opt.ghost, n), gy = Location::getY(opt.ghost, n);
        int cx = (int)lroundf(px(gx)), cy = (int)lroundf(py(gy));
        if (opt.ghostLegal) {
            gfx_image_alpha(game.toPlay() == P_BLACK ? &m_black : &m_white, cx - off, cy - off, 140);
        } else {
            float r = m_cell * 0.25f;
            gfx_rect((int)(cx - r), cy - 2, (int)(2 * r), 4, RGB(200, 40, 40));
        }
        // Repères sur la ligne et la colonne : on voit où le doigt pose même s'il cache la pierre
        gfx_rect(cx - 1, y0, 3, y1 - y0, RGBA(40, 120, 230, 90));
        gfx_rect(x0, cy - 1, x1 - x0, 3, RGBA(40, 120, 230, 90));
    }

    // Curseur manette
    if (opt.cursor != Board::NULL_LOC) {
        int cxl = Location::getX(opt.cursor, n), cyl = Location::getY(opt.cursor, n);
        float r = m_cell * 0.52f;
        gfx_ring(px(cxl), py(cyl), r, 3.0f, RGB(40, 120, 230));
    }
}
