/*
 * Nuklear - v1.17 - public domain
 * no warrenty implied; use at your own risk.
 * authored from 2015-2016 by Micha Mettke
 */
/*
 * ==============================================================
 *
 *                              API
 *
 * ===============================================================
 */
#ifndef NK_XLIB_GL_H_
#define NK_XLIB_GL_H_

#include <X11/Xlib.h>

NK_API struct nk_context*   nk_x11_init(Display *dpy, Window win);
NK_API void                 nk_x11_font_stash_begin(struct nk_font_atlas **atlas);
NK_API void                 nk_x11_font_stash_end(void);
NK_API int                  nk_x11_handle_event(XEvent *evt);
NK_API void                 nk_x11_render(enum nk_anti_aliasing);
NK_API void                 nk_x11_shutdown(void);

#endif
/*
 * ==============================================================
 *
 *                          IMPLEMENTATION
 *
 * ===============================================================
 */
#ifdef NK_XLIB_GL_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include <unistd.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xlocale.h>

#include <GL/gl.h>

#ifndef NK_X11_DOUBLE_CLICK_LO
#define NK_X11_DOUBLE_CLICK_LO 20
#endif
#ifndef NK_X11_DOUBLE_CLICK_HI
#define NK_X11_DOUBLE_CLICK_HI 200
#endif


#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#endif

struct nk_x11_vertex {
    GLfloat position[2];
    GLfloat uv[2];
    nk_byte col[4];
};

struct nk_x11_device {
    struct nk_buffer cmds;
    struct nk_draw_null_texture null;
    GLuint font_tex;
};

static struct nk_x11 {
    struct nk_x11_device ogl;
    struct nk_context ctx;
    struct nk_font_atlas atlas;
    Cursor cursor;
    Display *dpy;
    Window win;
    long last_button_click;
} x11;

NK_INTERN long
nk_timestamp(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0) return 0;
    return (long)((long)tv.tv_sec * 1000 + (long)tv.tv_usec/1000);
}

NK_INTERN void*
nk_malloc(nk_handle unused, void *old,nk_size size)
{
    NK_UNUSED(unused);
    NK_UNUSED(old);
    return malloc(size);
}
NK_INTERN void
nk_mfree(nk_handle unused, void *ptr)
{
    NK_UNUSED(unused);
    free(ptr);
}

NK_INTERN void
nk_x11_device_upload_atlas(const void *image, int width, int height)
{
    struct nk_x11_device *dev = &x11.ogl;
    glGenTextures(1, &dev->font_tex);
    glBindTexture(GL_TEXTURE_2D, dev->font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, image);
}

NK_INTERN void
nk_x11_font_query_font_glyph(struct nk_font *font, float height,
    struct nk_user_font_glyph *glyph, nk_rune codepoint,
    nk_rune next_codepoint)
{
    float scale;
    const struct nk_font_glyph *g;
    NK_ASSERT(glyph);
    NK_UNUSED(next_codepoint);

    NK_ASSERT(font);
    NK_ASSERT(font->glyphs);
    if (!font || !glyph)
        return;

    scale = height/font->info.height;
    g = nk_font_find_glyph(font, codepoint);
    glyph->width = (g->x1 - g->x0) * scale;
    glyph->height = (g->y1 - g->y0) * scale;
    glyph->offset = nk_vec2(g->x0 * scale, g->y0 * scale);
    glyph->xadvance = (g->xadvance * scale);
    glyph->uv[0] = nk_vec2(g->u0, g->v0);
    glyph->uv[1] = nk_vec2(g->u1, g->v1);
}

NK_INTERN void
nk_x11_draw_rect_uv(nk_uint id, struct nk_vec2 uv0, struct nk_vec2 uv1, 
    short x, short y, unsigned short w, unsigned short h,
    struct nk_color color)
{
	glBindTexture(GL_TEXTURE_2D, id);

    glColor4ub(color.r,color.g,color.b,color.a);
    glBegin(GL_TRIANGLES);
        glTexCoord2f(uv0.x, uv0.y); glVertex2f(x,     y);
        glTexCoord2f(uv0.x, uv1.y); glVertex2f(x,     y+ h);
        glTexCoord2f(uv1.x, uv0.y); glVertex2f(x + w, y);
        glTexCoord2f(uv1.x, uv0.y); glVertex2f(x + w, y);
        glTexCoord2f(uv0.x, uv1.y); glVertex2f(x,     y+ h);
        glTexCoord2f(uv1.x, uv1.y); glVertex2f(x + w, y+ h);
    glEnd();

	glBindTexture(GL_TEXTURE_2D, 0);
}

