/*
 * Copyright © 2008 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <cairo.h>
#include <glib.h>
#include <cairo-drm.h>

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "wayland-util.h"
#include "wayland-client.h"
#include "wayland-glib.h"

#include "window.h"

static const char gem_device[] = "/dev/dri/card0";
static const char socket_name[] = "\0wayland";

struct gears {
	struct window *window;

	struct display *d;
	struct rectangle rectangle;

	EGLDisplay display;
	EGLContext context;
	EGLImageKHR image;
	int drm_fd;
	int resized;
	GLfloat angle;
	cairo_surface_t *cairo_surface;

	GLint gear_list[3];
	GLuint fbo, color_rbo, depth_rbo;
};

struct gear_template {
	GLfloat material[4];
	GLfloat inner_radius;
	GLfloat outer_radius;
	GLfloat width;
	GLint teeth;
	GLfloat tooth_depth;
};

const static struct gear_template gear_templates[] = {
	{ { 0.8, 0.1, 0.0, 1.0 }, 1.0, 4.0, 1.0, 20, 0.7 },
	{ { 0.0, 0.8, 0.2, 1.0 }, 0.5, 2.0, 2.0, 10, 0.7 },
	{ { 0.2, 0.2, 1.0, 1.0 }, 1.3, 2.0, 0.5, 10, 0.7 }, 
};

static GLfloat light_pos[4] = {5.0, 5.0, 10.0, 0.0};

static void die(const char *msg)
{
	fprintf(stderr, "%s", msg);
	exit(EXIT_FAILURE);
}

static void
make_gear(const struct gear_template *t)
{
	GLint i;
	GLfloat r0, r1, r2;
	GLfloat angle, da;
	GLfloat u, v, len;

	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, t->material);

	r0 = t->inner_radius;
	r1 = t->outer_radius - t->tooth_depth / 2.0;
	r2 = t->outer_radius + t->tooth_depth / 2.0;

	da = 2.0 * M_PI / t->teeth / 4.0;

	glShadeModel(GL_FLAT);

	glNormal3f(0.0, 0.0, 1.0);

	/* draw front face */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		if (i < t->teeth) {
			glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
			glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
		}
	}
	glEnd();

	/* draw front sides of teeth */
	glBegin(GL_QUADS);
	da = 2.0 * M_PI / t->teeth / 4.0;
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), t->width * 0.5);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
	}
	glEnd();

	glNormal3f(0.0, 0.0, -1.0);

	/* draw back face */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		if (i < t->teeth) {
			glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
			glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		}
	}
	glEnd();

	/* draw back sides of teeth */
	glBegin(GL_QUADS);
	da = 2.0 * M_PI / t->teeth / 4.0;
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
	}
	glEnd();

	/* draw outward faces of teeth */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
		u = r2 * cos(angle + da) - r1 * cos(angle);
		v = r2 * sin(angle + da) - r1 * sin(angle);
		len = sqrt(u * u + v * v);
		u /= len;
		v /= len;
		glNormal3f(v, -u, 0.0);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -t->width * 0.5);
		glNormal3f(cos(angle), sin(angle), 0.0);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -t->width * 0.5);
		u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
		v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
		glNormal3f(v, -u, 0.0);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
		glNormal3f(cos(angle), sin(angle), 0.0);
	}

	glVertex3f(r1 * cos(0), r1 * sin(0), t->width * 0.5);
	glVertex3f(r1 * cos(0), r1 * sin(0), -t->width * 0.5);

	glEnd();

	glShadeModel(GL_SMOOTH);

	/* draw inside radius cylinder */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glNormal3f(-cos(angle), -sin(angle), 0.0);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
	}
	glEnd();
}

static void
draw_gears(struct gears *gears)
{
	GLfloat view_rotx = 20.0, view_roty = 30.0, view_rotz = 0.0;

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glPushMatrix();

	glTranslatef(0.0, 0.0, -50);

	glRotatef(view_rotx, 1.0, 0.0, 0.0);
	glRotatef(view_roty, 0.0, 1.0, 0.0);
	glRotatef(view_rotz, 0.0, 0.0, 1.0);

	glPushMatrix();
	glTranslatef(-3.0, -2.0, 0.0);
	glRotatef(gears->angle, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[0]);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(3.1, -2.0, 0.0);
	glRotatef(-2.0 * gears->angle - 9.0, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[1]);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(-3.1, 4.2, 0.0);
	glRotatef(-2.0 * gears->angle - 25.0, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[2]);
	glPopMatrix();

	glPopMatrix();

	glFlush();
}

static void
resize_window(struct gears *gears)
{
	EGLint attribs[] = {
		EGL_WIDTH,		0,
		EGL_HEIGHT,		0,
		EGL_IMAGE_FORMAT_MESA,	EGL_IMAGE_FORMAT_ARGB8888_MESA,
		EGL_IMAGE_USE_MESA,	EGL_IMAGE_USE_SHARE_MESA |
					EGL_IMAGE_USE_SCANOUT_MESA,
		EGL_NONE
	};

	/* Constrain child size to be square and at least 300x300 */
	window_get_child_rectangle(gears->window, &gears->rectangle);
	if (gears->rectangle.width > gears->rectangle.height)
		gears->rectangle.height = gears->rectangle.width;
	else
		gears->rectangle.width = gears->rectangle.height;
	if (gears->rectangle.width < 300) {
		gears->rectangle.width = 300;
		gears->rectangle.height = 300;
	}
	window_set_child_size(gears->window, &gears->rectangle);

	window_draw(gears->window);

	if (gears->image)
		eglDestroyImageKHR(gears->display, gears->image);
	attribs[1] = gears->rectangle.width;
	attribs[3] = gears->rectangle.height;
	gears->image = eglCreateDRMImageMESA(gears->display, attribs);

	glBindRenderbuffer(GL_RENDERBUFFER_EXT, gears->color_rbo);
	glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, gears->image);

	glBindRenderbuffer(GL_RENDERBUFFER_EXT, gears->depth_rbo);
	glRenderbufferStorage(GL_RENDERBUFFER_EXT,
			      GL_DEPTH_COMPONENT,
			      gears->rectangle.width,
			      gears->rectangle.height);

	glViewport(0, 0, gears->rectangle.width, gears->rectangle.height);

	gears->resized = 0;
}

