// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 Part D5: presenting a GL drawable into its window's CAMetalLayer.
//
// Per window drawable, a ring of three BGRA8 Metal textures, each imported once into ANGLE as an
// EGLImage with a renderbuffer and an FBO in the current context. A swap copies the drawable's
// colour attachment into ring texture i inside ANGLE (saving and restoring exactly the GLES
// state it touches, so gl4es's cached state stays true), signals the drawable's MTLSharedEvent
// from ANGLE's command stream, and then encodes on pgl's own queue: wait for the event, draw one
// full-screen triangle sampling texture i (vertical axis flipped, alpha 1) into the layer's next
// drawable, present it with spec 05 E3's pacing and commit.
//
// The layer is the one DXMT uses for the window: it comes from winemetal's unix entries
// (CreateMetalViewFromHWND, ReleaseMetalView, MetalLayer_get/setProps, the display-sync entry),
// called through dxmt_winemetal_unix_call_funcs with the entry numbers of winemetal_thunks.h.
//
// Manual retain/release (built with -fno-objc-arc). Every swap runs in an autorelease pool.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <mach/mach_time.h>
#include <time.h>

#include "pgl_internal.h"
#include "gl4esinit.h"
#include "gl4es_ios.h"

#define WINEMETAL_API   /* declarations only: nothing of winemetal's PE half is called */
#include "winemetal_thunks.h"

/* spec 05: the unix half of winemetal (DXMT), linked into the app */
extern const void *dxmt_winemetal_unix_call_funcs[];
extern int pg_get_vsync_locked( void );
extern void pg_present_count_add( uint32_t n );

typedef int32_t (*unix_entry)( void *args );

static int32_t winemetal_call( unsigned int code, void *params )
{
    return ((unix_entry)dxmt_winemetal_unix_call_funcs[code])( params );
}

/* ---------------------------------------------------------------- device, queue, pipeline */

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipeline;
static int pipeline_failed;
static pthread_once_t pipeline_once = PTHREAD_ONCE_INIT;

int pgl_metal_init( EGLDeviceEXT egl_device, char *name, size_t name_size, const char **why )
{
    PFNEGLQUERYDEVICEATTRIBEXTPROC query = (PFNEGLQUERYDEVICEATTRIBEXTPROC)eglGetProcAddress( "eglQueryDeviceAttribEXT" );
    EGLAttrib value = 0;

    @autoreleasepool
    {
        if (!query || !query( egl_device, EGL_METAL_DEVICE_ANGLE, &value ) || !value)
        {
            *why = "ANGLE reports no MTLDevice";
            return 1;
        }
        device = [(id<MTLDevice>)(uintptr_t)value retain];
        if (!(queue = [device newCommandQueue]))
        {
            *why = "no Metal command queue";
            return 1;
        }
        snprintf( name, name_size, "%s", device.name ? device.name.UTF8String : "Apple GPU" );
    }
    return 0;
}

/* a few lines of Metal shading language, compiled once through the public Metal API (compiling
 * GPU code at run time is not CPU code generation, spec 00 A) */
static const char shader_source[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct pgl_v2f { float4 pos [[position]]; float2 uv; };\n"
    "vertex pgl_v2f pgl_present_vs( uint vid [[vertex_id]] )\n"
    "{\n"
    "    float2 p = float2( (vid << 1) & 2, vid & 2 );\n"
    "    pgl_v2f out;\n"
    "    out.pos = float4( p * 2.0 - 1.0, 0.0, 1.0 );\n"
    /* GL's origin is bottom-left: the bottom of the layer shows row 0 of the texture ANGLE
     * rendered, so v is not inverted here as a plain Metal blit would invert it */
    "    out.uv = p;\n"
    "    return out;\n"
    "}\n"
    "fragment float4 pgl_present_fs( pgl_v2f in [[stage_in]], texture2d<float> tex [[texture(0)]] )\n"
    "{\n"
    "    constexpr sampler s( coord::normalized, filter::nearest, address::clamp_to_edge );\n"
    "    return float4( tex.sample( s, in.uv ).rgb, 1.0 );\n"
    "}\n";

static void make_pipeline( void )
{
    @autoreleasepool
    {
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(shader_source) options:nil error:&error];
        MTLRenderPipelineDescriptor *desc;
        id<MTLFunction> vs, fs;

        if (!library)
        {
            pgl_log( "[opengl] pgl: present shader: %s", error ? error.localizedDescription.UTF8String : "?" );
            pipeline_failed = 1;
            return;
        }
        vs = [library newFunctionWithName:@"pgl_present_vs"];
        fs = [library newFunctionWithName:@"pgl_present_fs"];
        desc = [[MTLRenderPipelineDescriptor alloc] init];   /* Metal's defaults */
        desc.vertexFunction = vs;
        desc.fragmentFunction = fs;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!pipeline)
        {
            pgl_log( "[opengl] pgl: present pipeline: %s", error ? error.localizedDescription.UTF8String : "?" );
            pipeline_failed = 1;
        }
        [desc release];
        [vs release];
        [fs release];
        [library release];
    }
}

