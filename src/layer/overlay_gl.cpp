#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>
#include <X11/Xlib.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "overlay_runtime.h"

#if defined(__GNUC__) || defined(__clang__)
#define SIGAW_GL_EXPORT __attribute__((visibility("default")))
#else
#define SIGAW_GL_EXPORT
#endif

#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#endif

#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#endif

#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif

#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif

#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif

#ifndef GL_VERTEX_ARRAY_BINDING_OES
#define GL_VERTEX_ARRAY_BINDING_OES 0x85B5
#endif

#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif

#ifndef GL_BLEND_EQUATION_RGB
#define GL_BLEND_EQUATION_RGB 0x8009
#endif

#ifndef GL_BLEND_EQUATION_ALPHA
#define GL_BLEND_EQUATION_ALPHA 0x883D
#endif

#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB 0x80C9
#endif

#ifndef GL_BLEND_DST_RGB
#define GL_BLEND_DST_RGB 0x80C8
#endif

#ifndef GL_BLEND_SRC_ALPHA
#define GL_BLEND_SRC_ALPHA 0x80CB
#endif

#ifndef GL_BLEND_DST_ALPHA
#define GL_BLEND_DST_ALPHA 0x80CA
#endif

using EglGetProcAddressFn = __eglMustCastToProperFunctionPointerType (*)(const char*);
using GlXSwapBuffersFn = void (*)(Display*, GLXDrawable);
using GlXDestroyContextFn = void (*)(Display*, GLXContext);
using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
using EglDestroyContextFn = EGLBoolean (*)(EGLDisplay, EGLContext);
using GlXGetProcAddressFn = __GLXextFuncPtr (*)(const GLubyte*);

using SigawPFNGLGENVERTEXARRAYSOESPROC = void (APIENTRYP)(GLsizei, GLuint*);
using SigawPFNGLBINDVERTEXARRAYOESPROC = void (APIENTRYP)(GLuint);
using SigawPFNGLDELETEVERTEXARRAYSOESPROC = void (APIENTRYP)(GLsizei, const GLuint*);
using SigawPFNGLBINDTEXTUREPROC = void (APIENTRYP)(GLenum, GLuint);
using SigawPFNGLBLENDFUNCPROC = void (APIENTRYP)(GLenum, GLenum);
using SigawPFNGLCOLORMASKPROC = void (APIENTRYP)(GLboolean, GLboolean, GLboolean, GLboolean);
using SigawPFNGLDELETETEXTURESPROC = void (APIENTRYP)(GLsizei, const GLuint*);
using SigawPFNGLDEPTHMASKPROC = void (APIENTRYP)(GLboolean);
using SigawPFNGLDISABLEPROC = void (APIENTRYP)(GLenum);
using SigawPFNGLDRAWARRAYSPROC = void (APIENTRYP)(GLenum, GLint, GLsizei);
using SigawPFNGLENABLEPROC = void (APIENTRYP)(GLenum);
using SigawPFNGLGENTEXTURESPROC = void (APIENTRYP)(GLsizei, GLuint*);
using SigawPFNGLGETBOOLEANVPROC = void (APIENTRYP)(GLenum, GLboolean*);
using SigawPFNGLGETINTEGERVPROC = void (APIENTRYP)(GLenum, GLint*);
using SigawPFNGLGETSTRINGPROC = const GLubyte* (APIENTRYP)(GLenum);
using SigawPFNGLISENABLEDPROC = GLboolean (APIENTRYP)(GLenum);
using SigawPFNGLPIXELSTOREIPROC = void (APIENTRYP)(GLenum, GLint);
using SigawPFNGLTEXIMAGE2DPROC =
    void (APIENTRYP)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using SigawPFNGLTEXPARAMETERIPROC = void (APIENTRYP)(GLenum, GLenum, GLint);
using SigawPFNGLTEXSUBIMAGE2DPROC =
    void (APIENTRYP)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
using SigawPFNGLVIEWPORTPROC = void (APIENTRYP)(GLint, GLint, GLsizei, GLsizei);

namespace {

enum class PlatformKind {
    Glx,
    Egl,
};

enum class ContextApi {
    Desktop,
    Gles,
};

enum class ShaderFlavor {
    Desktop120,
    Desktop330,
    Gles100,
    Gles300,
};

struct ContextKey {
    PlatformKind platform = PlatformKind::Glx;
    uintptr_t handle = 0;

