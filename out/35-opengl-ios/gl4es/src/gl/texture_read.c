// Changed for MYIOSDECK (spec 35 clarification 13, out/35-opengl-ios/CHANGES.md): glGetTexImage
// reads level 0 of a 2D or rectangle texture for every format wined3d's DirectDraw uses (attached
// to a framebuffer when GLES can render to its format, else drawn into an RGBA8 one, then
// converted on the CPU), honours a pack buffer for the other levels, and logs a format that still
// fails once. gl4es is MIT (its LICENSE).
#include "texture.h"

#include "../glx/hardext.h"
#include "../glx/streaming.h"
#include "array.h"
#include "blit.h"
#include "decompress.h"
#include "debug.h"
#include "enum_info.h"
#include "fpe.h"
#include "framebuffers.h"
#include "gles.h"
#include "init.h"
#include "loader.h"
#include "matrix.h"
#include "pixel.h"
#include "raster.h"

//#define DEBUG
#ifdef DEBUG
#define DBG(a) a
#else
#define DBG(a)
#endif

static int inline nlevel(int size, int level) {
    if(size) {
        size>>=level;
        if(!size) size=1;
    }
    return size;
}

void APIENTRY_GL4ES gl4es_glCopyTexImage2D(GLenum target,  GLint level,  GLenum internalformat,  GLint x,  GLint y,  
                                GLsizei width,  GLsizei height,  GLint border) {
    DBG(printf("glCopyTexImage2D(%s, %i, %s, %i, %i, %i, %i, %i), glstate->fbo.current_fb=%p\n", PrintEnum(target), level, PrintEnum(internalformat), x, y, width, height, border, glstate->fbo.current_fb);)
     //PUSH_IF_COMPILING(glCopyTexImage2D);
    FLUSH_BEGINEND;
    const GLuint itarget = what_target(target);

    // actually bound if targeting shared TEX2D
    realize_bound(glstate->texture.active, target);

    if (globals4es.skiptexcopies) {
        DBG(printf("glCopyTexImage2D skipped.\n"));
        return;
    }

    errorGL();

    // "Unmap" if buffer mapped...
    glbuffer_t *pack = glstate->vao->pack;
    glbuffer_t *unpack = glstate->vao->unpack;
    glstate->vao->pack = NULL;
    glstate->vao->unpack = NULL;
    
    readfboBegin(); // multiple readfboBegin() can be chained...
    gltexture_t* bound = glstate->texture.bound[glstate->texture.active][itarget];

    if(glstate->fbo.current_fb->read_type==0) {
        LOAD_GLES(glGetIntegerv);
        gles_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES, (GLint *) &glstate->fbo.current_fb->read_format);
        gles_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE_OES, (GLint *) &glstate->fbo.current_fb->read_type);
    }
    int copytex = ((bound->format==GL_RGBA && bound->type==GL_UNSIGNED_BYTE) 
        || (bound->format==glstate->fbo.current_fb->read_format && bound->type==glstate->fbo.current_fb->read_type));

    if (copytex) {
        GLenum fmt;
        switch(internalformat) {
            case GL_ALPHA:
            case GL_ALPHA8:
                fmt = GL_ALPHA; break;
            case GL_LUMINANCE:
            case GL_LUMINANCE8:
                fmt = GL_LUMINANCE; break;
            case GL_LUMINANCE_ALPHA:
            case GL_LUMINANCE8_ALPHA8:
                fmt = GL_LUMINANCE_ALPHA; break;
            case GL_RGB:
            case 3:
                fmt = GL_RGB; break;
            default:
                fmt = GL_RGBA;
        }
        LOAD_GLES(glCopyTexImage2D);
        gles_glCopyTexImage2D(target, level, fmt, x, y, width, height, border);
    } else {
        void* tmp = malloc(width*height*4);
        gl4es_glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        gl4es_glTexImage2D(target, level, internalformat, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        free(tmp);
    }
    
    readfboEnd();
    // "Remap" if buffer mapped...
    glstate->vao->pack = pack;
    glstate->vao->unpack = unpack;
}

