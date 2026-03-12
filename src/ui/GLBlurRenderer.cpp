#include "GLBlurRenderer.h"
#include <QDebug>
#include <QOpenGLContext>

GLBlurRenderer::GLBlurRenderer(QObject* parent) : QObject(parent) {}

GLBlurRenderer::~GLBlurRenderer() { cleanup(); }

bool GLBlurRenderer::initialize() {
    if (m_ready) return true;
    if (!QOpenGLContext::currentContext()) {
        qWarning() << "[GLBlur] no current GL context";
        return false;
    }
    initializeOpenGLFunctions();
    if (!compileShaders()) return false;

    // Full-screen quad
    const float quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    glGenTextures(2, m_tex);
    glGenFramebuffers(2, m_fbo);
    for (int i=0; i<2; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    m_ready = true;
    qInfo() << "[GLBlur] initialized";
    return true;
}

bool GLBlurRenderer::compileShaders() {
    m_hBlur = new QOpenGLShaderProgram(this);
    if (!m_hBlur->addShaderFromSourceCode(QOpenGLShader::Vertex,   kVertSrc) ||
        !m_hBlur->addShaderFromSourceCode(QOpenGLShader::Fragment, kHBlurSrc) ||
        !m_hBlur->link()) {
        qWarning() << "[GLBlur] hBlur compile failed:" << m_hBlur->log();
    return false;
        }
        m_vBlur = new QOpenGLShaderProgram(this);
        if (!m_vBlur->addShaderFromSourceCode(QOpenGLShader::Vertex,   kVertSrc) ||
            !m_vBlur->addShaderFromSourceCode(QOpenGLShader::Fragment, kVBlurSrc) ||
            !m_vBlur->link()) {
            qWarning() << "[GLBlur] vBlur compile failed:" << m_vBlur->log();
        return false;
            }
            return true;
}

void GLBlurRenderer::cleanup() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    glDeleteTextures(2, m_tex);
    glDeleteFramebuffers(2, m_fbo);
    m_ready = false;
}

QImage GLBlurRenderer::blurRegion(const QImage& source, const QRect& region, int radius) {
    if (!m_ready || source.isNull() || region.isEmpty()) return {};

    QImage crop = source.copy(region)
    .convertToFormat(QImage::Format_RGBA8888);
    const int w = crop.width(), h = crop.height();

    // Resize FBOs if needed
    if (m_fboSize != QSize(w,h)) {
        m_fboSize = {w,h};
        for (int i=0; i<2; ++i) {
            glBindTexture(GL_TEXTURE_2D, m_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, m_tex[i], 0);
        }
    }

    // Upload source to tex[0]
    glBindTexture(GL_TEXTURE_2D, m_tex[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                    GL_RGBA, GL_UNSIGNED_BYTE, crop.constBits());

    glViewport(0, 0, w, h);
    glBindVertexArray(m_vao);

    // Horizontal pass: tex[0] → fbo[1]
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[1]);
    m_hBlur->bind();
    m_hBlur->setUniformValue("tex",    0);
    m_hBlur->setUniformValue("texelW", 1.0f / w);
    m_hBlur->setUniformValue("radius", qMin(radius, 15));
    glBindTexture(GL_TEXTURE_2D, m_tex[0]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_hBlur->release();

    // Vertical pass: tex[1] → fbo[0]
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[0]);
    m_vBlur->bind();
    m_vBlur->setUniformValue("tex",    0);
    m_vBlur->setUniformValue("texelH", 1.0f / h);
    m_vBlur->setUniformValue("radius", qMin(radius, 15));
    glBindTexture(GL_TEXTURE_2D, m_tex[1]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vBlur->release();

    glBindVertexArray(0);

    // Read back result
    QImage result(w, h, QImage::Format_RGBA8888);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[0]);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, result.bits());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return result.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