    bool operator==(const ContextKey& other) const {
        return platform == other.platform && handle == other.handle;
    }
};

struct ContextKeyHash {
    size_t operator()(const ContextKey& key) const {
        return std::hash<uintptr_t>{}(key.handle) ^
               (static_cast<size_t>(key.platform) << 1);
    }
};

struct VertexAttribState {
    GLint enabled = 0;
    GLint size = 0;
    GLint stride = 0;
    GLint type = 0;
    GLint normalized = 0;
    GLint buffer_binding = 0;
    void* pointer = nullptr;
};

struct SavedGlState {
    GLint program = 0;
    GLint active_texture = GL_TEXTURE0;
    GLint texture_binding = 0;
    GLint array_buffer = 0;
    GLint framebuffer = 0;
    std::array<GLint, 4> viewport = {0, 0, 0, 0};
    std::array<GLboolean, 4> color_mask = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint unpack_alignment = 4;
    GLboolean blend = GL_FALSE;
    GLboolean depth_test = GL_FALSE;
    GLboolean depth_mask = GL_TRUE;
    GLboolean stencil_test = GL_FALSE;
    GLboolean cull_face = GL_FALSE;
    GLboolean scissor_test = GL_FALSE;
    GLint blend_src_rgb = GL_ONE;
    GLint blend_dst_rgb = GL_ZERO;
    GLint blend_src_alpha = GL_ONE;
    GLint blend_dst_alpha = GL_ZERO;
    GLint blend_equation_rgb = GL_FUNC_ADD;
    GLint blend_equation_alpha = GL_FUNC_ADD;
    GLint vertex_array = 0;
    VertexAttribState attrib0 = {};
};

#define SIGAW_GL_FUNCTIONS(X) \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture, "glActiveTexture") \
    X(PFNGLATTACHSHADERPROC, AttachShader, "glAttachShader") \
    X(PFNGLBINDATTRIBLOCATIONPROC, BindAttribLocation, "glBindAttribLocation") \
    X(PFNGLBINDBUFFERPROC, BindBuffer, "glBindBuffer") \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer, "glBindFramebuffer") \
    X(SigawPFNGLBINDTEXTUREPROC, BindTexture, "glBindTexture") \
    X(PFNGLBLENDEQUATIONPROC, BlendEquation, "glBlendEquation") \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, BlendEquationSeparate, "glBlendEquationSeparate") \
    X(SigawPFNGLBLENDFUNCPROC, BlendFunc, "glBlendFunc") \
    X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate, "glBlendFuncSeparate") \
    X(PFNGLBUFFERDATAPROC, BufferData, "glBufferData") \
    X(SigawPFNGLCOLORMASKPROC, ColorMask, "glColorMask") \
    X(PFNGLCOMPILESHADERPROC, CompileShader, "glCompileShader") \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram, "glCreateProgram") \
    X(PFNGLCREATESHADERPROC, CreateShader, "glCreateShader") \
    X(PFNGLDELETEBUFFERSPROC, DeleteBuffers, "glDeleteBuffers") \
    X(PFNGLDELETEPROGRAMPROC, DeleteProgram, "glDeleteProgram") \
    X(PFNGLDELETESHADERPROC, DeleteShader, "glDeleteShader") \
    X(SigawPFNGLDELETETEXTURESPROC, DeleteTextures, "glDeleteTextures") \
    X(SigawPFNGLDEPTHMASKPROC, DepthMask, "glDepthMask") \
    X(SigawPFNGLDISABLEPROC, Disable, "glDisable") \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, DisableVertexAttribArray, "glDisableVertexAttribArray") \
    X(SigawPFNGLDRAWARRAYSPROC, DrawArrays, "glDrawArrays") \
    X(SigawPFNGLENABLEPROC, Enable, "glEnable") \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray, "glEnableVertexAttribArray") \
    X(PFNGLGENBUFFERSPROC, GenBuffers, "glGenBuffers") \
    X(SigawPFNGLGENTEXTURESPROC, GenTextures, "glGenTextures") \
    X(SigawPFNGLGETBOOLEANVPROC, GetBooleanv, "glGetBooleanv") \
    X(SigawPFNGLGETINTEGERVPROC, GetIntegerv, "glGetIntegerv") \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog, "glGetProgramInfoLog") \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv, "glGetProgramiv") \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog, "glGetShaderInfoLog") \
    X(PFNGLGETSHADERIVPROC, GetShaderiv, "glGetShaderiv") \
    X(SigawPFNGLGETSTRINGPROC, GetString, "glGetString") \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation, "glGetUniformLocation") \
    X(PFNGLGETVERTEXATTRIBIVPROC, GetVertexAttribiv, "glGetVertexAttribiv") \
    X(PFNGLGETVERTEXATTRIBPOINTERVPROC, GetVertexAttribPointerv, "glGetVertexAttribPointerv") \
    X(SigawPFNGLISENABLEDPROC, IsEnabled, "glIsEnabled") \
    X(PFNGLLINKPROGRAMPROC, LinkProgram, "glLinkProgram") \
    X(SigawPFNGLPIXELSTOREIPROC, PixelStorei, "glPixelStorei") \
    X(PFNGLSHADERSOURCEPROC, ShaderSource, "glShaderSource") \
    X(SigawPFNGLTEXIMAGE2DPROC, TexImage2D, "glTexImage2D") \
    X(SigawPFNGLTEXPARAMETERIPROC, TexParameteri, "glTexParameteri") \
    X(SigawPFNGLTEXSUBIMAGE2DPROC, TexSubImage2D, "glTexSubImage2D") \
    X(PFNGLUNIFORM1IPROC, Uniform1i, "glUniform1i") \
    X(PFNGLUNIFORM2FPROC, Uniform2f, "glUniform2f") \
    X(PFNGLUNIFORM4FPROC, Uniform4f, "glUniform4f") \
    X(PFNGLUSEPROGRAMPROC, UseProgram, "glUseProgram") \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer, "glVertexAttribPointer") \
    X(SigawPFNGLVIEWPORTPROC, Viewport, "glViewport")