NK_INTERN void
nk_x11_draw_text(const struct nk_x11_device *dev,
    struct nk_font *font, struct nk_rect rect,
    const char *text, int len, float font_height,
    struct nk_color fg)
{
    float x = 0;
    int text_len = 0;
    nk_rune unicode = 0;
    nk_rune next = 0;
    int glyph_len = 0;
    int next_glyph_len = 0;
    struct nk_user_font_glyph g;
    if (!len || !text) return;

    x = rect.x;
    glyph_len = nk_utf_decode(text, &unicode, len);
    if (!glyph_len) return;

    /* draw every glyph image */
    while (text_len < len && glyph_len) {
        float gx, gy, gh, gw;
        float char_width = 0;
        if (unicode == NK_UTF_INVALID) break;

        /* query currently drawn glyph information */
        next_glyph_len = nk_utf_decode(text + text_len + glyph_len, &next, (int)len - text_len);
        nk_x11_font_query_font_glyph(font, font_height, &g, unicode,
                    (next == NK_UTF_INVALID) ? '\0' : next);

        /* calculate and draw glyph drawing rectangle and image */
        gx = x + g.offset.x;
        gy = rect.y + g.offset.y;
        gw = g.width; gh = g.height;
        char_width = g.xadvance;
        
		nk_x11_draw_rect_uv(dev->font_tex,g.uv[0],g.uv[1], gx, gy, gw, gh, fg);

        /* offset next glyph */
        text_len += glyph_len;
        x += char_width;
        glyph_len = next_glyph_len;
        unicode = next;
    }

}

NK_INTERN void
nk_x11_stroke_line(const struct nk_x11_device *dev, 
    short x0, short y0, short x1, short y1, 
	unsigned int line_thickness, struct nk_color col)
{
    glLineWidth(line_thickness);
    glColor4ub(col.r, col.g, col.b, col.a);
    glBegin(GL_LINE_STRIP);
        glVertex2f(x0, y0);
        glVertex2f(x1, y1);
    glEnd();
}

NK_INTERN void
nk_x11_stroke_curve(const struct nk_x11_device *dev, 
    struct nk_vec2i p1, struct nk_vec2i p2, struct nk_vec2i p3, struct nk_vec2i p4,
    unsigned int num_segments, unsigned short line_thickness, struct nk_color col)
{
    unsigned int i_step, segments;
    float t_step;
    struct nk_vec2i last = p1;

    segments = MAX(num_segments, 1);
    t_step = 1.0f/(float)segments;
    for (i_step = 1; i_step <= segments; ++i_step) {
        float t = t_step * (float)i_step;
        float u = 1.0f - t;
        float w1 = u*u*u;
        float w2 = 3*u*u*t;
        float w3 = 3*u*t*t;
        float w4 = t * t *t;
        float x = w1 * p1.x + w2 * p2.x + w3 * p3.x + w4 * p4.x;
        float y = w1 * p1.y + w2 * p2.y + w3 * p3.y + w4 * p4.y;
        nk_x11_stroke_line(dev, last.x, last.y,
                (short)x, (short)y, line_thickness,col);
        last.x = (short)x; last.y = (short)y;
    }
}

NK_INTERN void
nk_x11_draw_image(const struct nk_x11_device *dev,
    short x, short y, unsigned short w, unsigned short h,
    struct nk_image img, struct nk_color col)
{
	struct nk_vec2 uv[2];
    uv[0].x = (float)img.region[0]/(float)img.w;
    uv[0].y = (float)img.region[1]/(float)img.h;
    uv[1].x = (float)(img.region[0] + img.region[2])/(float)img.w;
    uv[1].y = (float)(img.region[1] + img.region[3])/(float)img.h;

    nk_x11_draw_rect_uv(img.handle.id,uv[0],uv[1], x, y, w, h, col);
}

NK_INTERN void
nk_x11_stroke_arc(const struct nk_x11_device *dev, 
    short x0, short y0, float radius, float a_min, float a_max, 
    unsigned int segments, short line_thickness, struct nk_color col)
{
    unsigned int i = 0;
    if (radius == 0.0f) return;

    const float d_angle = (a_max - a_min) / (float)segments;
    const float sin_d = sinf(d_angle);
    const float cos_d = cosf(d_angle);

    float cx = cosf(a_min) * radius;
    float cy = sinf(a_min) * radius;

    glLineWidth(line_thickness);
    glColor4ub(col.r, col.g, col.b, col.a);
    glBegin(GL_LINE_STRIP);
    for(i = 0; i <= segments; ++i) {
        float new_cx, new_cy;

        glVertex2f(x0 + cx, y0 + cy);

        new_cx = cx * cos_d - cy * sin_d;
        new_cy = cy * cos_d + cx * sin_d;
        cx = new_cx;
        cy = new_cy;
    }
    glEnd();
}