/* ---------------------------------------------------------------- pacing (spec 05 E3) */

static uint64_t now_ns( void )
{
    static mach_timebase_info_data_t timebase;
    if (!timebase.denom) mach_timebase_info( &timebase );
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

static _Atomic uint64_t last_acquire_ns;   /* the last real nextDrawable */
static _Atomic uint64_t acquires;

static int limiter_enabled( void )
{
    static int cached = -1;
    if (cached < 0)
    {
        const char *v = getenv( "DXMT_PRESENT_LIMITER" );
        cached = v && *v && strcmp( v, "0" );
    }
    return cached;
}

static void limiter_wait( double period_seconds )
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t deadline;
    uint64_t period = (uint64_t)(period_seconds * 1e9), now = now_ns(), next;
    struct timespec ts;

    pthread_mutex_lock( &lock );
    next = deadline = pgl_limiter_next( deadline, now, period );
    pthread_mutex_unlock( &lock );
    if (next <= now) return;   /* a late frame doesn't sleep */
    ts.tv_sec = (time_t)((next - now) / 1000000000u);
    ts.tv_nsec = (long)((next - now) % 1000000000u);
    nanosleep( &ts, NULL );
}

/* ---------------------------------------------------------------- the present state */

struct pgl_present
{
    _Atomic int refs;                      /* the drawable's + one per Metal pass in flight */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int busy[PGL_RING_SIZE];               /* a Metal pass reading texture i hasn't completed */

    /* the rest only under the drawable's lock */
    id<MTLTexture> tex[PGL_RING_SIZE];
    EGLImageKHR image[PGL_RING_SIZE];
    int ring_width, ring_height;
    pgl_context *ring_context;             /* where rb[] and fbo[] live */
    GLuint rb[PGL_RING_SIZE], fbo[PGL_RING_SIZE];
    unsigned int slot;
    int last_slot;                         /* the slot the last swap copied into, -1: none */
    id<MTLSharedEvent> event;
    uint64_t event_value;
    obj_handle_t view, layer;
    int layer_width, layer_height;
    int display_sync;                      /* -1: not set yet */
    int logged_wait, logged_sync, logged_other;
};

static struct pgl_present *present_create( void )
{
    struct pgl_present *p = calloc( 1, sizeof(*p) );

    if (!p) return NULL;
    atomic_init( &p->refs, 1 );
    pthread_mutex_init( &p->lock, NULL );
    pthread_cond_init( &p->cond, NULL );
    p->display_sync = -1;
    p->last_slot = -1;
    p->event = [device newSharedEvent];
    return p;
}

static void present_release( struct pgl_present *p )
{
    if (atomic_fetch_sub( &p->refs, 1 ) != 1) return;
    pthread_cond_destroy( &p->cond );
    pthread_mutex_destroy( &p->lock );
    free( p );
}

/* GLES state the copy touches, saved and restored exactly (D5.3.2) */
struct saved_gles
{
    GLint draw_fbo, read_fbo, renderbuffer;
    GLboolean scissor;
    GLboolean color_mask[4];
};