void APIENTRY_GL4ES gl4es_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                GLint x, GLint y, GLsizei width, GLsizei height) {
    const GLuint itarget = what_target(target);
    // WARNING: It seems glColorMask has an impact on what channel are actually copied by this. The crude glReadPixel / glTexSubImage cannot emulate that, and proper emulation will take need 2 read pixels.
    //  And using the real glCopyTexSubImage2D needs that the FrameBuffer were data are read is compatible with the Texture it's copied to...
    DBG(printf("glCopyTexSubImage2D(%s, %i, %i, %i, %i, %i, %i, %i), bounded texture=%u format/type=%s, %s\n", PrintEnum(target), level, xoffset, yoffset, x, y, width, height, (glstate->texture.bound[glstate->texture.active][itarget])?glstate->texture.bound[glstate->texture.active][itarget]->texture:0, PrintEnum((glstate->texture.bound[glstate->texture.active][itarget])?glstate->texture.bound[glstate->texture.active][itarget]->format:0), PrintEnum((glstate->texture.bound[glstate->texture.active][itarget])?glstate->texture.bound[glstate->texture.active][itarget]->type:0));)
    // PUSH_IF_COMPILING(glCopyTexSubImage2D);
    FLUSH_BEGINEND;

    if (globals4es.skiptexcopies) {
        DBG(printf("glCopyTexSubImage2D skipped.\n"));
        return;
    }
 
    LOAD_GLES(glCopyTexSubImage2D);
    errorGL();
    realize_bound(glstate->texture.active, target);
    
    // "Unmap" if buffer mapped...
    glbuffer_t *pack = glstate->vao->pack;
    glbuffer_t *unpack = glstate->vao->unpack;
    glstate->vao->pack = NULL;
    glstate->vao->unpack = NULL;

    readfboBegin(); // multiple readfboBegin() can be chained...

    gltexture_t* bound = glstate->texture.bound[glstate->texture.active][itarget];
#ifdef TEXSTREAM
    if (bound->streamed) {
        void* buff = GetStreamingBuffer(bound->streamingID);
        if ((bound->width == width) && (bound->height == height) && (xoffset == yoffset == 0)) {
            gl4es_glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, buff);
        } else {
            void* tmp = malloc(width*height*2);
            gl4es_glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, tmp);
            for (int y=0; y<height; y++) {
                memcpy(buff+((yoffset+y)*bound->width+xoffset)*2, tmp+y*width*2, width*2);
            }
            free(tmp);
        }
    } else 
#endif
    {
        int copytex = 0;
        if(glstate->fbo.current_fb->read_type==0) {
            LOAD_GLES(glGetIntegerv);
            gles_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES, (GLint *) &glstate->fbo.current_fb->read_format);
            gles_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE_OES, (GLint *) &glstate->fbo.current_fb->read_type);
        }
        copytex = ((bound->format==GL_RGBA && bound->type==GL_UNSIGNED_BYTE) 
            || (bound->format==glstate->fbo.current_fb->read_format && bound->type==glstate->fbo.current_fb->read_type));
        if (copytex || !glstate->colormask[0] || !glstate->colormask[1] || !glstate->colormask[2] || !glstate->colormask[3]) {
            gles_glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
            if(((((bound->max_level == level) && (level || bound->mipmap_need)) && (globals4es.automipmap!=3) && (bound->mipmap_need!=0))) && !(bound->max_level==bound->base_level && bound->base_level==0)) {
                LOAD_GLES2_OR_OES(glGenerateMipmap);
                if(gles_glGenerateMipmap)
                    gles_glGenerateMipmap(to_target(itarget));
            }
        } else {
            void* tmp = malloc(width*height*4);
            GLenum format = bound->format;
            GLenum type = bound->type;
            gl4es_glReadPixels(x, y, width, height, format, type, tmp);
            // mipmap will be calculated buy gl4es_glTexSubImage2D
            gl4es_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, tmp);
            free(tmp);
        }
    }
    readfboEnd();
    // "Remap" if buffer mapped...
    glstate->vao->pack = pack;
    glstate->vao->unpack = unpack;
}