struct GlFunctions {
#define SIGAW_GL_FN(type, field, name) type field = nullptr;
    SIGAW_GL_FUNCTIONS(SIGAW_GL_FN)
#undef SIGAW_GL_FN
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays = nullptr;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays = nullptr;
    SigawPFNGLGENVERTEXARRAYSOESPROC GenVertexArraysOES = nullptr;
    SigawPFNGLBINDVERTEXARRAYOESPROC BindVertexArrayOES = nullptr;
    SigawPFNGLDELETEVERTEXARRAYSOESPROC DeleteVertexArraysOES = nullptr;
};

struct ContextState {
    sigaw::overlay::Runtime runtime;
    ContextApi api = ContextApi::Desktop;
    ShaderFlavor shader = ShaderFlavor::Desktop120;
    GLuint program = 0;
    GLuint texture = 0;
    GLuint vbo = 0;
    GLuint vao = 0;
    GLint uniform_texture = -1;
    GLint uniform_surface_size = -1;
    GLint uniform_panel_rect = -1;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    bool program_failed = false;
    bool use_vao = false;
    bool use_oes_vao = false;
};

static GlFunctions g_gl = {};
static std::once_flag g_real_symbol_once;
static std::mutex g_context_mutex;
static std::unordered_map<ContextKey, std::unique_ptr<ContextState>, ContextKeyHash> g_contexts;
static thread_local bool g_in_hook = false;

static GlXSwapBuffersFn g_real_glx_swap_buffers = nullptr;
static GlXDestroyContextFn g_real_glx_destroy_context = nullptr;
static EglSwapBuffersFn g_real_egl_swap_buffers = nullptr;
static EglDestroyContextFn g_real_egl_destroy_context = nullptr;
static GlXGetProcAddressFn g_real_glx_get_proc_address = nullptr;
static EglGetProcAddressFn g_real_egl_get_proc_address = nullptr;

static void resolve_real_symbols()
{
    g_real_glx_swap_buffers = reinterpret_cast<GlXSwapBuffersFn>(dlsym(RTLD_NEXT, "glXSwapBuffers"));
    g_real_glx_destroy_context = reinterpret_cast<GlXDestroyContextFn>(dlsym(RTLD_NEXT, "glXDestroyContext"));
    g_real_egl_swap_buffers = reinterpret_cast<EglSwapBuffersFn>(dlsym(RTLD_NEXT, "eglSwapBuffers"));
    g_real_egl_destroy_context = reinterpret_cast<EglDestroyContextFn>(dlsym(RTLD_NEXT, "eglDestroyContext"));
    g_real_glx_get_proc_address =
        reinterpret_cast<GlXGetProcAddressFn>(dlsym(RTLD_NEXT, "glXGetProcAddressARB"));
    g_real_egl_get_proc_address =
        reinterpret_cast<EglGetProcAddressFn>(dlsym(RTLD_NEXT, "eglGetProcAddress"));
}

static void ensure_real_symbols()
{
    std::call_once(g_real_symbol_once, resolve_real_symbols);
}

static void* resolve_gl_symbol(const char* name)
{
    ensure_real_symbols();

    if (g_real_egl_get_proc_address) {
        if (auto ptr = reinterpret_cast<void*>(g_real_egl_get_proc_address(name))) {
            return ptr;
        }
    }

    if (g_real_glx_get_proc_address) {
        if (auto ptr = reinterpret_cast<void*>(g_real_glx_get_proc_address(
                reinterpret_cast<const GLubyte*>(name)))) {
            return ptr;
        }
    }

    if (auto ptr = dlsym(RTLD_NEXT, name)) {
        return ptr;
    }
    return dlsym(RTLD_DEFAULT, name);
}

static bool ensure_gl_functions()
{
#define SIGAW_LOAD_GL_FN(type, field, name) \
    if (!g_gl.field) { \
        g_gl.field = reinterpret_cast<type>(resolve_gl_symbol(name)); \
    }
    SIGAW_GL_FUNCTIONS(SIGAW_LOAD_GL_FN)
#undef SIGAW_LOAD_GL_FN

    if (!g_gl.GenVertexArrays) {
        g_gl.GenVertexArrays = reinterpret_cast<PFNGLGENVERTEXARRAYSPROC>(
            resolve_gl_symbol("glGenVertexArrays"));
    }
    if (!g_gl.BindVertexArray) {
        g_gl.BindVertexArray = reinterpret_cast<PFNGLBINDVERTEXARRAYPROC>(
            resolve_gl_symbol("glBindVertexArray"));
    }
    if (!g_gl.DeleteVertexArrays) {
        g_gl.DeleteVertexArrays = reinterpret_cast<PFNGLDELETEVERTEXARRAYSPROC>(
            resolve_gl_symbol("glDeleteVertexArrays"));
    }
    if (!g_gl.GenVertexArraysOES) {
        g_gl.GenVertexArraysOES = reinterpret_cast<SigawPFNGLGENVERTEXARRAYSOESPROC>(
            resolve_gl_symbol("glGenVertexArraysOES"));
    }
    if (!g_gl.BindVertexArrayOES) {
        g_gl.BindVertexArrayOES = reinterpret_cast<SigawPFNGLBINDVERTEXARRAYOESPROC>(
            resolve_gl_symbol("glBindVertexArrayOES"));
    }
    if (!g_gl.DeleteVertexArraysOES) {
        g_gl.DeleteVertexArraysOES = reinterpret_cast<SigawPFNGLDELETEVERTEXARRAYSOESPROC>(
            resolve_gl_symbol("glDeleteVertexArraysOES"));
    }

    return g_gl.ActiveTexture &&
           g_gl.AttachShader &&
           g_gl.BindAttribLocation &&
           g_gl.BindBuffer &&
           g_gl.BindFramebuffer &&
           g_gl.BindTexture &&
           g_gl.BlendEquation &&
           g_gl.BlendFunc &&
           g_gl.BufferData &&
           g_gl.ColorMask &&
           g_gl.CompileShader &&
           g_gl.CreateProgram &&
           g_gl.CreateShader &&
           g_gl.DeleteBuffers &&
           g_gl.DeleteProgram &&
           g_gl.DeleteShader &&
           g_gl.DeleteTextures &&
           g_gl.Disable &&
           g_gl.DisableVertexAttribArray &&
           g_gl.DrawArrays &&
           g_gl.Enable &&
           g_gl.EnableVertexAttribArray &&
           g_gl.GenBuffers &&
           g_gl.GenTextures &&
           g_gl.GetBooleanv &&
           g_gl.GetIntegerv &&
           g_gl.GetProgramInfoLog &&
           g_gl.GetProgramiv &&
           g_gl.GetShaderInfoLog &&
           g_gl.GetShaderiv &&
           g_gl.GetString &&
           g_gl.GetUniformLocation &&
           g_gl.GetVertexAttribiv &&
           g_gl.GetVertexAttribPointerv &&
           g_gl.IsEnabled &&
           g_gl.LinkProgram &&
           g_gl.PixelStorei &&
           g_gl.ShaderSource &&
           g_gl.TexImage2D &&
           g_gl.TexParameteri &&
           g_gl.TexSubImage2D &&
           g_gl.Uniform1i &&
           g_gl.Uniform2f &&
           g_gl.Uniform4f &&
           g_gl.UseProgram &&
           g_gl.VertexAttribPointer &&
           g_gl.Viewport;
}

