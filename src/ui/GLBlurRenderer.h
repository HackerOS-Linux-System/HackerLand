#pragma once
#include <QObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLFramebufferObject>
#include <QSize>
#include <QRect>
#include <QImage>

// ─────────────────────────────────────────────────────────────────────────────
// GLBlurRenderer — GPU Gaussian blur via two-pass OpenGL shaders
//
// Usage:
//   GLBlurRenderer blur;
//   blur.initialize();  // once, after GL context is current
//   QImage result = blur.blurImage(sourceImage, radius);
//
// Falls back gracefully to CPU fastBlur if GL is unavailable.
// ─────────────────────────────────────────────────────────────────────────────
class GLBlurRenderer : public QObject, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit GLBlurRenderer(QObject* parent = nullptr);
    ~GLBlurRenderer() override;

    bool initialize();  // call once with GL context current
    bool isReady() const { return m_ready; }

    // Blur imageRect region from sourcePixmap, return blurred QImage
    // radius: blur strength (1-20 useful range)
    QImage blurRegion(const QImage& source, const QRect& region, int radius);

private:
    bool compileShaders();
    void cleanup();

    bool                    m_ready  = false;
    QOpenGLShaderProgram*   m_hBlur  = nullptr;  // horizontal pass
    QOpenGLShaderProgram*   m_vBlur  = nullptr;  // vertical pass
    GLuint                  m_vao    = 0;
    GLuint                  m_vbo    = 0;
    GLuint                  m_tex[2] = {0, 0};
    GLuint                  m_fbo[2] = {0, 0};
    QSize                   m_fboSize;

    static constexpr const char* kHBlurSrc = R"GLSL(
        #version 330 core
        in  vec2 uv;
        out vec4 fragColor;
        uniform sampler2D tex;
        uniform float     texelW;
        uniform int       radius;

        void main() {
            vec4 sum   = vec4(0.0);
            float wSum = 0.0;
            float sigma = float(radius) / 2.0 + 0.5;
            for (int i = -radius; i <= radius; ++i) {
                float w = exp(-0.5 * float(i*i) / (sigma*sigma));
                sum  += texture(tex, uv + vec2(float(i) * texelW, 0.0)) * w;
                wSum += w;
            }
            fragColor = sum / wSum;
        }
    )GLSL";

    static constexpr const char* kVBlurSrc = R"GLSL(
        #version 330 core
        in  vec2 uv;
        out vec4 fragColor;
        uniform sampler2D tex;
        uniform float     texelH;
        uniform int       radius;

        void main() {
            vec4 sum   = vec4(0.0);
            float wSum = 0.0;
            float sigma = float(radius) / 2.0 + 0.5;
            for (int i = -radius; i <= radius; ++i) {
                float w = exp(-0.5 * float(i*i) / (sigma*sigma));
                sum  += texture(tex, uv + vec2(0.0, float(i) * texelH)) * w;
                wSum += w;
            }
            fragColor = sum / wSum;
        }
    )GLSL";

    static constexpr const char* kVertSrc = R"GLSL(
        #version 330 core
        layout(location=0) in vec2 pos;
        out vec2 uv;
        void main() {
            uv = pos * 0.5 + 0.5;
            gl_Position = vec4(pos, 0.0, 1.0);
        }
    )GLSL";
};