void APIENTRY_GL4ES gl4es_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid * data) {
    DBG(printf("glReadPixels(%i, %i, %i, %i, %s, %s, 0x%p)\n", x, y, width, height, PrintEnum(format), PrintEnum(type), data);)
    FLUSH_BEGINEND;
    if (glstate->list.compiling && glstate->list.active) {
        errorShim(GL_INVALID_OPERATION);
        return;	// never in list
    }
    LOAD_GLES(glReadPixels);
    errorGL();
    GLvoid* dst = data;
    if (glstate->vao->pack)
        dst = (char*)dst + (uintptr_t)glstate->vao->pack->data;
        
    readfboBegin();
    if ((format == GL_RGBA && type == GL_UNSIGNED_BYTE)     // should not use default GL_RGBA on Pandora as it's very slow...
       || (format == glstate->readf && type == glstate->readt)    // use the IMPLEMENTATION_READ too...
       || (format == GL_DEPTH_COMPONENT && (type == GL_FLOAT || type==GL_HALF_FLOAT)))   // this one will probably fail, as DEPTH is not readable on most GLES hardware 
    {
        // easy passthru
        gles_glReadPixels(x, y, width, height, format, type, dst);
        readfboEnd();
        return;
    }
    // grab data in GL_RGBA format
    int use_bgra = 0;
    if(glstate->readf==GL_BGRA && glstate->readt==GL_UNSIGNED_BYTE)
        use_bgra = 1;   // if IMPLEMENTATION_READ is BGRA, then use it as it's probably faster then RGBA.
    GLvoid *pixels = malloc(width*height*4);
    gles_glReadPixels(x, y, width, height, use_bgra?GL_BGRA:GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (! pixel_convert(pixels, &dst, width, height,
                        use_bgra?GL_BGRA:GL_RGBA, GL_UNSIGNED_BYTE, format, type, 0,glstate->texture.pack_align)) {
        LOGE("ReadPixels error: (%s, UNSIGNED_BYTE -> %s, %s )\n",
            PrintEnum(use_bgra?GL_BGRA:GL_RGBA), PrintEnum(format), PrintEnum(type));
    }
    free(pixels);
    readfboEnd();
    return;
}


#ifdef GL4ES_IOS
#include "gl4es_ios_pixels.h"

/* spec 35 clarification 13: level 0 of a 2D (or rectangle) texture into dst (already offset into
 * a bound pack buffer). The texture is attached to a temporary framebuffer when GLES can render
 * to the format it is stored in and the framebuffer is complete; otherwise it is drawn into an
 * RGBA8 texture of the same size, as upstream does for the other formats. The pixels are read as
 * RGBA8, which GLES reads from any colour buffer, and converted on the CPU to format/type
 * (gl4es_ios_pixels.c, or gl4es's pixel_convert for a pair that doesn't know). A texture format
 * that still fails is logged once: "[opengl] glGetTexImage <internal format> failed: <reason>".
 * The GL error stays clear either way, as upstream leaves it. */
static void ios_get_tex_image(gltexture_t *bound, GLenum format, GLenum type, GLvoid *dst) {
    const int shrink = bound->shrink;
    const int width = bound->width, height = bound->height, nwidth = bound->nwidth, nheight = bound->nheight;
    const int w = width << shrink, h = height << shrink, y0 = (nheight - height) << shrink;
    const GLuint old_fbo = glstate->fbo.current_fb->id;
    gltexture_t *bound2d = glstate->texture.bound[glstate->texture.active][ENABLED_TEX2D];
    const GLuint old_2d = bound2d ? bound2d->texture : 0;
    const int pack_align = glstate->texture.pack_align;
    glbuffer_t *pack = glstate->vao->pack;
    GLfloat clear[4];
    GLenum status = 0;
    GLuint fbo, temptex = 0;
    unsigned char *rgba = NULL;
    char reason[96];

    reason[0] = 0;
    memcpy(clear, glstate->clear_color, sizeof(clear));
    gl4es_glGenFramebuffers(1, &fbo);
    gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, fbo);
    if (shrink == 0 && gl4es_ios_attachable(bound->format, bound->type, hardext.bgra8888)) {
        gl4es_glFramebufferTexture2D(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, bound->texture, 0);
        status = gl4es_glCheckFramebufferStatus(GL_FRAMEBUFFER_OES);
    }
    if (status != GL_FRAMEBUFFER_COMPLETE_OES) {
        // drawn into an RGBA8 texture (ALPHA, LUMINANCE, a shrunk texture, an incomplete attachment)
        gl4es_glGenTextures(1, &temptex);
        gl4es_glBindTexture(GL_TEXTURE_2D, temptex);
        gl4es_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, nwidth<<shrink, nheight<<shrink, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        gl4es_glFramebufferTexture2D(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, temptex, 0);
        gl4es_glBindTexture(GL_TEXTURE_2D, old_2d);
        status = gl4es_glCheckFramebufferStatus(GL_FRAMEBUFFER_OES);
        if (status == GL_FRAMEBUFFER_COMPLETE_OES) {
            gl4es_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            gl4es_glClear(GL_COLOR_BUFFER_BIT);
            gl4es_blitTexture(bound->glname, 0.f, 0.f, width, height, nwidth, nheight, 1.0f, 1.0f, nwidth<<shrink, nheight<<shrink, 0, 0, BLIT_OPAQUE);
            gl4es_glClearColor(clear[0], clear[1], clear[2], clear[3]);
        } else {
            snprintf(reason, sizeof(reason), "framebuffer incomplete (0x%04X)", status);
        }
    }
    if (!reason[0] && !(rgba = malloc((size_t)w * (size_t)h * 4)))
        snprintf(reason, sizeof(reason), "out of memory (%dx%d)", w, h);
    if (!reason[0]) {
        // read as RGBA8, rows packed, into our own memory (no pack buffer for this read)
        glstate->vao->pack = NULL;
        if (pack_align != 4) gl4es_glPixelStorei(GL_PACK_ALIGNMENT, 4);
        gl4es_glReadPixels(0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (pack_align != 4) gl4es_glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
        glstate->vao->pack = pack;
        if (!gl4es_ios_from_rgba8(rgba, w, h, format, type, pack_align, dst)) {
            GLvoid *out = dst;
            if (!pixel_convert(rgba, &out, w, h, GL_RGBA, GL_UNSIGNED_BYTE, format, type, 0, pack_align)) {
                int n = snprintf(reason, sizeof(reason), "no conversion from RGBA8 to %s", PrintEnum(format));
                if (n > 0 && (size_t)n < sizeof(reason))
                    snprintf(reason + n, sizeof(reason) - (size_t)n, "/%s", PrintEnum(type));
            }
        }
        free(rgba);
    }
    gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, old_fbo);
    gl4es_glDeleteFramebuffers(1, &fbo);
    if (temptex) gl4es_glDeleteTextures(1, &temptex);
    if (reason[0]) {
        GLenum internal = bound->orig_internal ? bound->orig_internal : bound->internalformat;
        if (gl4es_ios_first_time(internal))
            printf("[opengl] glGetTexImage %s failed: %s\n", PrintEnum(internal), reason);
    }
    noerrorShim();
}
#endif