static int parse_major_version(std::string_view version)
{
    for (size_t i = 0; i < version.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(version[i]))) {
            continue;
        }

        int value = 0;
        while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) {
            value = value * 10 + (version[i] - '0');
            ++i;
        }
        return value;
    }

    return 0;
}

static ShaderFlavor choose_shader_flavor(ContextApi api)
{
    const auto* version_bytes = g_gl.GetString(GL_VERSION);
    const std::string version = version_bytes
        ? reinterpret_cast<const char*>(version_bytes)
        : std::string();
    const int major = parse_major_version(version);

    if (api == ContextApi::Gles) {
        return major >= 3 ? ShaderFlavor::Gles300 : ShaderFlavor::Gles100;
    }

    GLint profile_mask = 0;
    if (major > 3 || major == 3) {
        g_gl.GetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile_mask);
    }
    if ((profile_mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0) {
        return ShaderFlavor::Desktop330;
    }
    return ShaderFlavor::Desktop120;
}

static bool shader_uses_vao(ShaderFlavor flavor)
{
    return flavor == ShaderFlavor::Desktop330;
}

static const char* vertex_shader_source(ShaderFlavor flavor)
{
    switch (flavor) {
        case ShaderFlavor::Desktop330:
            return
                "#version 330 core\n"
                "layout(location = 0) in vec2 a_position;\n"
                "void main() {\n"
                "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
                "}\n";
        case ShaderFlavor::Gles300:
            return
                "#version 300 es\n"
                "layout(location = 0) in vec2 a_position;\n"
                "void main() {\n"
                "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
                "}\n";
        case ShaderFlavor::Gles100:
            return
                "attribute vec2 a_position;\n"
                "void main() {\n"
                "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
                "}\n";
        case ShaderFlavor::Desktop120:
        default:
            return
                "#version 120\n"
                "attribute vec2 a_position;\n"
                "void main() {\n"
                "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
                "}\n";
    }
}

