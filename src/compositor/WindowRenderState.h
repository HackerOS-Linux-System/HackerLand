#pragma once

#include <QImage>
#include <QRect>

// ─────────────────────────────────────────────────────────────────────────────
// WindowRenderState
//
// Per-window data cached across frames. Defined here in a dedicated header
// so that both WMOutput and RenderEngine can include it without redefinition.
// ─────────────────────────────────────────────────────────────────────────────
struct WindowRenderState {
    // Blur cache: blurred slice of the wallpaper behind this window
    QImage  blurredBackground;
    QRect   blurSourceRect;      ///< Which wallpaper crop was blurred
    bool    blurDirty = true;    ///< Needs re-bake on next frame

    // Geometry snapshot — blur is invalidated when this changes
    QRect   lastGeometry;

    // Per-window glow animation phase in [0, 2π) for active border pulse
    float   glowPhase = 0.0f;
};
