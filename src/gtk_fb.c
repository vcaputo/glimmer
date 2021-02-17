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

#include "fb.h"
#include "settings.h"

/* glimmer's GTK+-3.0 backend fb for rototiller */

typedef struct gtk_fb_t {
	GtkWidget	*window;
	GtkWidget	*image;
	unsigned	width, height;
	unsigned	fullscreen:1;
} gtk_fb_t;

typedef struct gtk_fb_page_t gtk_fb_page_t;

struct gtk_fb_page_t {
	cairo_surface_t	*surface;
};


/* this doesn't really do anything significant on gtk */
static int gtk_fb_init(const settings_t *settings, void **res_context)
{
	const char	*fullscreen;
	const char	*size;
	gtk_fb_t	*c;
	int		r;

	assert(settings);
	assert(res_context);

	fullscreen = settings_get_value(settings, "fullscreen");
	if (!fullscreen)
		return -EINVAL;

	size = settings_get_value(settings, "size");
	if (!size && !strcasecmp(fullscreen, "off"))
		return -EINVAL;

	c = calloc(1, sizeof(gtk_fb_t));
	if (!c)
		return -ENOMEM;

	if (!strcasecmp(fullscreen, "on"))
		c->fullscreen = 1;

	if (size) /* TODO: errors */
		sscanf(size, "%u%*[xX]%u", &c->width, &c->height);

	*res_context = c;

	return 0;
}


static void gtk_fb_shutdown(fb_t *fb, void *context)
{
	gtk_fb_t	*c = context;

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
	fb_t	*fb = user_data;

	fb_flip(fb);

	return FALSE;
}

/* this just queues drawing the image on the "tick" */
static gboolean queue_draw_cb(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data)
{
	gtk_fb_t	*c = user_data;

	gtk_widget_queue_draw(c->image);

	return G_SOURCE_CONTINUE;
}


static int gtk_fb_acquire(fb_t *fb, void *context, void *page)
{
	gtk_fb_t	*c = context;
	gtk_fb_page_t	*p = page;

	c->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	c->image = gtk_image_new_from_surface(p->surface);
	g_signal_connect(c->image, "draw", G_CALLBACK(draw_cb), fb);
	gtk_widget_add_tick_callback(c->image, queue_draw_cb, c, NULL);
	gtk_container_add(GTK_CONTAINER(c->window), c->image);
	gtk_widget_show_all(c->window);

	return 0;
}


static void gtk_fb_release(fb_t *fb, void *context)
{
	gtk_fb_t	*c = context;

	gtk_widget_destroy(c->window);
}


static void * gtk_fb_page_alloc(fb_t *fb, void *context, fb_page_t *res_page)
{
	gtk_fb_t	*c = context;
	gtk_fb_page_t	*p;

	p = calloc(1, sizeof(gtk_fb_page_t));
	if (!p)
		return NULL;

	/* XXX: note this is a plain in-memory surface that will always work everywhere,
	 * but that generality prevents potential optimizations.
	 * With some extra effort, on backends like X, an xshm surface could be
	 * created instead, for a potential performance boost by having the
	 * surface contents accessible server-side where accelerated copies may
	 * be used, while also accessible client-side where rototiller draws into.
	 * TODO if better X performance is desired.
	 */
	p->surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, c->width, c->height);

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


static int gtk_fb_page_free(fb_t *fb, void *context, void *page)
{
	gtk_fb_t	*c = context;
	gtk_fb_page_t	*p = page;

	cairo_surface_destroy(p->surface);
	free(p);

	return 0;
}


/* XXX: due to gtk's event-driven nature, this isn't a vsync-synchronous page flip,
 * so and fb_flip() must be scheduled independently to not just spin.
 * The "draw" signal on the image is used to drive fb_flip() on frameclock "ticks",
 * a method suggested by Christian Hergert, thanks!
 */
static int gtk_fb_page_flip(fb_t *fb, void *context, void *page)
{
	gtk_fb_t	*c = context;
	gtk_fb_page_t	*p = page;

	cairo_surface_mark_dirty(p->surface);
	gtk_image_set_from_surface(GTK_IMAGE(c->image), p->surface);

	return 0;
}


fb_ops_t gtk_fb_ops = {
	/* TODO: .setup may not be necessary in the gtk frontend, unless maybe
	 * it learns to use multiple fb backends, and would like to do the whole dynamic
	 * settings iterative dance established by rototiller.
	.setup = gtk_fb_setup,
	 */

	/* everything else seems to not be too far out of wack for the new frontend as-is,
	 * I only had to plumb down the fb_t *fb, which classic rototiller didn't need to do.
	 */
	.init = gtk_fb_init,
	.shutdown = gtk_fb_shutdown,
	.acquire = gtk_fb_acquire,
	.release = gtk_fb_release,
	.page_alloc = gtk_fb_page_alloc,
	.page_free = gtk_fb_page_free,
	.page_flip = gtk_fb_page_flip
};