static const char* fragment_shader_source(ShaderFlavor flavor)
{
    switch (flavor) {
        case ShaderFlavor::Desktop330:
            return
                "#version 330 core\n"
                "uniform sampler2D u_texture;\n"
                "uniform vec2 u_surface_size;\n"
                "uniform vec4 u_panel_rect;\n"
                "out vec4 out_color;\n"
                "void main() {\n"
                "  vec2 pixel = vec2(gl_FragCoord.x - 0.5, u_surface_size.y - gl_FragCoord.y - 0.5);\n"
                "  vec2 rel = pixel - u_panel_rect.xy;\n"
                "  if (rel.x < 0.0 || rel.y < 0.0 || rel.x >= u_panel_rect.z || rel.y >= u_panel_rect.w) {\n"
                "    out_color = vec4(0.0);\n"
                "    return;\n"
                "  }\n"
                "  out_color = texture(u_texture, rel / u_panel_rect.zw);\n"
                "}\n";
        case ShaderFlavor::Gles300:
            return
                "#version 300 es\n"
                "precision mediump float;\n"
                "uniform sampler2D u_texture;\n"
                "uniform vec2 u_surface_size;\n"
                "uniform vec4 u_panel_rect;\n"
                "out vec4 out_color;\n"
                "void main() {\n"
                "  vec2 pixel = vec2(gl_FragCoord.x - 0.5, u_surface_size.y - gl_FragCoord.y - 0.5);\n"
                "  vec2 rel = pixel - u_panel_rect.xy;\n"
                "  if (rel.x < 0.0 || rel.y < 0.0 || rel.x >= u_panel_rect.z || rel.y >= u_panel_rect.w) {\n"
                "    out_color = vec4(0.0);\n"
                "    return;\n"
                "  }\n"
                "  out_color = texture(u_texture, rel / u_panel_rect.zw);\n"
                "}\n";
        case ShaderFlavor::Gles100:
            return
                "precision mediump float;\n"
                "uniform sampler2D u_texture;\n"
                "uniform vec2 u_surface_size;\n"
                "uniform vec4 u_panel_rect;\n"
                "void main() {\n"
                "  vec2 pixel = vec2(gl_FragCoord.x - 0.5, u_surface_size.y - gl_FragCoord.y - 0.5);\n"
                "  vec2 rel = pixel - u_panel_rect.xy;\n"
                "  if (rel.x < 0.0 || rel.y < 0.0 || rel.x >= u_panel_rect.z || rel.y >= u_panel_rect.w) {\n"
                "    gl_FragColor = vec4(0.0);\n"
                "    return;\n"
                "  }\n"
                "  gl_FragColor = texture2D(u_texture, rel / u_panel_rect.zw);\n"
                "}\n";
        case ShaderFlavor::Desktop120:
        default:
            return
                "#version 120\n"
                "uniform sampler2D u_texture;\n"
                "uniform vec2 u_surface_size;\n"
                "uniform vec4 u_panel_rect;\n"
                "void main() {\n"
                "  vec2 pixel = vec2(gl_FragCoord.x - 0.5, u_surface_size.y - gl_FragCoord.y - 0.5);\n"
                "  vec2 rel = pixel - u_panel_rect.xy;\n"
                "  if (rel.x < 0.0 || rel.y < 0.0 || rel.x >= u_panel_rect.z || rel.y >= u_panel_rect.w) {\n"
                "    gl_FragColor = vec4(0.0);\n"
                "    return;\n"
                "  }\n"
                "  gl_FragColor = texture2D(u_texture, rel / u_panel_rect.zw);\n"
                "}\n";
    }
}

static GLuint compile_shader(GLenum type, const char* source)
{
    const GLuint shader = g_gl.CreateShader(type);
    if (shader == 0) {
        return 0;
    }

    g_gl.ShaderSource(shader, 1, &source, nullptr);
    g_gl.CompileShader(shader);

    GLint status = GL_FALSE;
    g_gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) {
        return shader;
    }

    GLint log_length = 0;
    g_gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<size_t>(std::max(0, log_length)), '\0');
    if (!log.empty()) {
        g_gl.GetShaderInfoLog(shader, log_length, nullptr, log.data());
    }
    fprintf(stderr, "[sigaw] OpenGL shader compile failed: %s\n", log.c_str());
    g_gl.DeleteShader(shader);
    return 0;
}

static bool link_program(ContextState& state)
{
    const GLuint vert = compile_shader(GL_VERTEX_SHADER, vertex_shader_source(state.shader));
    if (vert == 0) {
        return false;
    }

    const GLuint frag = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source(state.shader));
    if (frag == 0) {
        g_gl.DeleteShader(vert);
        return false;
    }

    state.program = g_gl.CreateProgram();
    if (state.program == 0) {
        g_gl.DeleteShader(vert);
        g_gl.DeleteShader(frag);
        return false;
    }

    g_gl.AttachShader(state.program, vert);
    g_gl.AttachShader(state.program, frag);
    g_gl.BindAttribLocation(state.program, 0, "a_position");
    g_gl.LinkProgram(state.program);
    g_gl.DeleteShader(vert);
    g_gl.DeleteShader(frag);

    GLint status = GL_FALSE;
    g_gl.GetProgramiv(state.program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        GLint log_length = 0;
        g_gl.GetProgramiv(state.program, GL_INFO_LOG_LENGTH, &log_length);
        std::string log(static_cast<size_t>(std::max(0, log_length)), '\0');
        if (!log.empty()) {
            g_gl.GetProgramInfoLog(state.program, log_length, nullptr, log.data());
        }
        fprintf(stderr, "[sigaw] OpenGL program link failed: %s\n", log.c_str());
        g_gl.DeleteProgram(state.program);
        state.program = 0;
        return false;
    }

    state.uniform_texture = g_gl.GetUniformLocation(state.program, "u_texture");
    state.uniform_surface_size = g_gl.GetUniformLocation(state.program, "u_surface_size");
    state.uniform_panel_rect = g_gl.GetUniformLocation(state.program, "u_panel_rect");
    return state.uniform_texture >= 0 &&
           state.uniform_surface_size >= 0 &&
           state.uniform_panel_rect >= 0;
}

static void bind_vao(GLuint vao, bool use_oes)
{
    if (use_oes) {
        g_gl.BindVertexArrayOES(vao);
    } else {
        g_gl.BindVertexArray(vao);
    }
}

static void delete_vao(GLuint vao, bool use_oes)
{
    if (vao == 0) {
        return;
    }
    if (use_oes) {
        g_gl.DeleteVertexArraysOES(1, &vao);
    } else {
        g_gl.DeleteVertexArrays(1, &vao);
    }
}

