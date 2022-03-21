/*
 *  Copyright (C) 2021 - Vito Caputo - <vcaputo@pengaru.com>
 *
 *  This program is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 3 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <assert.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>

#include <til_fb.h>
#include <til_settings.h>

/* glimmer's GTK+-3.0 backend fb for rototiller */

typedef struct gtk_fb_t {
	GtkWidget	*window;
	GtkWidget	*image;
	unsigned	width, height;
	unsigned	fullscreen:1;
	unsigned	resized:1;
} gtk_fb_t;

typedef struct gtk_fb_page_t gtk_fb_page_t;

struct gtk_fb_page_t {
	cairo_surface_t	*surface;
};


/* called on "size-allocate" for the fb's gtk window */
static gboolean resized(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	gtk_fb_t	*c = user_data;
	GtkAllocation	alloc;

	gtk_widget_get_allocation(c->image, &alloc);
	if (c->width != alloc.width ||
	    c->height != alloc.height) {

		/* just cache the new dimensions and set a resized flag, these will
		 * become realized @ flip time where the fb is available by telling
		 * the fb to rebuild via fb_rebuild() and clearing the resized flag.
		 */

		c->width = alloc.width;
		c->height = alloc.height;
		c->resized = 1;
	}

	return FALSE;
}


/* parse settings and get the output window realized before
 * attempting to create any pages "similar" to it.
 */
static int gtk_fb_init(const til_settings_t *settings, void **res_context)
{
	const char	*fullscreen;
	const char	*size;
	gtk_fb_t	*c;
	int		r;

	assert(settings);
	assert(res_context);

	fullscreen = til_settings_get_value(settings, "fullscreen", NULL);
	if (!fullscreen)
		return -EINVAL;

	size = til_settings_get_value(settings, "size", NULL);
	if (!size && !strcasecmp(fullscreen, "off"))
		return -EINVAL;

	c = calloc(1, sizeof(gtk_fb_t));
	if (!c)
		return -ENOMEM;

	if (!strcasecmp(fullscreen, "on"))
		c->fullscreen = 1;

	if (size) /* TODO: errors */
		sscanf(size, "%u%*[xX]%u", &c->width, &c->height);

	c->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_realize(c->window);
	g_signal_connect_after(c->window, "size-allocate", G_CALLBACK(resized), c);

	*res_context = c;

	return 0;
}


static void gtk_fb_shutdown(til_fb_t *fb, void *context)
{
	gtk_fb_t	*c = context;

	gtk_widget_destroy(c->window);
	free(c);
}


/* This performs the page flip on the "draw" signal, triggerd
 * on every "tick" by queue_draw_cb() below.
 * Note that "tick" in this context is a gtk concept, and unrelated to
 * rototiller ticks.  See gtk frame clocks for more info.
 * This is a little awkward as we're calling the public fb API from
 * the underlying implementation, maybe fix it up later.
 */
static gboolean draw_cb(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
	til_fb_t	*fb = user_data;

	til_fb_flip(fb);

	return FALSE;
}

/* this just queues drawing the image on the "tick" */
static gboolean queue_draw_cb(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data)
{
	gtk_fb_t	*c = user_data;

	gtk_widget_queue_draw(c->image);

	return G_SOURCE_CONTINUE;
}


static int gtk_fb_acquire(til_fb_t *fb, void *context, void *page)
{
	gtk_fb_t	*c = context;
	gtk_fb_page_t	*p = page;

	c->image = gtk_image_new_from_surface(p->surface);
	g_signal_connect(c->image, "draw", G_CALLBACK(draw_cb), fb);
	gtk_widget_add_tick_callback(c->image, queue_draw_cb, c, NULL);
	gtk_container_add(GTK_CONTAINER(c->window), c->image);
	gtk_widget_show_all(c->window);

	return 0;
}


static void gtk_fb_release(til_fb_t *fb, void *context)
{
	gtk_fb_t	*c = context;

	gtk_widget_destroy(c->image);
}


static void * gtk_fb_page_alloc(til_fb_t *fb, void *context, til_fb_page_t *res_page)
{
	gtk_fb_t	*c = context;
	gtk_fb_page_t	*p;
	GdkWindow	*gdk_window;

	p = calloc(1, sizeof(gtk_fb_page_t));
	if (!p)
		return NULL;

	/* by using gdk_window_create_similar_image_surface(), we enable
	 * potential optimizations like XSHM use on the xlib cairo backend.
	 */
	gdk_window = gtk_widget_get_window(c->window);
	p->surface = gdk_window_create_similar_image_surface(gdk_window, CAIRO_FORMAT_RGB24, c->width, c->height, 0);

	res_page->fragment.buf = (uint32_t *)cairo_image_surface_get_data(p->surface);
	res_page->fragment.width = c->width;
	res_page->fragment.frame_width = c->width;
	res_page->fragment.height = c->height;
	res_page->fragment.frame_height = c->height;
	res_page->fragment.stride = cairo_image_surface_get_stride(p->surface) - (c->width * 4);
	res_page->fragment.pitch = cairo_image_surface_get_stride(p->surface);

	cairo_surface_flush(p->surface);
	cairo_surface_mark_dirty(p->surface);

	return p;
}


static int gtk_fb_page_free(til_fb_t *fb, void *context, void *page)
{
	gtk_fb_t	*c = context;
	gtk_fb_page_t	*p = page;

	cairo_surface_destroy(p->surface);
	free(p);

	return 0;
}


/* XXX: due to gtk's event-driven nature, this isn't a vsync-synchronous page flip,
 * so til_fb_flip() must be scheduled independently to not just spin.
 * The "draw" signal on the image is used to drive til_fb_flip() on frameclock "ticks",
 * a method suggested by Christian Hergert, thanks!
 */
static int gtk_fb_page_flip(til_fb_t *fb, void *context, void *page)
{
	gtk_fb_t	*c = context;
	gtk_fb_page_t	*p = page;

	cairo_surface_mark_dirty(p->surface);
	gtk_image_set_from_surface(GTK_IMAGE(c->image), p->surface);

	if (c->resized) {
		c->resized = 0;
		til_fb_rebuild(fb);
	}

	return 0;
}


til_fb_ops_t gtk_fb_ops = {
	/* TODO: .setup may not be necessary in the gtk frontend, unless maybe
	 * it learns to use multiple fb backends, and would like to do the whole dynamic
	 * settings iterative dance established by rototiller.
	.setup = gtk_fb_setup,
	 */

	/* everything else seems to not be too far out of wack for the new frontend as-is,
	 * I only had to plumb down the til_fb_t *fb, which classic rototiller didn't need to do.
	 */
	.init = gtk_fb_init,
	.shutdown = gtk_fb_shutdown,
	.acquire = gtk_fb_acquire,
	.release = gtk_fb_release,
	.page_alloc = gtk_fb_page_alloc,
	.page_free = gtk_fb_page_free,
	.page_flip = gtk_fb_page_flip
};