NK_INTERN void
nk_x11_stroke_circle(const struct nk_x11_device *dev,
    short x, short y, short w, short h, 
    unsigned int num_segments, short line_thickness, struct nk_color col)
{
    float cx = x + (w / 2.0f);
    float cy = y + (h / 2.0f);
    float radius = MIN(w,h) / 2.0f;
    float twicePi = 2.0f * 3.141592654f;

    nk_x11_stroke_arc(dev, cx, cy, radius, 0.0f, twicePi, num_segments, 1, col);
}

NK_INTERN void
nk_x11_fill_arc(const struct nk_x11_device *dev, 
    short x0, short y0, float radius, float a_min, float a_max, 
    unsigned int segments, struct nk_color col)
{
    unsigned int i = 0;
    if (radius == 0.0f) return;

    const float d_angle = (a_max - a_min) / (float)segments;
    const float sin_d = sinf(d_angle);
    const float cos_d = cosf(d_angle);

    float cx = cosf(a_min) * radius;
    float cy = sinf(a_min) * radius;

    glColor4ub(col.r, col.g, col.b, col.a);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(x0, y0);
    for(i = 0; i <= segments; ++i) {
        float new_cx, new_cy;

        glVertex2f(x0 + cx, y0 + cy);

        new_cx = cx * cos_d - cy * sin_d;
        new_cy = cy * cos_d + cx * sin_d;
        cx = new_cx;
        cy = new_cy;
    }
    glEnd();
}

NK_INTERN void
nk_x11_fill_circle(const struct nk_x11_device *dev,
    short x, short y, short w, short h, 
    unsigned int num_segments, struct nk_color col)
{
    float cx = x + (w / 2.0f);
    float cy = y + (h / 2.0f);
    float radius = MIN(w,h) / 2.0f;
    float twicePi = 2.0f * 3.141592654f;

    nk_x11_fill_arc(dev, cx, cy, radius, 0.0f, twicePi, num_segments, col);
}

NK_INTERN void
nk_x11_stroke_rect(const struct nk_x11_device *dev,
    short x, short y, short w, short h,
    short r, short line_thickness, struct nk_color col)
{
    if (r == 0)
    {
        glLineWidth(line_thickness);
        glColor4ub(col.r, col.g, col.b, col.a);
        glBegin(GL_LINE_LOOP);
            glVertex2f(x,   y);
            glVertex2f(x,   y + h);
            glVertex2f(x+w, y + h);
            glVertex2f(x+w, y);
        glEnd();
    }
    else
    {
        const short xc = x + r - 1;
        const short yc = y + r;
        const short wc = (short)(w - (2 * r)) + 1;
        const short hc = (short)(h - (2 * r)) + 1;

        nk_x11_stroke_line(dev, xc    , y    , xc + wc, y      , line_thickness, col);
        nk_x11_stroke_line(dev, x + w , yc   , x + w  , yc + hc, line_thickness, col);
        nk_x11_stroke_line(dev, xc    , y + h, xc + wc, y + h  , line_thickness, col);
        nk_x11_stroke_line(dev, x     , yc   , x      , yc + hc, line_thickness, col);

        float angle = 2.0f * 3.141592654f / 4.0f;

        nk_x11_stroke_arc(dev, xc + wc, yc     , (float)r, 3*angle, 4*angle, 4, line_thickness, col);
        nk_x11_stroke_arc(dev, xc     , yc     , (float)r, 2*angle, 3*angle, 4, line_thickness, col);
        nk_x11_stroke_arc(dev, xc     , yc + hc, (float)r,   angle, 2*angle, 4, line_thickness, col);
        nk_x11_stroke_arc(dev, xc + wc, yc + hc, (float)r,       0,   angle, 4, line_thickness, col);
    }
}

NK_INTERN void
nk_x11_stroke_polyline(const struct nk_x11_device *dev, 
    struct nk_vec2i *pnts, int count, unsigned short line_thickness, struct nk_color col)
{
    int i;
    glLineWidth(line_thickness);
    glColor4ub(col.r, col.g, col.b, col.a);
    glBegin(GL_LINE_STRIP);
    for (i = 0; i < count; ++i) {
        glVertex2f((float)pnts[i].x, (float)pnts[i].y);
    }
    glEnd();
}