static bool ensure_geometry(ContextState& state)
{
    if (state.vbo == 0) {
        static const GLfloat vertices[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f,
        };

        g_gl.GenBuffers(1, &state.vbo);
        if (state.vbo == 0) {
            return false;
        }
        g_gl.BindBuffer(GL_ARRAY_BUFFER, state.vbo);
        g_gl.BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    }

    if (state.use_vao && state.vao == 0) {
        if (state.use_oes_vao) {
            if (!g_gl.GenVertexArraysOES || !g_gl.BindVertexArrayOES || !g_gl.DeleteVertexArraysOES) {
                return false;
            }
            g_gl.GenVertexArraysOES(1, &state.vao);
        } else {
            if (!g_gl.GenVertexArrays || !g_gl.BindVertexArray || !g_gl.DeleteVertexArrays) {
                return false;
            }
            g_gl.GenVertexArrays(1, &state.vao);
        }
        if (state.vao == 0) {
            return false;
        }

        bind_vao(state.vao, state.use_oes_vao);
        g_gl.BindBuffer(GL_ARRAY_BUFFER, state.vbo);
        g_gl.EnableVertexAttribArray(0);
        g_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, nullptr);
    }

    return true;
}

static bool ensure_program(ContextState& state, ContextApi api)
{
    const ShaderFlavor desired = choose_shader_flavor(api);
    const bool desired_vao = shader_uses_vao(desired);
    const bool desired_oes_vao = api == ContextApi::Gles &&
        !desired_vao &&
        g_gl.GenVertexArraysOES && g_gl.BindVertexArrayOES && g_gl.DeleteVertexArraysOES;

    if (state.program != 0 && (state.shader != desired || state.use_vao != desired_vao ||
                               state.use_oes_vao != desired_oes_vao)) {
        if (state.program != 0) {
            g_gl.DeleteProgram(state.program);
        }
        delete_vao(state.vao, state.use_oes_vao);
        if (state.vbo != 0) {
            g_gl.DeleteBuffers(1, &state.vbo);
            state.vbo = 0;
        }
        if (state.texture != 0) {
            g_gl.DeleteTextures(1, &state.texture);
            state.texture = 0;
        }
        state.program = 0;
        state.texture_width = 0;
        state.texture_height = 0;
    }

    state.api = api;
    state.shader = desired;
    state.use_vao = desired_vao || desired_oes_vao;
    state.use_oes_vao = desired_oes_vao;

    if (state.program_failed) {
        return false;
    }

    if (state.program == 0 && !link_program(state)) {
        state.program_failed = true;
        return false;
    }

    if (!ensure_geometry(state)) {
        return false;
    }

    if (state.texture == 0) {
        g_gl.GenTextures(1, &state.texture);
        if (state.texture == 0) {
            return false;
        }
        g_gl.BindTexture(GL_TEXTURE_2D, state.texture);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    return true;
}

static void save_vertex_attrib_state(VertexAttribState& out)
{
    g_gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &out.enabled);
    g_gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &out.size);
    g_gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &out.stride);
    g_gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &out.type);
    g_gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &out.normalized);
    g_gl.GetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &out.buffer_binding);
    g_gl.GetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &out.pointer);
}

static void restore_vertex_attrib_state(const VertexAttribState& state)
{
    g_gl.BindBuffer(GL_ARRAY_BUFFER, state.buffer_binding);
    g_gl.VertexAttribPointer(
        0,
        state.size > 0 ? state.size : 4,
        state.type != 0 ? static_cast<GLenum>(state.type) : GL_FLOAT,
        state.normalized ? GL_TRUE : GL_FALSE,
        state.stride,
        state.pointer
    );

    if (state.enabled) {
        g_gl.EnableVertexAttribArray(0);
    } else {
        g_gl.DisableVertexAttribArray(0);
    }
}

static void save_gl_state(SavedGlState& state, const ContextState& ctx)
{
    g_gl.GetIntegerv(GL_CURRENT_PROGRAM, &state.program);
    g_gl.GetIntegerv(GL_ACTIVE_TEXTURE, &state.active_texture);
    g_gl.ActiveTexture(GL_TEXTURE0);
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture_binding);
    g_gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &state.array_buffer);
    g_gl.GetIntegerv(ctx.api == ContextApi::Desktop ? GL_DRAW_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING,
                     &state.framebuffer);
    g_gl.GetIntegerv(GL_VIEWPORT, state.viewport.data());
    g_gl.GetBooleanv(GL_COLOR_WRITEMASK, state.color_mask.data());
    g_gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &state.unpack_alignment);
    state.blend = g_gl.IsEnabled(GL_BLEND);
    state.depth_test = g_gl.IsEnabled(GL_DEPTH_TEST);
    g_gl.GetBooleanv(GL_DEPTH_WRITEMASK, &state.depth_mask);
    state.stencil_test = g_gl.IsEnabled(GL_STENCIL_TEST);
    state.cull_face = g_gl.IsEnabled(GL_CULL_FACE);
    state.scissor_test = g_gl.IsEnabled(GL_SCISSOR_TEST);
    g_gl.GetIntegerv(GL_BLEND_SRC_RGB, &state.blend_src_rgb);
    g_gl.GetIntegerv(GL_BLEND_DST_RGB, &state.blend_dst_rgb);
    g_gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &state.blend_src_alpha);
    g_gl.GetIntegerv(GL_BLEND_DST_ALPHA, &state.blend_dst_alpha);
    g_gl.GetIntegerv(GL_BLEND_EQUATION_RGB, &state.blend_equation_rgb);
    g_gl.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &state.blend_equation_alpha);

    if (ctx.use_vao) {
        g_gl.GetIntegerv(ctx.use_oes_vao ? GL_VERTEX_ARRAY_BINDING_OES : GL_VERTEX_ARRAY_BINDING,
                         &state.vertex_array);
    } else {
        save_vertex_attrib_state(state.attrib0);
    }
}

