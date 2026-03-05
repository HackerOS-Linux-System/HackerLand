#include "AnimationEngine.h"
#include "Window.h"
#include "Config.h"
#include <QVariantAnimation>
#include <QtMath>
#include <QDebug>

AnimationEngine::AnimationEngine(QObject* parent) : QObject(parent) {}

AnimationEngine::~AnimationEngine() {
    qDeleteAll(m_animations);
}

float AnimationEngine::easeOutBack(float t) const {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * qPow(t - 1.0f, 3) + c1 * qPow(t - 1.0f, 2);
}

float AnimationEngine::easeInCubic(float t) const {
    return t * t * t;
}

float AnimationEngine::easeOutCubic(float t) const {
    float t2 = t - 1.0f;
    return 1.0f + t2 * t2 * t2;
}

float AnimationEngine::spring(float t) const {
    // Spring-like animation (similar to niri's smooth animations)
    float zeta = 0.55f; // damping
    float omega = 2.0f * M_PI * 1.5f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - qExp(-zeta * omega * t) * qCos(omega * qSqrt(1.0f - zeta*zeta) * t);
}

void AnimationEngine::cancelAnimation(Window* w) {
    auto* existing = m_animations.take(w);
    if (existing) {
        if (existing->anim) existing->anim->stop();
        delete existing;
    }
}

bool AnimationEngine::isAnimating(Window* w) const {
    return m_animations.contains(w);
}

void AnimationEngine::animateWindowOpen(Window* w, const QRect& targetGeom) {
    if (!m_enabled || !Config::instance().animationsEnabled()) {
        w->setGeometry(targetGeom);
        w->setOpacity(1.0f);
        w->setAnimProgress(1.0f);
        return;
    }

    cancelAnimation(w);

    auto* wa = new WindowAnimation();
    wa->fromGeometry = QRect(
        targetGeom.x() + targetGeom.width() / 2 - 10,
                             targetGeom.y() + targetGeom.height() / 2 - 10,
                             20, 20
    );
    wa->toGeometry   = targetGeom;
    wa->fromOpacity  = 0.0f;
    wa->toOpacity    = 1.0f;
    wa->fromScale    = Config::instance().anim.scaleFactor;
    wa->toScale      = 1.0f;
    wa->duration     = Config::instance().anim.windowOpenMs;

    wa->anim = std::make_unique<QVariantAnimation>();
    wa->anim->setStartValue(0.0f);
    wa->anim->setEndValue(1.0f);
    wa->anim->setDuration(wa->duration);

    connect(wa->anim.get(), &QVariantAnimation::valueChanged, [=](const QVariant& v) {
        float raw = v.toFloat();
        float t = spring(raw);

        QRect cur;
        cur.setX(     (int)(wa->fromGeometry.x() * (1-t) + wa->toGeometry.x() * t));
        cur.setY(     (int)(wa->fromGeometry.y() * (1-t) + wa->toGeometry.y() * t));
        cur.setWidth( (int)(wa->fromGeometry.width() * (1-t) + wa->toGeometry.width() * t));
        cur.setHeight((int)(wa->fromGeometry.height() * (1-t) + wa->toGeometry.height() * t));

        float op = wa->fromOpacity + (wa->toOpacity - wa->fromOpacity) * qMin(t * 2.0f, 1.0f);

        w->setGeometry(cur);
        w->setOpacity(op);
        w->setAnimProgress(t);
    });

    connect(wa->anim.get(), &QVariantAnimation::finished, [=]() {
        w->setGeometry(targetGeom);
        w->setOpacity(1.0f);
        w->setAnimProgress(1.0f);
        cancelAnimation(w);
        emit animationFinished(w);
    });

    m_animations.insert(w, wa);
    wa->anim->start();
}

void AnimationEngine::animateWindowClose(Window* w, std::function<void()> onDone) {
    if (!m_enabled || !Config::instance().animationsEnabled()) {
        if (onDone) onDone();
        return;
    }

    cancelAnimation(w);

    auto* wa = new WindowAnimation();
    wa->fromGeometry = w->geometry();
    wa->toGeometry   = QRect(
        w->geometry().x() + w->geometry().width() / 2 - 20,
                             w->geometry().y() + w->geometry().height() / 2 - 20,
                             40, 40
    );
    wa->fromOpacity  = w->opacity();
    wa->toOpacity    = 0.0f;
    wa->duration     = Config::instance().anim.windowCloseMs;

    wa->anim = std::make_unique<QVariantAnimation>();
    wa->anim->setStartValue(0.0f);
    wa->anim->setEndValue(1.0f);
    wa->anim->setDuration(wa->duration);

    connect(wa->anim.get(), &QVariantAnimation::valueChanged, [=](const QVariant& v) {
        float t = easeInCubic(v.toFloat());
        QRect cur;
        cur.setX(     (int)(wa->fromGeometry.x() * (1-t) + wa->toGeometry.x() * t));
        cur.setY(     (int)(wa->fromGeometry.y() * (1-t) + wa->toGeometry.y() * t));
        cur.setWidth( (int)(wa->fromGeometry.width() * (1-t) + wa->toGeometry.width() * t));
        cur.setHeight((int)(wa->fromGeometry.height() * (1-t) + wa->toGeometry.height() * t));
        float op = wa->fromOpacity + (wa->toOpacity - wa->fromOpacity) * t;
        w->setGeometry(cur);
        w->setOpacity(op);
        w->setAnimProgress(1.0f - t);
    });

    connect(wa->anim.get(), &QVariantAnimation::finished, [=]() {
        cancelAnimation(w);
        if (onDone) onDone();
        emit animationFinished(w);
    });

    m_animations.insert(w, wa);
    wa->anim->start();
}

void AnimationEngine::animateWindowMove(Window* w, const QRect& from, const QRect& to) {
    if (!m_enabled || !Config::instance().animationsEnabled()) {
        w->setGeometry(to);
        return;
    }

    cancelAnimation(w);

    auto* wa = new WindowAnimation();
    wa->fromGeometry = from;
    wa->toGeometry   = to;
    wa->duration     = Config::instance().anim.tileRearrangeMs;

    wa->anim = std::make_unique<QVariantAnimation>();
    wa->anim->setStartValue(0.0f);
    wa->anim->setEndValue(1.0f);
    wa->anim->setDuration(wa->duration);

    connect(wa->anim.get(), &QVariantAnimation::valueChanged, [=](const QVariant& v) {
        float t = easeOutCubic(v.toFloat());
        QRect cur;
        cur.setX(     (int)(from.x() * (1-t) + to.x() * t));
        cur.setY(     (int)(from.y() * (1-t) + to.y() * t));
        cur.setWidth( (int)(from.width() * (1-t) + to.width() * t));
        cur.setHeight((int)(from.height() * (1-t) + to.height() * t));
        w->setGeometry(cur);
    });

    connect(wa->anim.get(), &QVariantAnimation::finished, [=]() {
        w->setGeometry(to);
        cancelAnimation(w);
        emit animationFinished(w);
    });

    m_animations.insert(w, wa);
    wa->anim->start();
}

void AnimationEngine::animateOpacity(Window* w, float from, float to, int ms) {
    auto* anim = new QVariantAnimation(this);
    anim->setStartValue(from);
    anim->setEndValue(to);
    anim->setDuration(ms);
    connect(anim, &QVariantAnimation::valueChanged, [=](const QVariant& v) {
        w->setOpacity(v.toFloat());
    });
    connect(anim, &QVariantAnimation::finished, anim, &QObject::deleteLater);
    anim->start();
}