NK_INTERN void
nk_x11_stroke_polygon(const struct nk_x11_device *dev, 
    struct nk_vec2i *pnts, int count, unsigned short line_thickness, struct nk_color col)
{
    int i;
    glLineWidth(line_thickness);
    glColor4ub(col.r, col.g, col.b, col.a);
    glBegin(GL_LINE_LOOP);
    for (i = 0; i < count; ++i) {
        glVertex2f((float)pnts[i].x, (float)pnts[i].y);
    }
    glEnd();
}

static const float EPSILON=0.0000000001f;

float Area(struct nk_vec2i *pnts, int count)
{
  int n = count;
  float A=0.0f;

  for(int p=n-1,q=0; q<n; p=q++)
  {
    A+= pnts[p].x*pnts[q].y - pnts[q].x*pnts[p].y;
  }
  return A*0.5f;
}

int InsideTriangle(float Ax, float Ay,
                   float Bx, float By,
                   float Cx, float Cy,
                   float Px, float Py)

{
  float ax, ay, bx, by, cx, cy, apx, apy, bpx, bpy, cpx, cpy;
  float cCROSSap, bCROSScp, aCROSSbp;

  ax = Cx - Bx;  ay = Cy - By;
  bx = Ax - Cx;  by = Ay - Cy;
  cx = Bx - Ax;  cy = By - Ay;
  apx= Px - Ax;  apy= Py - Ay;
  bpx= Px - Bx;  bpy= Py - By;
  cpx= Px - Cx;  cpy= Py - Cy;

  aCROSSbp = ax*bpy - ay*bpx;
  cCROSSap = cx*apy - cy*apx;
  bCROSScp = bx*cpy - by*cpx;

  return ((aCROSSbp >= 0.0f) && (bCROSScp >= 0.0f) && (cCROSSap >= 0.0f));
}

int Snip(struct nk_vec2i *pnts, int count,int u,int v,int w,int n,int *V)
{
  int p;
  float Ax, Ay, Bx, By, Cx, Cy, Px, Py;

  Ax = pnts[V[u]].x;
  Ay = pnts[V[u]].y;

  Bx = pnts[V[v]].x;
  By = pnts[V[v]].y;

  Cx = pnts[V[w]].x;
  Cy = pnts[V[w]].y;

  if ( EPSILON > (((Bx-Ax)*(Cy-Ay)) - ((By-Ay)*(Cx-Ax))) ) return 0;

  for (p=0;p<n;p++)
  {
    if( (p == u) || (p == v) || (p == w) ) continue;
    Px = pnts[V[p]].x;
    Py = pnts[V[p]].y;
    if (InsideTriangle(Ax,Ay,Bx,By,Cx,Cy,Px,Py)) return 0;
  }

  return 1;
}

// https://www.flipcode.com/archives/Efficient_Polygon_Triangulation.shtml
NK_INTERN void
nk_x11_fill_polygon(const struct nk_x11_device *dev,
    struct nk_vec2i *pnts, int count, const struct nk_color col)
{
    /* allocate and initialize list of Vertices in polygon */
#define MAX_POINTS 64
    int V[MAX_POINTS];
    int n = count;

    if ( n < 3 || n > MAX_POINTS) return;

    /* we want a counter-clockwise polygon in V */
    if ( 0.0f < Area(pnts,count) )
        for (int v=0; v<n; v++) V[v] = v;
    else
        for(int v=0; v<n; v++) V[v] = (n-1)-v;

    int nv = n;

    /*  remove nv-2 Vertices, creating 1 triangle every time */
    int new_count = 2*nv;   /* error detection */

    glColor4ub(col.r, col.g, col.b, col.a);
    glBegin(GL_TRIANGLES);
    for(int m=0, v=nv-1; nv>2; )
    {
        /* if we loop, it is probably a non-simple polygon */
        if (0 >= (new_count--))
        {
            //** Triangulate: ERROR - probable bad polygon!
            return;
        }

        /* three consecutive vertices in current polygon, <u,v,w> */
        int u = v  ; if (nv <= u) u = 0;     /* previous */
        v = u+1; if (nv <= v) v = 0;     /* new v    */
        int w = v+1; if (nv <= w) w = 0;     /* next     */

        if ( Snip(pnts,count,u,v,w,nv,V) )
        {
        int a,b,c,s,t;

        /* true names of the vertices */
        a = V[u]; b = V[v]; c = V[w];

        /* output Triangle */
        glVertex2f(pnts[a].x, pnts[a].y);
        glVertex2f(pnts[b].x, pnts[b].y);
        glVertex2f(pnts[c].x, pnts[c].y);

        m++;

        /* remove v from remaining polygon */
        for(s=v,t=v+1;t<nv;s++,t++) V[s] = V[t]; nv--;

        /* resest error detection counter */
        new_count = 2*nv;
        }
    }
    glEnd();

#undef MAX_POINTS
}