static void restore_gl_state(const SavedGlState& state, const ContextState& ctx)
{
    if (ctx.use_vao) {
        bind_vao(static_cast<GLuint>(state.vertex_array), ctx.use_oes_vao);
    } else {
        restore_vertex_attrib_state(state.attrib0);
    }

    g_gl.BindBuffer(GL_ARRAY_BUFFER, state.array_buffer);
    g_gl.BindFramebuffer(ctx.api == ContextApi::Desktop ? GL_DRAW_FRAMEBUFFER : GL_FRAMEBUFFER,
                         static_cast<GLuint>(state.framebuffer));
    g_gl.Viewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
    g_gl.ColorMask(state.color_mask[0], state.color_mask[1], state.color_mask[2], state.color_mask[3]);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, state.unpack_alignment);

    if (state.blend) {
        g_gl.Enable(GL_BLEND);
    } else {
        g_gl.Disable(GL_BLEND);
    }
    if (g_gl.BlendFuncSeparate) {
        g_gl.BlendFuncSeparate(state.blend_src_rgb, state.blend_dst_rgb,
                               state.blend_src_alpha, state.blend_dst_alpha);
    } else {
        g_gl.BlendFunc(state.blend_src_rgb, state.blend_dst_rgb);
    }
    if (g_gl.BlendEquationSeparate) {
        g_gl.BlendEquationSeparate(state.blend_equation_rgb, state.blend_equation_alpha);
    } else {
        g_gl.BlendEquation(state.blend_equation_rgb);
    }

    if (state.depth_test) {
        g_gl.Enable(GL_DEPTH_TEST);
    } else {
        g_gl.Disable(GL_DEPTH_TEST);
    }
    g_gl.DepthMask(state.depth_mask);

    if (state.stencil_test) {
        g_gl.Enable(GL_STENCIL_TEST);
    } else {
        g_gl.Disable(GL_STENCIL_TEST);
    }
    if (state.cull_face) {
        g_gl.Enable(GL_CULL_FACE);
    } else {
        g_gl.Disable(GL_CULL_FACE);
    }
    if (state.scissor_test) {
        g_gl.Enable(GL_SCISSOR_TEST);
    } else {
        g_gl.Disable(GL_SCISSOR_TEST);
    }

    g_gl.ActiveTexture(state.active_texture);
    g_gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(state.texture_binding));
    g_gl.UseProgram(state.program);
}

static void destroy_context_resources(ContextState& state)
{
    if (!ensure_gl_functions()) {
        return;
    }

    if (state.program != 0) {
        g_gl.DeleteProgram(state.program);
        state.program = 0;
    }
    if (state.texture != 0) {
        g_gl.DeleteTextures(1, &state.texture);
        state.texture = 0;
    }
    if (state.vbo != 0) {
        g_gl.DeleteBuffers(1, &state.vbo);
        state.vbo = 0;
    }
    delete_vao(state.vao, state.use_oes_vao);
    state.vao = 0;
}

static ContextState& get_context_state(PlatformKind platform, uintptr_t handle)
{
    std::lock_guard<std::mutex> lock(g_context_mutex);
    auto [it, _] = g_contexts.emplace(
        ContextKey{platform, handle},
        std::make_unique<ContextState>()
    );
    return *it->second;
}

static void drop_context_state(PlatformKind platform, uintptr_t handle, bool current)
{
    std::unique_ptr<ContextState> removed;
    {
        std::lock_guard<std::mutex> lock(g_context_mutex);
        auto it = g_contexts.find(ContextKey{platform, handle});
        if (it == g_contexts.end()) {
            return;
        }
        removed = std::move(it->second);
        g_contexts.erase(it);
    }

    if (current && removed) {
        destroy_context_resources(*removed);
    }
}

static bool query_glx_surface_size(Display* display, GLXDrawable drawable, uint32_t* width, uint32_t* height)
{
    unsigned int w = 0;
    unsigned int h = 0;
    glXQueryDrawable(display, drawable, GLX_WIDTH, &w);
    glXQueryDrawable(display, drawable, GLX_HEIGHT, &h);
    *width = w;
    *height = h;
    return w > 0 && h > 0;
}

static bool query_egl_surface_size(EGLDisplay display, EGLSurface surface, uint32_t* width, uint32_t* height)
{
    EGLint w = 0;
    EGLint h = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &w) != EGL_TRUE ||
        eglQuerySurface(display, surface, EGL_HEIGHT, &h) != EGL_TRUE) {
        return false;
    }

    *width = static_cast<uint32_t>(w);
    *height = static_cast<uint32_t>(h);
    return w > 0 && h > 0;
}

static ContextApi current_egl_api()
{
    return eglQueryAPI() == EGL_OPENGL_ES_API ? ContextApi::Gles : ContextApi::Desktop;
}