void APIENTRY_GL4ES gl4es_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid * img) {
    DBG(printf("glGetTexImage(%s, %i, %s, %s, %p)\n", PrintEnum(target), level, PrintEnum(format), PrintEnum(type), img);)
    FLUSH_BEGINEND;
    const GLuint itarget = what_target(target);    

    realize_bound(glstate->texture.active, target);
       
    gltexture_t* bound = glstate->texture.bound[glstate->texture.active][itarget];
    int width = bound->width;
    int height = bound->height;
    int nwidth = bound->nwidth;
    int nheight = bound->nheight;
    int shrink = bound->shrink;
    if (level != 0) {
        //printf("STUBBED glGetTexImage with level=%i\n", level);
#ifdef GL4ES_IOS
        // spec 35 clarification 13: level 0 is read into tmp (our memory, so no pack buffer for
        // that read); the result goes to the pack buffer's offset below, once
        glbuffer_t *pack = glstate->vao->pack;
        glstate->vao->pack = NULL;
#endif
        void* tmp = malloc(width*height*pixel_sizeof(format, type)); // tmp space...
        void* tmp2;
        gl4es_glGetTexImage(map_tex_target(target), 0, format, type, tmp);
#ifdef GL4ES_IOS
        glstate->vao->pack = pack;
        if (pack)
            img = (char*)img + (uintptr_t)pack->data;
#endif
        for (int i=0; i<level; i++) {
            pixel_halfscale(tmp, &tmp2, width, height, format, type);
            free(tmp);
            tmp = tmp2;
            width = nlevel(width, 1);
            height = nlevel(height, 1);
        }
        memcpy(img, tmp, width*height*pixel_sizeof(format, type));
        free(tmp);
        return;
    }
    
#ifdef GL4ES_IOS
    // spec 35 clarification 13: a rectangle texture is a 2D texture in GLES (wined3d may use them)
    if (target!=GL_TEXTURE_2D && target!=GL_TEXTURE_RECTANGLE_ARB) {
#else
    if (target!=GL_TEXTURE_2D) {
#endif
        return;
    }

    DBG(printf("glGetTexImage(%s, %i, %s, %s, 0x%p), texture=0x%x, size=%i,%i\n", PrintEnum(target), level, PrintEnum(format), PrintEnum(type), img, bound->glname, width, height);)
    
    GLvoid *dst = img;
    if (glstate->vao->pack)
        dst = (char*)dst + (uintptr_t)glstate->vao->pack->data;
#ifdef TEXSTREAM
    if (globals4es.texstream && bound->streamed) {
        noerrorShim();
        pixel_convert(GetStreamingBuffer(bound->streamingID), &dst, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, format, type, 0, glstate->texture.unpack_align);
        readfboEnd();
        return;
    }
#endif
    if (globals4es.texcopydata && bound->data) {
        //printf("texcopydata* glGetTexImage(0x%04X, %d, 0x%04x, 0x%04X, %p)\n", target, level, format, type, img);
        noerrorShim();
        if (!pixel_convert(bound->data, &dst, width, height, GL_RGBA, GL_UNSIGNED_BYTE, format, type, 0, glstate->texture.pack_align))
            printf("LIBGL: Error on pixel_convert while glGetTexImage\n");
    } else {
#ifdef GL4ES_IOS
        ios_get_tex_image(bound, format, type, dst);
        return;
#endif
        // Setup an FBO the same size of the texture
        GLuint oldBind = bound->glname;
        GLuint old_fbo = glstate->fbo.current_fb->id;
        GLuint fbo;
    
        // if the texture is not RGBA or RGB or ALPHA, the "just attach texture to the fbo" trick will not work, and a full Blit has to be done
        if((bound->format==GL_RGBA || bound->format==GL_RGB || (bound->format==GL_BGRA && hardext.bgra8888) || bound->format==GL_ALPHA) && (shrink==0)) {
            gl4es_glGenFramebuffers(1, &fbo);
            gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, fbo);
            gl4es_glFramebufferTexture2D(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, oldBind, 0);
            // Read the pixels!
            gl4es_glReadPixels(0, nheight-height, width, height, format, type, img);	// using "full" version with conversion of format/type
            gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, old_fbo);
            gl4es_glDeleteFramebuffers(1, &fbo);
            noerrorShim();
        } else {
            gl4es_glGenFramebuffers(1, &fbo);
            gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, fbo);
            GLuint temptex;
            gl4es_glGenTextures(1, &temptex);
            gl4es_glBindTexture(GL_TEXTURE_2D, temptex);
            gl4es_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, nwidth<<shrink, nheight<<shrink, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            gl4es_glFramebufferTexture2D(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, temptex, 0);
            gl4es_glBindTexture(GL_TEXTURE_2D, oldBind);
            // blit the texture
            gl4es_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            gl4es_glClear(GL_COLOR_BUFFER_BIT);
            gl4es_blitTexture(oldBind, 0.f, 0.f, width, height, nwidth, nheight, 1.0f, 1.0f, nwidth<<shrink, nheight<<shrink, 0, 0, BLIT_OPAQUE);
            // Read the pixels!
            gl4es_glReadPixels(0, (nheight-height)<<shrink, width<<shrink, height<<shrink, format, type, img);	// using "full" version with conversion of format/type
            gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, old_fbo);
            gl4es_glDeleteFramebuffers(1, &fbo);
            gl4es_glDeleteTextures(1, &temptex);
            noerrorShim();
        }
    }
}

void APIENTRY_GL4ES gl4es_glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
            GLsizei width, GLint border) {
    gl4es_glCopyTexImage2D(GL_TEXTURE_1D, level, internalformat, x, y, width, 1, border);
            
}

void APIENTRY_GL4ES gl4es_glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y,
                                GLsizei width) {
    gl4es_glCopyTexSubImage2D(GL_TEXTURE_1D, level, xoffset, 0, x, y, width, 1);
}
                                
//Direct wrapper
AliasExport(void,glGetTexImage,,(GLenum target, GLint level, GLenum format, GLenum type, GLvoid * img));
AliasExport(void,glReadPixels,,(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid * data));
AliasExport(void,glCopyTexImage1D,,(GLenum target,  GLint level,  GLenum internalformat,  GLint x,  GLint y, GLsizei width,  GLint border));
AliasExport(void,glCopyTexImage2D,,(GLenum target,  GLint level,  GLenum internalformat,  GLint x,  GLint y, GLsizei width,  GLsizei height,  GLint border));
AliasExport(void,glCopyTexSubImage2D,,(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height));
AliasExport(void,glCopyTexSubImage1D,,(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width));