static void save_gles( struct saved_gles *s )
{
    glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING_ANGLE, &s->draw_fbo );
    glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING_ANGLE, &s->read_fbo );
    glGetIntegerv( GL_RENDERBUFFER_BINDING, &s->renderbuffer );
    s->scissor = glIsEnabled( GL_SCISSOR_TEST );
    glGetBooleanv( GL_COLOR_WRITEMASK, s->color_mask );
}

static void restore_gles( const struct saved_gles *s )
{
    glBindFramebuffer( GL_READ_FRAMEBUFFER_ANGLE, (GLuint)s->read_fbo );
    glBindFramebuffer( GL_DRAW_FRAMEBUFFER_ANGLE, (GLuint)s->draw_fbo );
    glBindRenderbuffer( GL_RENDERBUFFER, (GLuint)s->renderbuffer );
    if (s->scissor) glEnable( GL_SCISSOR_TEST );
    glColorMask( s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3] );
}

/* the per-context renderbuffers and FBOs of the ring; their context current */
static void ring_gl_delete( struct pgl_present *p )
{
    unsigned int i;

    if (!p->ring_context) return;
    if (p->ring_context == pgl_thread_context())
    {
        glDeleteFramebuffers( PGL_RING_SIZE, p->fbo );
        glDeleteRenderbuffers( PGL_RING_SIZE, p->rb );
    }
    for (i = 0; i < PGL_RING_SIZE; i++) p->fbo[i] = p->rb[i] = 0;
    p->ring_context = NULL;
}

static void ring_metal_delete( struct pgl_present *p )
{
    unsigned int i;

    for (i = 0; i < PGL_RING_SIZE; i++)
    {
        if (p->image[i] != EGL_NO_IMAGE_KHR) pgl_eglDestroyImageKHR( pgl_display, p->image[i] );
        p->image[i] = EGL_NO_IMAGE_KHR;
        [p->tex[i] release];
        p->tex[i] = nil;
    }
    p->ring_width = p->ring_height = 0;
}

/* D5.2: three textures of the drawable's size, each an EGLImage; recreated on a size change */
static int ring_ensure( pgl_drawable *d, struct pgl_present *p )
{
    MTLTextureDescriptor *desc;
    unsigned int i;

    if (p->tex[0] && p->ring_width == d->att_width && p->ring_height == d->att_height) return 1;

    ring_gl_delete( p );
    ring_metal_delete( p );
    desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:(NSUInteger)d->att_width
                                                             height:(NSUInteger)d->att_height
                                                          mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;
    for (i = 0; i < PGL_RING_SIZE; i++)
    {
        if (!(p->tex[i] = [device newTextureWithDescriptor:desc])) break;
        p->image[i] = EGL_NO_IMAGE_KHR;
#ifdef EGL_TEXTURE_INTERNAL_FORMAT_ANGLE
        {
            /* the internal format as D5.2 gives it; without the attribute when ANGLE refuses it */
            static const EGLint with_format[] = { EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, GL_BGRA_EXT, EGL_NONE };
            p->image[i] = pgl_eglCreateImageKHR( pgl_display, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
                                                 (EGLClientBuffer)p->tex[i], with_format );
        }
#endif
        if (p->image[i] == EGL_NO_IMAGE_KHR)
            p->image[i] = pgl_eglCreateImageKHR( pgl_display, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
                                                 (EGLClientBuffer)p->tex[i], NULL );
        if (p->image[i] == EGL_NO_IMAGE_KHR) break;
    }
    if (i < PGL_RING_SIZE)
    {
        static _Atomic uint64_t fails;
        pgl_log_rare( &fails, "[opengl] pgl: present ring %dx%d failed (EGL error %#x)",
                      d->att_width, d->att_height, eglGetError() );
        ring_metal_delete( p );
        return 0;
    }
    p->ring_width = d->att_width;
    p->ring_height = d->att_height;
    return 1;
}