static bool upload_texture(ContextState& state, const sigaw::overlay::PreparedFrame& frame)
{
    const bool resized =
        state.texture_width != frame.width || state.texture_height != frame.height;
    if (!frame.changed && !resized) {
        return true;
    }

    g_gl.ActiveTexture(GL_TEXTURE0);
    g_gl.BindTexture(GL_TEXTURE_2D, state.texture);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (resized) {
        g_gl.TexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            static_cast<GLsizei>(frame.width),
            static_cast<GLsizei>(frame.height),
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            frame.rgba
        );
        state.texture_width = frame.width;
        state.texture_height = frame.height;
        return true;
    }

    g_gl.TexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        static_cast<GLsizei>(frame.width),
        static_cast<GLsizei>(frame.height),
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        frame.rgba
    );
    return true;
}

static bool render_overlay(ContextState& state, ContextApi api,
                           const sigaw::overlay::PreparedFrame& frame,
                           uint32_t surface_width, uint32_t surface_height)
{
    if (!ensure_gl_functions() || !ensure_program(state, api)) {
        return false;
    }

    SavedGlState saved = {};
    save_gl_state(saved, state);

    if (!upload_texture(state, frame)) {
        restore_gl_state(saved, state);
        return false;
    }

    g_gl.BindFramebuffer(api == ContextApi::Desktop ? GL_DRAW_FRAMEBUFFER : GL_FRAMEBUFFER, 0);
    g_gl.Viewport(0, 0, static_cast<GLsizei>(surface_width), static_cast<GLsizei>(surface_height));
    g_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_gl.Disable(GL_DEPTH_TEST);
    g_gl.DepthMask(GL_FALSE);
    g_gl.Disable(GL_STENCIL_TEST);
    g_gl.Disable(GL_CULL_FACE);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Enable(GL_BLEND);
    g_gl.BlendEquation(GL_FUNC_ADD);
    g_gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    g_gl.UseProgram(state.program);
    g_gl.Uniform1i(state.uniform_texture, 0);
    g_gl.Uniform2f(state.uniform_surface_size,
                   static_cast<GLfloat>(surface_width),
                   static_cast<GLfloat>(surface_height));
    g_gl.Uniform4f(state.uniform_panel_rect,
                   static_cast<GLfloat>(frame.placement.x),
                   static_cast<GLfloat>(frame.placement.y),
                   static_cast<GLfloat>(frame.width),
                   static_cast<GLfloat>(frame.height));

    if (state.use_vao) {
        bind_vao(state.vao, state.use_oes_vao);
    } else {
        g_gl.BindBuffer(GL_ARRAY_BUFFER, state.vbo);
        g_gl.EnableVertexAttribArray(0);
        g_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, nullptr);
    }

    g_gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    restore_gl_state(saved, state);
    return true;
}

static void render_for_glx(Display* display, GLXDrawable drawable)
{
    const GLXContext context = glXGetCurrentContext();
    if (context == nullptr || g_in_hook) {
        return;
    }

    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    if (!query_glx_surface_size(display, drawable, &surface_width, &surface_height)) {
        return;
    }

    auto& state = get_context_state(PlatformKind::Glx, reinterpret_cast<uintptr_t>(context));
    const auto frame = state.runtime.prepare(surface_width, surface_height);
    if (frame.empty()) {
        return;
    }

    g_in_hook = true;
    render_overlay(state, ContextApi::Desktop, frame, surface_width, surface_height);
    g_in_hook = false;
}

static void render_for_egl(EGLDisplay display, EGLSurface surface)
{
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT || g_in_hook) {
        return;
    }

    const EGLenum api = eglQueryAPI();
    if (api != EGL_OPENGL_API && api != EGL_OPENGL_ES_API) {
        return;
    }

    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    if (!query_egl_surface_size(display, surface, &surface_width, &surface_height)) {
        return;
    }

    auto& state = get_context_state(PlatformKind::Egl, reinterpret_cast<uintptr_t>(context));
    const auto frame = state.runtime.prepare(surface_width, surface_height);
    if (frame.empty()) {
        return;
    }

    g_in_hook = true;
    render_overlay(state, current_egl_api(), frame, surface_width, surface_height);
    g_in_hook = false;
}

} /* namespace */

extern "C" {

SIGAW_GL_EXPORT void glXSwapBuffers(Display* display, GLXDrawable drawable)
{
    ensure_real_symbols();
    render_for_glx(display, drawable);
    if (g_real_glx_swap_buffers) {
        g_real_glx_swap_buffers(display, drawable);
    }
}

SIGAW_GL_EXPORT EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    ensure_real_symbols();
    render_for_egl(display, surface);
    if (!g_real_egl_swap_buffers) {
        return EGL_FALSE;
    }
    return g_real_egl_swap_buffers(display, surface);
}

SIGAW_GL_EXPORT void glXDestroyContext(Display* display, GLXContext context)
{
    ensure_real_symbols();
    const bool current = glXGetCurrentContext() == context;
    drop_context_state(PlatformKind::Glx, reinterpret_cast<uintptr_t>(context), current);
    if (g_real_glx_destroy_context) {
        g_real_glx_destroy_context(display, context);
    }
}

SIGAW_GL_EXPORT EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context)
{
    ensure_real_symbols();
    const bool current = eglGetCurrentContext() == context;
    drop_context_state(PlatformKind::Egl, reinterpret_cast<uintptr_t>(context), current);
    if (!g_real_egl_destroy_context) {
        return EGL_FALSE;
    }
    return g_real_egl_destroy_context(display, context);
}

} /* extern "C" */
