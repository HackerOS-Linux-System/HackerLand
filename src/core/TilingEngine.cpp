#include "TilingEngine.h"
#include "Window.h"
#include <QtMath>
#include <QDebug>

TilingEngine::TilingEngine(QObject* parent) : QObject(parent) {}

// ─────────────────────────────────────────────────────────────────────────────
// Gap helpers
// ─────────────────────────────────────────────────────────────────────────────

QRect TilingEngine::applyInnerGap(const QRect& rect,
                                  bool adjTop, bool adjRight,
                                  bool adjBottom, bool adjLeft) const {
                                      const int h = m_gaps.inner / 2;
                                      return rect.adjusted(
                                          adjLeft   ? h : 0,
                                          adjTop    ? h : 0,
                                          adjRight  ? -h : 0,
                                          adjBottom ? -h : 0
                                      );
                                  }

                                  QRect TilingEngine::applyOuterGap(const QRect& area) const {
                                      const int o = m_gaps.outer;
                                      return area.adjusted(o, o, -o, -o);
                                  }

                                  QRect TilingEngine::applyHalfGap(const QRect& rect) const {
                                      const int h = m_gaps.inner / 2;
                                      return rect.adjusted(h, h, -h, -h);
                                  }

                                  QRect TilingEngine::applyConstraints(const QRect& rect, const Window* w) const {
                                      if (!w) return rect;
                                      QSize sz = rect.size();
                                      if (w->minSize().isValid()) {
                                          sz.setWidth (qMax(sz.width(),  w->minSize().width()));
                                          sz.setHeight(qMax(sz.height(), w->minSize().height()));
                                      }
                                      if (w->maxSize().isValid() && !w->maxSize().isEmpty()) {
                                          if (w->maxSize().width()  > 0) sz.setWidth (qMin(sz.width(),  w->maxSize().width()));
                                          if (w->maxSize().height() > 0) sz.setHeight(qMin(sz.height(), w->maxSize().height()));
                                      }
                                      QPoint center = rect.center();
                                      return QRect(center.x() - sz.width() / 2,
                                                   center.y() - sz.height() / 2,
                                                   sz.width(), sz.height());
                                  }

                                  // ─────────────────────────────────────────────────────────────────────────────
                                  // Context builder
                                  // ─────────────────────────────────────────────────────────────────────────────

                                  TilingContext TilingEngine::buildContext(const QList<Window*>& windows,
                                                                           const QRect&          area) const {
                                                                               TilingContext ctx;
                                                                               ctx.area        = area;
                                                                               ctx.gaps        = m_gaps;
                                                                               ctx.masterRatio = m_masterRatio;
                                                                               ctx.maxColumns  = m_maxColumns;

                                                                               for (auto* w : windows) {
                                                                                   if (!w->isVisible()) continue;
                                                                                   if (w->isFullscreen())     ctx.fullscreen.append(w);
                                                                                   else if (w->isFloating())  ctx.floating.append(w);
                                                                                   else                       ctx.tiled.append(w);
                                                                               }
                                                                               return ctx;
                                                                           }

                                                                           // ─────────────────────────────────────────────────────────────────────────────
                                                                           // Primary entry point
                                                                           // ─────────────────────────────────────────────────────────────────────────────

                                                                           QList<TileResult> TilingEngine::tile(const QList<Window*>& windows,
                                                                                                                const QRect&          area,
                                                                                                                TilingLayout          layout) const {
                                                                                                                    TilingContext ctx = buildContext(windows, area);
                                                                                                                    QList<TileResult> results;
                                                                                                                    int z = 0;

                                                                                                                    // Fullscreen windows span the full output
                                                                                                                    for (auto* w : ctx.fullscreen)
                                                                                                                        results.append({w, area, 1000 + z++});

                                                                                                                    // Floating windows keep their geometry
                                                                                                                    z = 100;
                                                                                                                    for (auto* w : ctx.floating)
                                                                                                                        results.append({w, w->geometry(), z++});

                                                                                                                    if (ctx.tiled.isEmpty()) return results;

                                                                                                                    // Smart gaps: single tiled window fills work area without gaps
                                                                                                                    const bool isSingle = (ctx.tiled.size() == 1) && m_gaps.smartGaps;

                                                                                                                    QList<TileResult> tiledResults;
                                                                                                                    if (isSingle) {
                                                                                                                        tiledResults = layoutMonocle(ctx);
                                                                                                                    } else {
                                                                                                                        switch (layout) {
                                                                                                                            case TilingLayout::Spiral:      tiledResults = layoutSpiral(ctx);      break;
                                                                                                                            case TilingLayout::Tall:        tiledResults = layoutTall(ctx);        break;
                                                                                                                            case TilingLayout::Wide:        tiledResults = layoutWide(ctx);        break;
                                                                                                                            case TilingLayout::Grid:        tiledResults = layoutGrid(ctx);        break;
                                                                                                                            case TilingLayout::Dwindle:     tiledResults = layoutDwindle(ctx);     break;
                                                                                                                            case TilingLayout::Monocle:     tiledResults = layoutMonocle(ctx);     break;
                                                                                                                            case TilingLayout::Centered:    tiledResults = layoutCentered(ctx);    break;
                                                                                                                            case TilingLayout::ThreeColumn: tiledResults = layoutThreeColumn(ctx); break;
                                                                                                                            case TilingLayout::BSP:         tiledResults = layoutBSP(ctx);         break;
                                                                                                                            default:                        tiledResults = layoutSpiral(ctx);      break;
                                                                                                                        }
                                                                                                                    }

                                                                                                                    results.append(tiledResults);
                                                                                                                    return results;
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: Spiral (Fibonacci)
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                QList<TileResult> TilingEngine::layoutSpiral(const TilingContext& ctx) const {
                                                                                                                    QList<TileResult> results;
                                                                                                                    const auto& windows = ctx.tiled;
                                                                                                                    if (windows.isEmpty()) return results;

                                                                                                                    QRect remaining = applyOuterGap(ctx.area);

                                                                                                                    for (int i = 0; i < windows.size(); ++i) {
                                                                                                                        const bool last = (i == windows.size() - 1);
                                                                                                                        QRect winRect;

                                                                                                                        if (last) {
                                                                                                                            winRect = remaining;
                                                                                                                        } else {
                                                                                                                            const int   dir   = i % 4;
                                                                                                                            const float ratio = (i == 0) ? ctx.masterRatio : 0.5f;

                                                                                                                            if (dir == 0) {
                                                                                                                                const int w = (int)(remaining.width() * ratio);
                                                                                                                                winRect   = QRect(remaining.x(), remaining.y(), w, remaining.height());
                                                                                                                                remaining = remaining.adjusted(w, 0, 0, 0);
                                                                                                                            } else if (dir == 1) {
                                                                                                                                const int h = (int)(remaining.height() * ratio);
                                                                                                                                winRect   = QRect(remaining.x(), remaining.y(), remaining.width(), h);
                                                                                                                                remaining = remaining.adjusted(0, h, 0, 0);
                                                                                                                            } else if (dir == 2) {
                                                                                                                                const int w = (int)(remaining.width() * (1.0f - ratio));
                                                                                                                                winRect   = QRect(remaining.right() - w, remaining.y(), w, remaining.height());
                                                                                                                                remaining = remaining.adjusted(0, 0, -w, 0);
                                                                                                                            } else {
                                                                                                                                const int h = (int)(remaining.height() * (1.0f - ratio));
                                                                                                                                winRect   = QRect(remaining.x(), remaining.bottom() - h, remaining.width(), h);
                                                                                                                                remaining = remaining.adjusted(0, 0, 0, -h);
                                                                                                                            }
                                                                                                                        }

                                                                                                                        results.append({windows[i], applyHalfGap(winRect), i});
                                                                                                                    }
                                                                                                                    return results;
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: Tall (master-left)
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                QList<TileResult> TilingEngine::layoutTall(const TilingContext& ctx) const {
                                                                                                                    QList<TileResult> results;
                                                                                                                    const auto& windows = ctx.tiled;
                                                                                                                    if (windows.isEmpty()) return results;

                                                                                                                    const QRect a = applyOuterGap(ctx.area);

                                                                                                                    if (windows.size() == 1) {
                                                                                                                        results.append({windows[0], applyHalfGap(a), 0});
                                                                                                                        return results;
                                                                                                                    }

                                                                                                                    const int masterW = (int)(a.width() * ctx.masterRatio);
                                                                                                                    results.append({windows[0], applyHalfGap(QRect(a.x(), a.y(), masterW, a.height())), 0});

                                                                                                                    const int stackX = a.x() + masterW;
                                                                                                                    const int stackW = a.width() - masterW;
                                                                                                                    const int n      = windows.size() - 1;

                                                                                                                    for (int i = 0; i < n; ++i) {
                                                                                                                        const int y = a.y() + (int)((float)i       / n * a.height());
                                                                                                                        const int h = a.y() + (int)((float)(i + 1) / n * a.height()) - y;
                                                                                                                        results.append({windows[i + 1],
                                                                                                                            applyHalfGap(QRect(stackX, y, stackW, h)),
                                                                                                                                       i + 1});
                                                                                                                    }
                                                                                                                    return results;
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: Wide (master-top)
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                QList<TileResult> TilingEngine::layoutWide(const TilingContext& ctx) const {
                                                                                                                    QList<TileResult> results;
                                                                                                                    const auto& windows = ctx.tiled;
                                                                                                                    if (windows.isEmpty()) return results;

                                                                                                                    const QRect a = applyOuterGap(ctx.area);

                                                                                                                    if (windows.size() == 1) {
                                                                                                                        results.append({windows[0], applyHalfGap(a), 0});
                                                                                                                        return results;
                                                                                                                    }

                                                                                                                    const int masterH = (int)(a.height() * ctx.masterRatio);
                                                                                                                    results.append({windows[0], applyHalfGap(QRect(a.x(), a.y(), a.width(), masterH)), 0});

                                                                                                                    const int stackY = a.y() + masterH;
                                                                                                                    const int stackH = a.height() - masterH;
                                                                                                                    const int n      = windows.size() - 1;

                                                                                                                    for (int i = 0; i < n; ++i) {
                                                                                                                        const int x = a.x() + (int)((float)i       / n * a.width());
                                                                                                                        const int w = a.x() + (int)((float)(i + 1) / n * a.width()) - x;
                                                                                                                        results.append({windows[i + 1],
                                                                                                                            applyHalfGap(QRect(x, stackY, w, stackH)),
                                                                                                                                       i + 1});
                                                                                                                    }
                                                                                                                    return results;
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: Grid
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                QList<TileResult> TilingEngine::layoutGrid(const TilingContext& ctx) const {
                                                                                                                    QList<TileResult> results;
                                                                                                                    const auto& windows = ctx.tiled;
                                                                                                                    if (windows.isEmpty()) return results;

                                                                                                                    const QRect a    = applyOuterGap(ctx.area);
                                                                                                                    const int   n    = windows.size();
                                                                                                                    const int   cols = qMax(1, (int)qCeil(qSqrt((double)n)));
                                                                                                                    const int   rows = (n + cols - 1) / cols;

                                                                                                                    for (int i = 0; i < n; ++i) {
                                                                                                                        const int col       = i % cols;
                                                                                                                        const int row       = i / cols;
                                                                                                                        const int colsInRow = (row == rows - 1) ? (n - row * cols) : cols;
                                                                                                                        const int cellW     = a.width()  / cols;
                                                                                                                        const int cellH     = a.height() / rows;
                                                                                                                        const int xOffset   = (cols - colsInRow) * cellW / 2;

                                                                                                                        results.append({windows[i],
                                                                                                                            applyHalfGap(QRect(a.x() + col * cellW + xOffset,
                                                                                                                                               a.y() + row * cellH,
                                                                                                                                               cellW, cellH)),
                                                                                                                                               i});
                                                                                                                    }
                                                                                                                    return results;
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: Dwindle
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                QList<TileResult> TilingEngine::layoutDwindle(const TilingContext& ctx) const {
                                                                                                                    QList<TileResult> results;
                                                                                                                    const auto& windows = ctx.tiled;
                                                                                                                    if (windows.isEmpty()) return results;

                                                                                                                    QRect remaining = applyOuterGap(ctx.area);

                                                                                                                    for (int i = 0; i < windows.size(); ++i) {
                                                                                                                        const bool last = (i == windows.size() - 1);
                                                                                                                        QRect winRect;

                                                                                                                        if (last) {
                                                                                                                            winRect = remaining;
                                                                                                                        } else if (i % 2 == 0) {
                                                                                                                            const int w = remaining.width() / 2;
                                                                                                                            winRect   = QRect(remaining.x(), remaining.y(), w, remaining.height());
                                                                                                                            remaining = remaining.adjusted(w, 0, 0, 0);
                                                                                                                        } else {
                                                                                                                            const int h = remaining.height() / 2;
                                                                                                                            winRect   = QRect(remaining.x(), remaining.y(), remaining.width(), h);
                                                                                                                            remaining = remaining.adjusted(0, h, 0, 0);
                                                                                                                        }

                                                                                                                        results.append({windows[i], applyHalfGap(winRect), i});
                                                                                                                    }
                                                                                                                    return results;
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: Monocle
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                QList<TileResult> TilingEngine::layoutMonocle(const TilingContext& ctx) const {
                                                                                                                    QList<TileResult> results;
                                                                                                                    const QRect a = applyOuterGap(ctx.area);
                                                                                                                    const QRect r = applyHalfGap(a);
                                                                                                                    for (int i = 0; i < ctx.tiled.size(); ++i)
                                                                                                                        results.append({ctx.tiled[i], r, i});
                                                                                                                    return results;
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: Centered
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                QList<TileResult> TilingEngine::layoutCentered(const TilingContext& ctx) const {
                                                                                                                    if (ctx.tiled.isEmpty()) return {};

                                                                                                                    if (ctx.tiled.size() == 1) {
                                                                                                                        const QRect& area = ctx.area;
                                                                                                                        const int w = (int)(area.width()  * 0.72f);
                                                                                                                        const int h = (int)(area.height() * 0.78f);
                                                                                                                        const int x = area.x() + (area.width()  - w) / 2;
                                                                                                                        const int y = area.y() + (area.height() - h) / 2;
                                                                                                                        return {{ctx.tiled[0], QRect(x, y, w, h), 0}};
                                                                                                                    }

                                                                                                                    return layoutSpiral(ctx);
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: ThreeColumn
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                QList<TileResult> TilingEngine::layoutThreeColumn(const TilingContext& ctx) const {
                                                                                                                    QList<TileResult> results;
                                                                                                                    const auto& windows = ctx.tiled;
                                                                                                                    if (windows.isEmpty()) return results;

                                                                                                                    const QRect a = applyOuterGap(ctx.area);

                                                                                                                    if (windows.size() <= 2) return layoutTall(ctx);

                                                                                                                    const int colW = a.width() / 3;

                                                                                                                    // Left column — slaves[0..n/3]
                                                                                                                    // Centre column — master
                                                                                                                    // Right column — remaining slaves
                                                                                                                    const int masterIdx = 0;
                                                                                                                    QList<Window*> left, right;
                                                                                                                    for (int i = 1; i < windows.size(); ++i) {
                                                                                                                        if (i % 2 == 1) left.append(windows[i]);
                                                                                                                        else             right.append(windows[i]);
                                                                                                                    }

                                                                                                                    auto fillColumn = [&](const QList<Window*>& col, int xOff, int z0) {
                                                                                                                        const int n = col.size();
                                                                                                                        for (int i = 0; i < n; ++i) {
                                                                                                                            const int y = a.y() + (int)((float)i       / n * a.height());
                                                                                                                            const int h = a.y() + (int)((float)(i + 1) / n * a.height()) - y;
                                                                                                                            results.append({col[i], applyHalfGap(QRect(a.x() + xOff, y, colW, h)), z0 + i});
                                                                                                                        }
                                                                                                                    };

                                                                                                                    fillColumn(left,  0,       10);
                                                                                                                    results.append({windows[masterIdx], applyHalfGap(QRect(a.x() + colW, a.y(), colW, a.height())), 0});
                                                                                                                    fillColumn(right, colW * 2, 20);

                                                                                                                    return results;
                                                                                                                }

                                                                                                                // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                // Layout: BSP
                                                                                                                // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                BSPNode* TilingEngine::buildBSPTree(int count, const QRect& rect,
                                                                                                                                                    BSPNode::Split preferredSplit) const {
                                                                                                                                                        auto* node = new BSPNode();
                                                                                                                                                        node->rect = rect;

                                                                                                                                                        if (count <= 1) {
                                                                                                                                                            node->isLeaf      = true;
                                                                                                                                                            node->windowIndex = 0;
                                                                                                                                                            return node;
                                                                                                                                                        }

                                                                                                                                                        node->isLeaf = false;
                                                                                                                                                        node->split  = preferredSplit;
                                                                                                                                                        node->ratio  = 0.5f;

                                                                                                                                                        const BSPNode::Split nextSplit =
                                                                                                                                                        (preferredSplit == BSPNode::Split::Horizontal)
                                                                                                                                                        ? BSPNode::Split::Vertical
                                                                                                                                                        : BSPNode::Split::Horizontal;

                                                                                                                                                        const int leftCount  = count / 2;
                                                                                                                                                        const int rightCount = count - leftCount;

                                                                                                                                                        QRect leftRect, rightRect;
                                                                                                                                                        if (preferredSplit == BSPNode::Split::Horizontal) {
                                                                                                                                                            const int w = (int)(rect.width() * node->ratio);
                                                                                                                                                            leftRect  = QRect(rect.x(), rect.y(), w, rect.height());
                                                                                                                                                            rightRect = QRect(rect.x() + w, rect.y(), rect.width() - w, rect.height());
                                                                                                                                                        } else {
                                                                                                                                                            const int h = (int)(rect.height() * node->ratio);
                                                                                                                                                            leftRect  = QRect(rect.x(), rect.y(), rect.width(), h);
                                                                                                                                                            rightRect = QRect(rect.x(), rect.y() + h, rect.width(), rect.height() - h);
                                                                                                                                                        }

                                                                                                                                                        node->left  = buildBSPTree(leftCount,  leftRect,  nextSplit);
                                                                                                                                                        node->right = buildBSPTree(rightCount, rightRect, nextSplit);
                                                                                                                                                        return node;
                                                                                                                                                    }

                                                                                                                                                    void TilingEngine::collectBSPResults(const BSPNode*       node,
                                                                                                                                                                                         const TilingContext& ctx,
                                                                                                                                                                                         QList<TileResult>&   out,
                                                                                                                                                                                         int&                 index) const {
                                                                                                                                                                                             if (!node) return;
                                                                                                                                                                                             if (node->isLeaf) {
                                                                                                                                                                                                 if (index < ctx.tiled.size())
                                                                                                                                                                                                     out.append({ctx.tiled[index++], applyHalfGap(node->rect), index});
                                                                                                                                                                                                 return;
                                                                                                                                                                                             }
                                                                                                                                                                                             collectBSPResults(node->left,  ctx, out, index);
                                                                                                                                                                                             collectBSPResults(node->right, ctx, out, index);
                                                                                                                                                                                         }

                                                                                                                                                                                         QList<TileResult> TilingEngine::layoutBSP(const TilingContext& ctx) const {
                                                                                                                                                                                             QList<TileResult> results;
                                                                                                                                                                                             if (ctx.tiled.isEmpty()) return results;

                                                                                                                                                                                             const QRect area = applyOuterGap(ctx.area);
                                                                                                                                                                                             BSPNode* root = buildBSPTree(ctx.tiled.size(), area, BSPNode::Split::Horizontal);
                                                                                                                                                                                             int index = 0;
                                                                                                                                                                                             collectBSPResults(root, ctx, results, index);
                                                                                                                                                                                             delete root;
                                                                                                                                                                                             return results;
                                                                                                                                                                                         }

                                                                                                                                                                                         // ─────────────────────────────────────────────────────────────────────────────
                                                                                                                                                                                         // String helpers
                                                                                                                                                                                         // ─────────────────────────────────────────────────────────────────────────────

                                                                                                                                                                                         TilingLayout TilingEngine::layoutFromString(const QString& s) {
                                                                                                                                                                                             if (s == "tall")        return TilingLayout::Tall;
                                                                                                                                                                                             if (s == "wide")        return TilingLayout::Wide;
                                                                                                                                                                                             if (s == "grid")        return TilingLayout::Grid;
                                                                                                                                                                                             if (s == "dwindle")     return TilingLayout::Dwindle;
                                                                                                                                                                                             if (s == "monocle")     return TilingLayout::Monocle;
                                                                                                                                                                                             if (s == "centered")    return TilingLayout::Centered;
                                                                                                                                                                                             if (s == "threecolumn") return TilingLayout::ThreeColumn;
                                                                                                                                                                                             if (s == "bsp")         return TilingLayout::BSP;
                                                                                                                                                                                             return TilingLayout::Spiral;
                                                                                                                                                                                         }

                                                                                                                                                                                         QString TilingEngine::layoutToString(TilingLayout l) {
                                                                                                                                                                                             switch (l) {
                                                                                                                                                                                                 case TilingLayout::Tall:        return "tall";
                                                                                                                                                                                                 case TilingLayout::Wide:        return "wide";
                                                                                                                                                                                                 case TilingLayout::Grid:        return "grid";
                                                                                                                                                                                                 case TilingLayout::Dwindle:     return "dwindle";
                                                                                                                                                                                                 case TilingLayout::Monocle:     return "monocle";
                                                                                                                                                                                                 case TilingLayout::Centered:    return "centered";
                                                                                                                                                                                                 case TilingLayout::ThreeColumn: return "threecolumn";
                                                                                                                                                                                                 case TilingLayout::BSP:         return "bsp";
                                                                                                                                                                                                 default:                        return "spiral";
                                                                                                                                                                                             }
                                                                                                                                                                                         }

                                                                                                                                                                                         QString TilingEngine::layoutDisplayName(TilingLayout l) {
                                                                                                                                                                                             switch (l) {
                                                                                                                                                                                                 case TilingLayout::Spiral:      return "Spiral";
                                                                                                                                                                                                 case TilingLayout::Tall:        return "Tall";
                                                                                                                                                                                                 case TilingLayout::Wide:        return "Wide";
                                                                                                                                                                                                 case TilingLayout::Grid:        return "Grid";
                                                                                                                                                                                                 case TilingLayout::Dwindle:     return "Dwindle";
                                                                                                                                                                                                 case TilingLayout::Monocle:     return "Monocle";
                                                                                                                                                                                                 case TilingLayout::Centered:    return "Centered";
                                                                                                                                                                                                 case TilingLayout::ThreeColumn: return "3-Column";
                                                                                                                                                                                                 case TilingLayout::BSP:         return "BSP";
                                                                                                                                                                                                 default:                        return "Spiral";
                                                                                                                                                                                             }
                                                                                                                                                                                         }

                                                                                                                                                                                         QString TilingEngine::layoutIcon(TilingLayout l) {
                                                                                                                                                                                             switch (l) {
                                                                                                                                                                                                 case TilingLayout::Spiral:      return "󰿊";
                                                                                                                                                                                                 case TilingLayout::Tall:        return "";
                                                                                                                                                                                                 case TilingLayout::Wide:        return "";
                                                                                                                                                                                                 case TilingLayout::Grid:        return "󱇜";
                                                                                                                                                                                                 case TilingLayout::Dwindle:     return "󰕴";
                                                                                                                                                                                                 case TilingLayout::Monocle:     return "󱗖";
                                                                                                                                                                                                 case TilingLayout::Centered:    return "󰿏";
                                                                                                                                                                                                 case TilingLayout::ThreeColumn: return "󰕴";
                                                                                                                                                                                                 case TilingLayout::BSP:         return "󰾍";
                                                                                                                                                                                                 default:                        return "";
                                                                                                                                                                                             }
                                                                                                                                                                                         }