static int ring_gl_ensure( struct pgl_present *p )
{
    pgl_context *ctx = pgl_thread_context();
    struct saved_gles saved;
    unsigned int i;

    if (p->ring_context == ctx && p->fbo[0]) return 1;
    ring_gl_delete( p );

    save_gles( &saved );
    glGenRenderbuffers( PGL_RING_SIZE, p->rb );
    glGenFramebuffers( PGL_RING_SIZE, p->fbo );
    for (i = 0; i < PGL_RING_SIZE; i++)
    {
        glBindRenderbuffer( GL_RENDERBUFFER, p->rb[i] );
        pgl_glEGLImageTargetRenderbufferStorageOES( GL_RENDERBUFFER, (GLeglImageOES)p->image[i] );
        glBindFramebuffer( GL_DRAW_FRAMEBUFFER_ANGLE, p->fbo[i] );
        glFramebufferRenderbuffer( GL_DRAW_FRAMEBUFFER_ANGLE, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, p->rb[i] );
    }
    restore_gles( &saved );
    p->ring_context = ctx;
    return 1;
}

/* D5.3.1: wait at most 100 ms for the Metal pass that last read texture i */
static void wait_slot( struct pgl_present *p, unsigned int i )
{
    struct timespec deadline;
    int waited = 0;

    pthread_mutex_lock( &p->lock );
    if (p->busy[i])
    {
        clock_gettime( CLOCK_REALTIME, &deadline );
        deadline.tv_nsec += 100 * 1000 * 1000;
        if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
        waited = 1;
        while (p->busy[i])
            if (pthread_cond_timedwait( &p->cond, &p->lock, &deadline )) break;
    }
    pthread_mutex_unlock( &p->lock );
    if (waited && !p->logged_wait)
    {
        p->logged_wait = 1;
        pgl_log( "[opengl] pgl: a swap waited for the GPU to finish reading a present texture" );
    }
}

/* gl4es holds immediate-mode drawing back (glBegin/glEnd blocks it merges, glRect, glBitmap) and
 * draws it at its next state change or flush. The copy below reads the framebuffer through ANGLE,
 * past gl4es, so gl4es's own glFlush runs first: it draws what is pending, then flushes GLES. A
 * frame that ends with glEnd() was otherwise copied without its last drawing (the render test's
 * orientation and immediate-mode scenes read black). */
static void (*gl4es_flush)( void );
static pthread_once_t gl4es_flush_once = PTHREAD_ONCE_INIT;

static void load_gl4es_flush( void )
{
    gl4es_flush = (void (*)( void ))gl4es_GetProcAddress( "glFlush" );
    if (!gl4es_flush) pgl_log( "[opengl] pgl: gl4es has no glFlush; immediate-mode drawing may miss a frame" );
}

/* D5.3.2 and D5.3.3: copy the colour attachment into texture i and signal value n */
static void copy_and_signal( pgl_drawable *d, struct pgl_present *p, unsigned int i, uint64_t n )
{
    GLuint src = gl4es_ios_gles_framebuffer( d->fbo );
    struct saved_gles saved;
    EGLAttrib attribs[7];
    EGLSync sync;

    pthread_once( &gl4es_flush_once, load_gl4es_flush );
    if (gl4es_flush) gl4es_flush();
    save_gles( &saved );
    glBindFramebuffer( GL_READ_FRAMEBUFFER_ANGLE, src );
    glBindFramebuffer( GL_DRAW_FRAMEBUFFER_ANGLE, p->fbo[i] );
    if (saved.scissor) glDisable( GL_SCISSOR_TEST );
    glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
    if (pgl_es_major >= 3)
        pgl_glBlitFramebuffer( 0, 0, d->att_width, d->att_height, 0, 0, d->att_width, d->att_height,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST );
    else
        pgl_glBlitFramebufferANGLE( 0, 0, d->att_width, d->att_height, 0, 0, d->att_width, d->att_height,
                                    GL_COLOR_BUFFER_BIT, GL_NEAREST );
    restore_gles( &saved );

    attribs[0] = EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE;
    attribs[1] = (EGLAttrib)p->event;
    attribs[2] = EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE;
    attribs[3] = (EGLAttrib)(n & 0xffffffffu);
    attribs[4] = EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE;
    attribs[5] = (EGLAttrib)(n >> 32);
    attribs[6] = EGL_NONE;
    sync = eglCreateSync( pgl_display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, attribs );
    if (sync != EGL_NO_SYNC)
    {
        glFlush();
        eglDestroySync( pgl_display, sync );
    }
    else
    {
        /* no GPU signal: finish the copy here and signal from the CPU */
        if (!p->logged_sync)
        {
            p->logged_sync = 1;
            pgl_log( "[opengl] pgl: EGL_SYNC_METAL_SHARED_EVENT_ANGLE failed (EGL error %#x), finishing on the CPU",
                     eglGetError() );
        }
        glFinish();
        p->event.signaledValue = n;
    }
}

