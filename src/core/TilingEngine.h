#pragma once

#include <QObject>
#include <QRect>
#include <QSize>
#include <QList>
#include <QString>
#include <QHash>
#include <functional>

class Window;

// ─────────────────────────────────────────────────────────────────────────────
// TilingLayout
//
// Enumeration of every layout algorithm the engine knows how to run.
// Stored per-workspace so each workspace can use a different layout.
// ─────────────────────────────────────────────────────────────────────────────
enum class TilingLayout {
    Spiral,     ///< Fibonacci / golden-ratio spiral — each new window takes
    ///<   half of the remaining space, cycling through four split
    ///<   directions (right → down → left → up).  Similar to bspwm.
    Tall,       ///< One master column on the left (width = masterRatio × area)
    ///<   + a vertical stack of equal-height slaves on the right.
    Wide,       ///< One master row on the top (height = masterRatio × area)
    ///<   + a horizontal row of equal-width slaves on the bottom.
    Grid,       ///< Evenly divide the area into a near-square grid.  The last
    ///<   row is centred when the window count is not a perfect square.
    Dwindle,    ///< Each successive split alternates horizontal / vertical and
    ///<   halves the remaining rectangle, producing a corner cascade.
    Monocle,    ///< All tiled windows share the same full-area rectangle and
    ///<   are stacked; only the active one is visible at a time.
    Centered,   ///< Single window: centred at ~72 % of the work area.
    ///<   Multiple windows: falls back to Spiral.
    ThreeColumn,///< Three equal-width columns; master goes in the centre.
    BSP         ///< Binary-space partition; each split direction is stored in
    ///<   the per-window tile slot for stable re-layout.
};

// ─────────────────────────────────────────────────────────────────────────────
// TileResult
//
// The output of one tiling pass for a single window.
// The compositor animates the window from its current geometry to
// targetGeometry and sets its z-order to zOrder.
// ─────────────────────────────────────────────────────────────────────────────
struct TileResult {
    Window* window;          ///< The window being placed.
    QRect   targetGeometry;  ///< Destination rect in compositor coordinates.
    int     zOrder;          ///< Render order within the workspace (0 = bottom).
};

// ─────────────────────────────────────────────────────────────────────────────
// GapPolicy
//
// Controls how inner and outer gaps are applied.  The engine honours this so
// individual layouts do not need to hard-code gap logic.
// ─────────────────────────────────────────────────────────────────────────────
struct GapPolicy {
    int  inner       = 10;   ///< Pixels between adjacent tiled windows.
    int  outer       = 12;   ///< Pixels between the outermost windows and the
    ///<   work-area boundary.
    bool smartGaps   = true; ///< When true: suppress all gaps if only one
    ///<   window is tiled (fills the full work area).
    bool smartBorders= true; ///< When true: suppress window borders if only one
    ///<   window is tiled.
};

// ─────────────────────────────────────────────────────────────────────────────
// TilingContext
//
// Everything one layout pass needs, bundled into one argument so the
// per-layout methods have a clean signature.
// ─────────────────────────────────────────────────────────────────────────────
struct TilingContext {
    QList<Window*> tiled;    ///< Windows to be placed by the engine (already
    ///<   filtered: tiled, visible, non-fullscreen).
    QList<Window*> floating; ///< Floating windows — returned unchanged.
    QList<Window*> fullscreen;///< Fullscreen windows — span the full output.
    QRect          area;     ///< Work area in compositor coordinates.
    GapPolicy      gaps;
    float          masterRatio = 0.55f;
    int            maxColumns  = 3;    ///< Used by Grid / ThreeColumn.
    bool           monocleActive = false; ///< For Monocle: paint active on top.
};

// ─────────────────────────────────────────────────────────────────────────────
// BSPNode  (used internally by the BSP layout)
//
// Each node represents either a split point or a leaf (one window slot).
// The tree is rebuilt every time the window list changes.
// ─────────────────────────────────────────────────────────────────────────────
struct BSPNode {
    enum class Split { Horizontal, Vertical };

    bool   isLeaf   = true;
    Split  split    = Split::Horizontal;
    float  ratio    = 0.5f;            ///< Where in [0.1, 0.9] the split falls.
    QRect  rect;                       ///< Assigned by the layout pass.
    int    windowIndex = -1;           ///< Index into TilingContext::tiled.

    BSPNode* left  = nullptr;          ///< Child towards the origin.
    BSPNode* right = nullptr;          ///< Child away from the origin.

    ~BSPNode() { delete left; delete right; }
};

// ─────────────────────────────────────────────────────────────────────────────
// TilingEngine
//
// Pure-functional tiling layout engine.  Given a list of windows and a work
// area rectangle it returns the desired geometry for each window.  It holds no
// mutable per-window state between calls: all per-window parameters it needs
// (min/max size, tile slot) are read from the Window objects themselves.
//
// Usage
// ─────
//   TilingEngine engine;
//   engine.setMasterRatio(0.55f);
//   engine.setGaps(10, 12);
//
//   auto results = engine.tile(windows, workArea, TilingLayout::Spiral);
//   for (const auto& r : results)
//       animateWindow(r.window, r.targetGeometry);
//
// Thread safety
// ─────────────
//   tile() and all layout methods are const and do not modify shared state.
//   They may safely be called from a worker thread as long as the Window
//   objects are not mutated concurrently.
// ─────────────────────────────────────────────────────────────────────────────
class TilingEngine : public QObject {
    Q_OBJECT

public:
    // ── Construction ──────────────────────────────────────────────────────
    explicit TilingEngine(QObject* parent = nullptr);
    ~TilingEngine() override = default;

    // ── Primary entry point ───────────────────────────────────────────────