NK_INTERN void
nk_x11_fill_rect(const struct nk_x11_device *dev,
    const short x, const short y, const short w, const short h,
    const short r, const struct nk_color col)
{
    if (r == 0)
    {
        glColor4ub(col.r, col.g, col.b, col.a);
        glBegin(GL_TRIANGLES);
            glVertex2f(x,     y);
            glVertex2f(x,     y + h);
            glVertex2f(x + w, y);
            glVertex2f(x + w, y);
            glVertex2f(x,     y + h);
            glVertex2f(x + w, y + h);
        glEnd();
    }
    else
    {
        const short xc = x + r - 1;
        const short yc = y + r;
        const short wc = (short)(w - (2 * r)) + 1;
        const short hc = (short)(h - (2 * r)) + 1;

        struct nk_vec2i pnts[12];
        pnts[0].x = x;
        pnts[0].y = yc;
        pnts[1].x = xc;
        pnts[1].y = yc;
        pnts[2].x = xc;
        pnts[2].y = y;

        pnts[3].x = xc + wc;
        pnts[3].y = y;
        pnts[4].x = xc + wc;
        pnts[4].y = yc;
        pnts[5].x = x + w;
        pnts[5].y = yc;

        pnts[6].x = x + w;
        pnts[6].y = yc + hc;
        pnts[7].x = xc + wc;
        pnts[7].y = yc + hc;
        pnts[8].x = xc + wc;
        pnts[8].y = y + h;

        pnts[9].x = xc;
        pnts[9].y = y + h;
        pnts[10].x = xc;
        pnts[10].y = yc + hc;
        pnts[11].x = x;
        pnts[11].y = yc + hc;

        nk_x11_fill_polygon(dev, pnts, 12, col);

        float angle = 2.0f * 3.141592654f / 4.0f;

        nk_x11_fill_arc(dev, xc + wc, yc     , (float)r, 3*angle, 4*angle, 4, col);
        nk_x11_fill_arc(dev, xc     , yc     , (float)r, 2*angle, 3*angle, 4, col);
        nk_x11_fill_arc(dev, xc     , yc + hc, (float)r,   angle, 2*angle, 4, col);
        nk_x11_fill_arc(dev, xc + wc, yc + hc, (float)r,       0,   angle, 4, col);
    }
}

NK_INTERN void
nk_x11_draw_rect_multi_color(const struct nk_x11_device *dev,
    const short x, const short y, const short w, const short h, struct nk_color tl,
    struct nk_color tr, struct nk_color br, struct nk_color bl)
{
    glBegin(GL_TRIANGLES);
        glColor4ub(tl.r,tl.g,tl.b,tl.a);
        glVertex2f(x, y);
        glColor4ub(bl.r,bl.g,bl.b,bl.a);
        glVertex2f(x, y + h);
        glColor4ub(tr.r,tr.g,tr.b,tr.a);
        glVertex2f(x+w, y);
        glColor4ub(tr.r,tr.g,tr.b,tr.a);
        glVertex2f(x+w, y);
        glColor4ub(bl.r,bl.g,bl.b,bl.a);
        glVertex2f(x, y + h);
        glColor4ub(br.r,br.g,br.b,br.a);
        glVertex2f(x+w, y + h);
    glEnd();
}

NK_INTERN void
nk_x11_fill_triangle(const struct nk_x11_device *dev,
    const short x0, const short y0, const short x1, const short y1,
    const short x2, const short y2, const struct nk_color col)
{
    struct nk_vec2i pnts[3];
    pnts[0].x = x0;
    pnts[0].y = y0;
    pnts[1].x = x1;
    pnts[1].y = y1;
    pnts[2].x = x2;
    pnts[2].y = y2;
    nk_x11_fill_polygon(dev, pnts, 3, col);
}

NK_INTERN void
nk_x11_stroke_triangle(const struct nk_x11_device *dev,
    const short x0, const short y0, const short x1, const short y1,
    const short x2, const short y2, const unsigned short line_thickness,
    const struct nk_color col)
{
    struct nk_vec2i pnts[3];
    pnts[0].x = x0;
    pnts[0].y = y0;
    pnts[1].x = x1;
    pnts[1].y = y1;
    pnts[2].x = x2;
    pnts[2].y = y2;
    nk_x11_stroke_polygon(dev, pnts, 3, line_thickness, col);
}