/* D5.1: the window's layer, asked for at the first present; its properties as D5.4 says */
static int layer_ensure( pgl_drawable *d, struct pgl_present *p )
{
    struct unixcall_generic_obj_constptr_noret set;
    struct unixcall_generic_obj_ptr_noret get;
    struct WMTLayerProps props;

    if (!p->view)
    {
        struct unixcall_create_metal_view_from_hwnd params;

        memset( &params, 0, sizeof(params) );
        params.hwnd = d->hwnd;
        params.device = (obj_handle_t)(uintptr_t)device;
        winemetal_call( unix_create_metal_view_from_hwnd, &params );
        if (!params.ret_view || !params.ret_layer)
        {
            static _Atomic uint64_t none;
            pgl_log_rare( &none, "[opengl] pgl: no Metal layer for window %#llx", (unsigned long long)d->hwnd );
            return 0;
        }
        p->view = params.ret_view;
        p->layer = params.ret_layer;
        p->layer_width = p->layer_height = 0;
        p->display_sync = -1;
    }

    if (p->layer_width != d->att_width || p->layer_height != d->att_height)
    {
        memset( &props, 0, sizeof(props) );
        get.handle = p->layer;
        WMT_MEMPTR_SET( get.arg, &props );
        winemetal_call( unix_metal_layer_get_props, &get );
        /* D5.6: a size we didn't set means another presenter (Direct3D) used the layer */
        if (p->layer_width && !p->logged_other &&
            ((int)props.drawable_width != p->layer_width || (int)props.drawable_height != p->layer_height))
        {
            p->logged_other = 1;
            pgl_log( "[opengl] pgl: window %#llx is also presented by another API; the last present wins",
                     (unsigned long long)d->hwnd );
        }
        props.device = (obj_handle_t)(uintptr_t)device;
        props.drawable_width = d->att_width;   /* Windows pixels (spec 05 E2.5) */
        props.drawable_height = d->att_height;
        props.framebuffer_only = true;
        props.opaque = true;
        props.pixel_format = WMTPixelFormatBGRA8Unorm;
        set.handle = p->layer;
        WMT_MEMPTR_SET( set.arg, &props );
        winemetal_call( unix_metal_layer_set_props, &set );
        p->layer_width = d->att_width;
        p->layer_height = d->att_height;
    }

    /* D5.4: displaySyncEnabled follows the swap interval, sent only when it changes */
    if (p->display_sync != (d->interval != 0))
    {
        struct unixcall_dxmt_ios_layer_display_sync sync;
        sync.layer = p->layer;
        sync.enabled = d->interval != 0;
        winemetal_call( unix_dxmt_ios_layer_display_sync, &sync );
        p->display_sync = d->interval != 0;
    }
    return 1;
}

/* the next drawable as spec 05 E3 takes it: RAW mode's 18 ms rule and the timing log */
static id<CAMetalDrawable> next_drawable( struct pgl_present *p, int mode )
{
    static _Atomic uint64_t slow;
    id<CAMetalDrawable> drawable;
    uint64_t t0, t1, n;
    double ms;

    if (pgl_pace_raw_skip( mode, now_ns(), atomic_load( &last_acquire_ns ) )) return nil;
    t0 = now_ns();
    drawable = [(CAMetalLayer *)(uintptr_t)p->layer nextDrawable];
    t1 = now_ns();
    atomic_store( &last_acquire_ns, t1 );
    ms = (double)(t1 - t0) / 1e6;
    n = atomic_fetch_add( &acquires, 1 );
    if (pgl_acquire_logged( n, ms ))
    {
        if (n < 8) pgl_log( "[opengl] nextDrawable #%llu took %.2f ms%s", (unsigned long long)(n + 1), ms, drawable ? "" : " (nil)" );
        else pgl_log_rare( &slow, "[opengl] nextDrawable blocked %.1f ms%s", ms, drawable ? "" : " (nil)" );
    }
    return drawable;
}