    /// Compute tile positions for every window in \p windows.
    ///
    /// Internally the engine:
    ///   1. Separates windows into tiled / floating / fullscreen buckets.
    ///   2. Applies smart-gap suppression if only one tiled window is present.
    ///   3. Delegates to the appropriate layout method.
    ///   4. Appends floating windows (geometry unchanged) and fullscreen
    ///      windows (geometry = \p area) with appropriate z-orders.
    ///
    /// Returns results sorted so that lower z-orders come first.
    QList<TileResult> tile(const QList<Window*>& windows,
                           const QRect&          area,
                           TilingLayout          layout) const;

                           // ── Named layout methods ──────────────────────────────────────────────
                           // Each method receives only the tiled-window subset and the gap-shrunk
                           // work area; it must NOT handle floating / fullscreen windows.

                           /// Fibonacci spiral: each window takes a half of the remaining rect,
                           /// cycling through directions Right → Down → Left → Up.
                           QList<TileResult> layoutSpiral(const TilingContext& ctx) const;

                           /// Master-left + right stack.
                           QList<TileResult> layoutTall(const TilingContext& ctx) const;

                           /// Master-top + bottom row.
                           QList<TileResult> layoutWide(const TilingContext& ctx) const;

                           /// Near-square grid; last row centred if window count is not a perfect square.
                           QList<TileResult> layoutGrid(const TilingContext& ctx) const;

                           /// Alternating-axis halving cascade into a corner.
                           QList<TileResult> layoutDwindle(const TilingContext& ctx) const;

                           /// All windows stacked on top of each other (monocle / tabbed).
                           QList<TileResult> layoutMonocle(const TilingContext& ctx) const;

                           /// Single window centred at ~72 % of the work area; multiple → Spiral.
                           QList<TileResult> layoutCentered(const TilingContext& ctx) const;

                           /// Three equal columns with master in the middle.
                           QList<TileResult> layoutThreeColumn(const TilingContext& ctx) const;

                           /// Binary-space partition using per-window tile-slot split direction.
                           QList<TileResult> layoutBSP(const TilingContext& ctx) const;

                           // ── Configuration ─────────────────────────────────────────────────────

                           float masterRatio() const           { return m_masterRatio; }
                           void  setMasterRatio(float r)       { m_masterRatio = qBound(0.1f, r, 0.9f); }
                           void  adjustMasterRatio(float delta){ setMasterRatio(m_masterRatio + delta); }

                           int  gapInner()  const { return m_gaps.inner; }
                           int  gapOuter()  const { return m_gaps.outer; }
                           void setGaps(int inner, int outer) {
                               m_gaps.inner = qMax(0, inner);
                               m_gaps.outer = qMax(0, outer);
                           }

                           bool smartGaps()    const { return m_gaps.smartGaps;    }
                           bool smartBorders() const { return m_gaps.smartBorders; }
                           void setSmartGaps(bool v)    { m_gaps.smartGaps    = v; }
                           void setSmartBorders(bool v) { m_gaps.smartBorders = v; }

                           int  maxColumns()   const { return m_maxColumns; }
                           void setMaxColumns(int c) { m_maxColumns = qMax(1, c); }

                           // ── String ↔ enum helpers ─────────────────────────────────────────────

                           static TilingLayout layoutFromString(const QString& s);
                           static QString      layoutToString(TilingLayout l);

                           /// Human-readable display name (e.g. shown in the bar).
                           static QString      layoutDisplayName(TilingLayout l);

                           /// Icon glyph for the bar indicator.
                           static QString      layoutIcon(TilingLayout l);

private:
    // ── Context builder ───────────────────────────────────────────────────

    /// Partition \p windows into tiled / floating / fullscreen, apply
    /// smart-gap suppression, and populate a TilingContext ready for a
    /// layout method.
    TilingContext buildContext(const QList<Window*>& windows,
                               const QRect&          area) const;

                               // ── Gap helpers ───────────────────────────────────────────────────────

                               /// Shrink \p rect inward by half the inner gap on each side that is
                               /// adjacent to another tile (indicated by the four bool flags).
                               QRect applyInnerGap(const QRect& rect,
                                                   bool adjTop, bool adjRight,
                                                   bool adjBottom, bool adjLeft) const;

                                                   /// Shrink \p area inward by the outer gap on all four sides.
                                                   QRect applyOuterGap(const QRect& area) const;

                                                   /// Apply a uniform half-inner-gap to every side of \p rect.
                                                   /// Convenience wrapper used by layouts that don't track adjacency.
                                                   QRect applyHalfGap(const QRect& rect) const;

                                                   // ── Size-constraint helper ─────────────────────────────────────────────

                                                   /// Clamp \p rect so the window's minSize / maxSize constraints are
                                                   /// respected, keeping the rect centred around its original centre point.
                                                   QRect applyConstraints(const QRect& rect, const Window* w) const;

                                                   // ── BSP helpers ───────────────────────────────────────────────────────

                                                   /// Recursively build a balanced BSP tree for \p count leaf windows
                                                   /// occupying \p rect.  Caller owns the returned root node.
                                                   BSPNode* buildBSPTree(int count, const QRect& rect,
                                                                         BSPNode::Split preferredSplit) const;

                                                                         /// Walk the BSP tree and collect one TileResult per leaf, assigning
                                                                         /// windows from \p ctx.tiled in slot order.
                                                                         void collectBSPResults(const BSPNode*       node,
                                                                                                const TilingContext& ctx,
                                                                                                QList<TileResult>&   out,
                                                                                                int&                 index) const;

                                                                                                // ── Members ───────────────────────────────────────────────────────────
                                                                                                float      m_masterRatio = 0.55f;
                                                                                                GapPolicy  m_gaps;
                                                                                                int        m_maxColumns  = 3;
};