NK_API void
nk_x11_render(enum nk_anti_aliasing AA)
{
    const struct nk_command *cmd;
    struct nk_context *ctx = &x11.ctx;
    int width, height;
    XWindowAttributes attr;
    XGetWindowAttributes(x11.dpy, x11.win, &attr);
    width = attr.width;
    height = attr.height;

    /* setup global state */
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* setup viewport/project */
    glViewport(0,0,(GLsizei)width,(GLsizei)height);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    nk_foreach(cmd, ctx)
    {
        switch (cmd->type) {
        case NK_COMMAND_NOP: break;
        case NK_COMMAND_SCISSOR: {
            const struct nk_command_scissor *s =(const struct nk_command_scissor*)cmd;
            glScissor((s->x)-1,(height - (s->y + s->h))-1,(s->w)+2,(s->h)+2);
        } break;
        case NK_COMMAND_LINE: {
            const struct nk_command_line *l = (const struct nk_command_line *)cmd;
            nk_x11_stroke_line(&x11.ogl, l->begin.x, l->begin.y, l->end.x, l->end.y, l->line_thickness, l->color);
        } break;
        case NK_COMMAND_RECT: {
            const struct nk_command_rect *r = (const struct nk_command_rect *)cmd;
            nk_x11_stroke_rect(&x11.ogl, r->x, r->y, r->w, r->h, r->rounding, r->line_thickness, r->color);
        } break;
        case NK_COMMAND_RECT_FILLED: {
            const struct nk_command_rect_filled *r = (const struct nk_command_rect_filled *)cmd;
            nk_x11_fill_rect(&x11.ogl, r->x, r->y, r->w, r->h, r->rounding, r->color);
        } break;
        case NK_COMMAND_RECT_MULTI_COLOR: {
            const struct nk_command_rect_multi_color *r = (const struct nk_command_rect_multi_color *)cmd;
	        nk_x11_draw_rect_multi_color(&x11.ogl, r->x, r->y, r->w, r->h, r->left, r->top, r->right, r->bottom);
        } break;
        case NK_COMMAND_CIRCLE: {
            const struct nk_command_circle *c = (const struct nk_command_circle *)cmd;
            nk_x11_stroke_circle(&x11.ogl, c->x, c->y, c->w, c->h, 22, c->line_thickness, c->color);
        } break;
        case NK_COMMAND_CIRCLE_FILLED: {
            const struct nk_command_circle_filled *c = (const struct nk_command_circle_filled *)cmd;        
            nk_x11_fill_circle(&x11.ogl, c->x, c->y, c->w, c->h, 22, c->color);
        } break;
        case NK_COMMAND_TRIANGLE: {
            const struct nk_command_triangle*t = (const struct nk_command_triangle*)cmd;
            nk_x11_stroke_triangle(&x11.ogl, t->a.x, t->a.y, t->b.x, t->b.y, t->c.x, t->c.y, 
                t->line_thickness, t->color);
        } break;
        case NK_COMMAND_TRIANGLE_FILLED: {
            const struct nk_command_triangle_filled *t = (const struct nk_command_triangle_filled *)cmd;
            nk_x11_fill_triangle(&x11.ogl, t->a.x, t->a.y, t->b.x, t->b.y, t->c.x, t->c.y, t->color);
        } break;
        case NK_COMMAND_TEXT: {
            const struct nk_command_text *t = (const struct nk_command_text*)cmd;
            nk_x11_draw_text(&x11.ogl, (struct nk_font*)t->font->userdata.ptr, nk_rect(t->x, t->y, t->w, t->h),
                t->string, t->length, t->height, t->foreground);
        } break;
        case NK_COMMAND_CURVE: {
            const struct nk_command_curve *q = (const struct nk_command_curve *)cmd;
            nk_x11_stroke_curve(&x11.ogl, q->begin, q->ctrl[0], q->ctrl[1],
                q->end, 22, q->line_thickness, q->color);
        } break;
        case NK_COMMAND_IMAGE: {
            const struct nk_command_image *i = (const struct nk_command_image *)cmd;
            nk_x11_draw_image(&x11.ogl, i->x, i->y, i->w, i->h, i->img, i->col);
        } break;
        case NK_COMMAND_ARC: {
            const struct nk_command_arc *a = (const struct nk_command_arc *)cmd;
            nk_x11_stroke_arc(&x11.ogl, a->cx, a->cy, a->r, a->a[0], a->a[1], 22, a->line_thickness, a->color);
        } break;
        case NK_COMMAND_ARC_FILLED: {
            const struct nk_command_arc_filled *a = (const struct nk_command_arc_filled *)cmd;
            nk_x11_fill_arc(&x11.ogl, a->cx, a->cy, a->r, a->a[0], a->a[1], 22, a->color);
        } break;
        case NK_COMMAND_POLYGON: {
            const struct nk_command_polygon *p =(const struct nk_command_polygon*)cmd;
            nk_x11_stroke_polygon(&x11.ogl, (struct nk_vec2i *)p->points, p->point_count, 
                p->line_thickness, p->color);
        } break;
        case NK_COMMAND_POLYGON_FILLED: {
            const struct nk_command_polygon_filled *p = (const struct nk_command_polygon_filled *)cmd;
            nk_x11_fill_polygon(&x11.ogl, (struct nk_vec2i *)p->points, p->point_count, p->color);
        } break;
        case NK_COMMAND_POLYLINE: {
            const struct nk_command_polyline *p = (const struct nk_command_polyline *)cmd;
            nk_x11_stroke_polyline(&x11.ogl, (struct nk_vec2i *)p->points, p->point_count, 
                p->line_thickness, p->color);
        } break;
        case NK_COMMAND_CUSTOM:
        default: break;
        }
    }
    nk_clear(ctx);
    glFlush();
    
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

}