static void
resize_handler(struct window *window, void *data)
{
	struct gears *gears = data;

	/* Right now, resizing the window from the per-frame callback
	 * is fine, since the window drawing code is so slow that we
	 * can't draw more than one window per frame anyway.  However,
	 * once we implement faster resizing, this will show lag
	 * between pointer motion and window size even if resizing is
	 * fast.  We need to keep processing motion events and posting
	 * new frames as fast as possible so when the server
	 * composites the next frame it will have the most recent size
	 * possible, like what we do for window moves. */

	gears->resized = 1;
}

static void
keyboard_focus_handler(struct window *window,
		       struct wl_input_device *device, void *data)
{
	struct gears *gears = data;

	gears->resized = 1;
}

static void
acknowledge_handler(struct window *window,
		    uint32_t key, uint32_t frame,
		    void *data)
{
	struct gears *gears = data;

	if (key == 10) {
		if (gears->resized)
			resize_window(gears);

		draw_gears(gears);
	}
}

static void
frame_handler(struct window *window,
	      uint32_t frame, uint32_t timestamp, void *data)
{
  	struct gears *gears = data;

	window_copy_image(gears->window, &gears->rectangle, gears->image);

	window_commit(gears->window, 10);

	gears->angle = (GLfloat) (timestamp % 8192) * 360 / 8192.0;
}

static struct gears *
gears_create(struct display *display)
{
	const int x = 200, y = 200, width = 450, height = 500;
	EGLint major, minor, count;
	EGLConfig config;
	struct gears *gears;
	int i;

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE,		0,
		EGL_NO_SURFACE_CAPABLE_MESA,	EGL_OPENGL_BIT,
		EGL_RENDERABLE_TYPE,		EGL_OPENGL_BIT,
		EGL_NONE
	};

	gears = malloc(sizeof *gears);
	memset(gears, 0, sizeof *gears);
	gears->d = display;
	gears->window = window_create(display, "Wayland Gears",
				      x, y, width, height);

	gears->display = display_get_egl_display(gears->d);
	if (gears->display == NULL)
		die("failed to create egl display\n");

	if (!eglInitialize(gears->display, &major, &minor))
		die("failed to initialize display\n");

	if (!eglChooseConfig(gears->display, config_attribs, &config, 1, &count) ||
	    count == 0)
		die("eglChooseConfig() failed\n");

	eglBindAPI(EGL_OPENGL_API);

	gears->context = eglCreateContext(gears->display, config, EGL_NO_CONTEXT, NULL);
	if (gears->context == NULL)
		die("failed to create context\n");

	if (!eglMakeCurrent(gears->display, NULL, NULL, gears->context))
		die("faile to make context current\n");

	glGenFramebuffers(1, &gears->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER_EXT, gears->fbo);

	glGenRenderbuffers(1, &gears->color_rbo);
	glBindRenderbuffer(GL_RENDERBUFFER_EXT, gears->color_rbo);
	glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER_EXT,
				  GL_COLOR_ATTACHMENT0_EXT,
				  GL_RENDERBUFFER_EXT,
				  gears->color_rbo);

	glGenRenderbuffers(1, &gears->depth_rbo);
	glBindRenderbuffer(GL_RENDERBUFFER_EXT, gears->depth_rbo);
	glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER_EXT,
				  GL_DEPTH_ATTACHMENT_EXT,
				  GL_RENDERBUFFER_EXT,
				  gears->depth_rbo);
	for (i = 0; i < 3; i++) {
		gears->gear_list[i] = glGenLists(1);
		glNewList(gears->gear_list[i], GL_COMPILE);
		make_gear(&gear_templates[i]);
		glEndList();
	}

	glEnable(GL_NORMALIZE);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.0, 1.0, -1.0, 1.0, 5.0, 200.0);
	glMatrixMode(GL_MODELVIEW);

	glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
	glEnable(GL_CULL_FACE);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 0.92);

	if (glCheckFramebufferStatus (GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "framebuffer incomplete\n");

	resize_window(gears);
	draw_gears(gears);
	frame_handler(gears->window, 0, 0, gears);

	window_set_resize_handler(gears->window, resize_handler, gears);
	window_set_keyboard_focus_handler(gears->window, keyboard_focus_handler, gears);
	window_set_acknowledge_handler(gears->window, acknowledge_handler, gears);
	window_set_frame_handler(gears->window, frame_handler, gears);

	return gears;
}

int main(int argc, char *argv[])
{
	struct display *d;
	struct gears *gears;

	d = display_create(&argc, &argv, NULL);
	gears = gears_create(d);
	display_run(d);

	return 0;
}