/* D5.3.4: the Metal pass */
static void metal_pass( pgl_drawable *d, struct pgl_present *p, unsigned int i, uint64_t n )
{
    int mode = pg_get_vsync_locked();
    double period = pgl_pace_period( mode );
    id<CAMetalDrawable> drawable;
    id<MTLCommandBuffer> cb;
    id<MTLRenderCommandEncoder> enc;
    MTLRenderPassDescriptor *rp;

    if (!layer_ensure( d, p ) || !(drawable = next_drawable( p, mode )))
    {
        pg_present_count_add( 1 );   /* no drawable: the frame counts as presented */
        return;
    }

    cb = [queue commandBuffer];
    [cb encodeWaitForEvent:p->event value:n];
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pipeline];
    [enc setFragmentTexture:p->tex[i] atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];

    if (period > 0 && !limiter_enabled())
        [cb presentDrawable:drawable afterMinimumDuration:period];
    else
    {
        if (period > 0) limiter_wait( period );
        [cb presentDrawable:drawable];
    }

    pthread_mutex_lock( &p->lock );
    p->busy[i] = 1;
    pthread_mutex_unlock( &p->lock );
    atomic_fetch_add( &p->refs, 1 );
    [cb addCompletedHandler:^( id<MTLCommandBuffer> done ) {
        (void)done;
        pthread_mutex_lock( &p->lock );
        p->busy[i] = 0;
        pthread_cond_broadcast( &p->cond );
        pthread_mutex_unlock( &p->lock );
        present_release( p );
    }];
    [cb commit];
    pg_present_count_add( 1 );
}

/* D5.3 steps 1-4; returns 1 when the Metal pass ran (it counts the frame itself) */
static int swap( pgl_drawable *d, int visible )
{
    struct pgl_present *p;
    unsigned int i;
    uint64_t n;

    pthread_once( &pipeline_once, make_pipeline );
    if (!d->present) d->present = present_create();
    if (!(p = d->present) || !p->event || pipeline_failed) return 0;
    if (!ring_ensure( d, p ) || !ring_gl_ensure( p )) return 0;

    i = p->slot;
    p->slot = pgl_ring_next( i );
    wait_slot( p, i );
    n = ++p->event_value;
    copy_and_signal( d, p, i, n );
    p->last_slot = (int)i;
    if (!visible) return 0;
    metal_pass( d, p, i, n );
    return 1;
}

int pgl_present_swap( pgl_drawable *d, int visible )
{
    int counted;

    /* D5.3.5: every Metal and ANGLE call of a swap inside a pool drained before it returns */
    @autoreleasepool
    {
        counted = swap( d, visible );
    }
    /* no Metal pass (a hidden window, no ring): the frame counts as presented */
    if (!counted) pg_present_count_add( 1 );
    return 1;
}

void *pgl_drawable_last_copy( pgl_drawable *d, void **event, uint64_t *value )
{
    struct pgl_present *p;
    void *tex = NULL;

    pthread_mutex_lock( &d->lock );
    if ((p = d->present) && p->last_slot >= 0)
    {
        tex = p->tex[p->last_slot];
        *event = p->event;
        *value = p->event_value;
    }
    pthread_mutex_unlock( &d->lock );
    return tex;
}

void pgl_present_detach( pgl_drawable *d )
{
    struct pgl_present *p = d->present;
    if (p) ring_gl_delete( p );
}

void pgl_present_stop( pgl_drawable *d )
{
    struct pgl_present *p = d->present;
    struct unixcall_generic_obj_noret params;

    if (!p || !p->view) return;
    params.handle = p->view;
    winemetal_call( unix_release_metal_view, &params );
    p->view = p->layer = 0;
    p->layer_width = p->layer_height = 0;
    p->display_sync = -1;
}

void pgl_present_destroy( pgl_drawable *d )
{
    struct pgl_present *p = d->present;

    if (!p) return;
    @autoreleasepool
    {
        pgl_present_stop( d );
        ring_gl_delete( p );
        ring_metal_delete( p );
        [p->event release];
        p->event = nil;
    }
    d->present = NULL;
    present_release( p );
}