NK_API void
nk_x11_font_stash_begin(struct nk_font_atlas **atlas)
{
    struct nk_allocator alloc;
    alloc.userdata.ptr = 0;
    alloc.alloc = nk_malloc;
    alloc.free = nk_mfree;
    nk_font_atlas_init(&x11.atlas,&alloc);
    nk_font_atlas_begin(&x11.atlas);
    *atlas = &x11.atlas;
}

NK_API void
nk_x11_font_stash_end(void)
{
    const void *image; int w, h;
    image = nk_font_atlas_bake(&x11.atlas, &w, &h, NK_FONT_ATLAS_RGBA32);
    nk_x11_device_upload_atlas(image, w, h);
    nk_font_atlas_end(&x11.atlas, nk_handle_id((int)x11.ogl.font_tex), &x11.ogl.null);
    if (x11.atlas.default_font)
        nk_style_set_font(&x11.ctx, &x11.atlas.default_font->handle);
}

NK_API int
nk_x11_handle_event(XEvent *evt)
{
    struct nk_context *ctx = &x11.ctx;

    /* optional grabbing behavior */
    if (ctx->input.mouse.grab) {
        XDefineCursor(x11.dpy, x11.win, x11.cursor);
        ctx->input.mouse.grab = 0;
    } else if (ctx->input.mouse.ungrab) {
        XWarpPointer(x11.dpy, None, x11.win, 0, 0, 0, 0,
            (int)ctx->input.mouse.pos.x, (int)ctx->input.mouse.pos.y);
        XUndefineCursor(x11.dpy, x11.win);
        ctx->input.mouse.ungrab = 0;
    }

    if (evt->type == KeyPress || evt->type == KeyRelease)
    {
        /* Key handler */
        int ret, down = (evt->type == KeyPress);
        KeySym *code = XGetKeyboardMapping(x11.dpy, (KeyCode)evt->xkey.keycode, 1, &ret);
        if (*code == XK_Shift_L || *code == XK_Shift_R) nk_input_key(ctx, NK_KEY_SHIFT, down);
        else if (*code == XK_Control_L || *code == XK_Control_R) nk_input_key(ctx, NK_KEY_CTRL, down);
        else if (*code == XK_Delete)    nk_input_key(ctx, NK_KEY_DEL, down);
        else if (*code == XK_Return)    nk_input_key(ctx, NK_KEY_ENTER, down);
        else if (*code == XK_Tab)       nk_input_key(ctx, NK_KEY_TAB, down);
        else if (*code == XK_Left)      nk_input_key(ctx, NK_KEY_LEFT, down);
        else if (*code == XK_Right)     nk_input_key(ctx, NK_KEY_RIGHT, down);
        else if (*code == XK_Up)        nk_input_key(ctx, NK_KEY_UP, down);
        else if (*code == XK_Down)      nk_input_key(ctx, NK_KEY_DOWN, down);
        else if (*code == XK_BackSpace) nk_input_key(ctx, NK_KEY_BACKSPACE, down);
        else if (*code == XK_space && !down) nk_input_char(ctx, ' ');
        else if (*code == XK_Page_Up)   nk_input_key(ctx, NK_KEY_SCROLL_UP, down);
        else if (*code == XK_Page_Down) nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down);
        else if (*code == XK_Home) {
            nk_input_key(ctx, NK_KEY_TEXT_START, down);
            nk_input_key(ctx, NK_KEY_SCROLL_START, down);
        } else if (*code == XK_End) {
            nk_input_key(ctx, NK_KEY_TEXT_END, down);
            nk_input_key(ctx, NK_KEY_SCROLL_END, down);
        } else {
            if (*code == 'c' && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_COPY, down);
            else if (*code == 'v' && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_PASTE, down);
            else if (*code == 'x' && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_CUT, down);
            else if (*code == 'z' && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_TEXT_UNDO, down);
            else if (*code == 'r' && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_TEXT_REDO, down);
            else if (*code == XK_Left && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, down);
            else if (*code == XK_Right && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, down);
            else if (*code == 'b' && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_TEXT_LINE_START, down);
            else if (*code == 'e' && (evt->xkey.state & ControlMask))
                nk_input_key(ctx, NK_KEY_TEXT_LINE_END, down);
            else {
                if (*code == 'i')
                    nk_input_key(ctx, NK_KEY_TEXT_INSERT_MODE, down);
                else if (*code == 'r')
                    nk_input_key(ctx, NK_KEY_TEXT_REPLACE_MODE, down);
                if (down) {
                    char buf[32];
                    KeySym keysym = 0;
                    if (XLookupString((XKeyEvent*)evt, buf, 32, &keysym, NULL) != NoSymbol)
                        nk_input_glyph(ctx, buf);
                }
            }
        }
        XFree(code);
        return 1;
    } else if (evt->type == ButtonPress || evt->type == ButtonRelease) {
        /* Button handler */
        int down = (evt->type == ButtonPress);
        const int x = evt->xbutton.x, y = evt->xbutton.y;
        if (evt->xbutton.button == Button1) {
            if (down) { /* Double-Click Button handler */
                long dt = nk_timestamp() - x11.last_button_click;
                if (dt > NK_X11_DOUBLE_CLICK_LO && dt < NK_X11_DOUBLE_CLICK_HI)
                    nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, nk_true);
                x11.last_button_click = nk_timestamp();
            } else nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, nk_false);
            nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down);
        } else if (evt->xbutton.button == Button2)
            nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, down);
        else if (evt->xbutton.button == Button3)
            nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, down);
        else if (evt->xbutton.button == Button4)
            nk_input_scroll(ctx, nk_vec2(0,1.0f));
        else if (evt->xbutton.button == Button5)
            nk_input_scroll(ctx, nk_vec2(0,-1.0f));
        else return 0;
        return 1;
    } else if (evt->type == MotionNotify) {
        /* Mouse motion handler */
        const int x = evt->xmotion.x, y = evt->xmotion.y;
        nk_input_motion(ctx, x, y);
        if (ctx->input.mouse.grabbed) {
            ctx->input.mouse.pos.x = ctx->input.mouse.prev.x;
            ctx->input.mouse.pos.y = ctx->input.mouse.prev.y;
            XWarpPointer(x11.dpy, None, x11.win, 0, 0, 0, 0, (int)ctx->input.mouse.pos.x, (int)ctx->input.mouse.pos.y);
        }
        return 1;
    } else if (evt->type == KeymapNotify) {
        XRefreshKeyboardMapping(&evt->xmapping);
        return 1;
    }
    return 0;
}

NK_API struct nk_context*
nk_x11_init(Display *dpy, Window win)
{
    struct nk_allocator alloc;
    alloc.userdata.ptr = 0;
    alloc.alloc = nk_malloc;
    alloc.free = nk_mfree;

    x11.dpy = dpy;
    x11.win = win;

    if (!setlocale(LC_ALL,"")) return 0;
    if (!XSupportsLocale()) return 0;
    if (!XSetLocaleModifiers("@im=none")) return 0;

    /* create invisible cursor */
    {static XColor dummy; char data[1] = {0};
    Pixmap blank = XCreateBitmapFromData(dpy, win, data, 1, 1);
    if (blank == None) return 0;
    x11.cursor = XCreatePixmapCursor(dpy, blank, blank, &dummy, &dummy, 0, 0);
    XFreePixmap(dpy, blank);}

    nk_buffer_init(&x11.ogl.cmds, &alloc, NK_BUFFER_DEFAULT_INITIAL_SIZE);
    nk_init(&x11.ctx, &alloc, 0);
    return &x11.ctx;
}

NK_API void
nk_x11_shutdown(void)
{
    struct nk_x11_device *dev = &x11.ogl;
    nk_font_atlas_clear(&x11.atlas);
    nk_free(&x11.ctx);
    glDeleteTextures(1, &dev->font_tex);
    nk_buffer_free(&dev->cmds);
    XFreeCursor(x11.dpy, x11.cursor);
    memset(&x11, 0, sizeof(x11));
}

#endif
