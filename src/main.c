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

#include <gtk/gtk.h>
#include <pthread.h>
#include <sys/time.h>

#include "rototiller.h"

/* glimmer is a GTK+-3.0 frontend for rototiller */

extern fb_ops_t gtk_fb_ops;

#define DEFAULT_WIDTH	320
#define DEFAULT_HEIGHT	480

#define BOX_SPACING	4
#define NUM_FB_PAGES	3

static struct glimmer_t {
	GtkWidget			*modules_list;

	fb_t				*fb;
	settings_t			*fb_settings;

	settings_t			*module_settings;
	const rototiller_module_t	*module;
	void				*module_context;
	pthread_t			thread;
	struct timeval			start_tv;
	unsigned			ticks_offset;	/* XXX: this isn't leveraged currently */
} glimmer;


static unsigned get_ticks(const struct timeval *start, const struct timeval *now, unsigned offset)
{
	return (unsigned)((now->tv_sec - start->tv_sec) * 1000 + (now->tv_usec - start->tv_usec) / 1000) + offset;
}


/* TODO: this should probably move into librototiller */
static void * glimmer_thread(void *foo)
{
	struct timeval	now;

	for (;;) {
		fb_page_t	*page;
		unsigned	ticks;

		page = fb_page_get(glimmer.fb);
		gettimeofday(&now, NULL);
		ticks = get_ticks(&glimmer.start_tv, &now, glimmer.ticks_offset);
		rototiller_module_render(glimmer.module, glimmer.module_context, ticks, &page->fragment);
		fb_page_put(glimmer.fb, page);
	}
}


static void glimmer_go(GtkButton *button, gpointer user_data)
{
	int	r;

	if (glimmer.fb) {
		pthread_cancel(glimmer.thread);
		pthread_join(glimmer.thread, NULL);

		glimmer.fb = fb_free(glimmer.fb);
		glimmer.fb_settings = settings_free(glimmer.fb_settings);
		glimmer.module_settings = settings_free(glimmer.module_settings);
	}

	/* TODO: translate the GTK+ settings panel values into
	 * glimmer.{fb,module}_settings
	 */

	/* For now, construct a simple 640x480 non-fullscreen fb, and
	 * simply don't do any module setup (those *should* have static builtin
	 * defaults that at least work on some level.
	 */
	glimmer.fb_settings = settings_new("fullscreen=off,size=640x480");
	glimmer.module_settings = settings_new("TODO");

	r = fb_new(&gtk_fb_ops, glimmer.fb_settings, NUM_FB_PAGES, &glimmer.fb);
	if (r < 0) {
		puts("fb no go!");
		return;
	}

	gettimeofday(&glimmer.start_tv, NULL);
	glimmer.module = rototiller_lookup_module(gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(glimmer.modules_list)));
	r = rototiller_module_create_context(
					glimmer.module,
					get_ticks(
						&glimmer.start_tv,
						&glimmer.start_tv,
						glimmer.ticks_offset),
					&glimmer.module_context);
	if (r < 0) {
		puts("context no go!");
		return;
	}

	pthread_create(&glimmer.thread, NULL, glimmer_thread, NULL);
}


static void activate(GtkApplication *app, gpointer user_data)
{
	GtkWidget			*window, *vbox, *settings, *button;
	const rototiller_module_t	**modules;
	size_t				n_modules;

	rototiller_get_modules(&modules, &n_modules);

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "glimmer");
	gtk_window_set_default_size(GTK_WINDOW(window), DEFAULT_WIDTH, DEFAULT_HEIGHT);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, BOX_SPACING);
	gtk_container_add(GTK_CONTAINER(window), vbox);

	glimmer.modules_list = gtk_combo_box_text_new();
	for (size_t i = 0; i < n_modules; i++) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(glimmer.modules_list), NULL, modules[i]->name);

		/* like rototiller, default to rtv */
		if (!strcmp(modules[i]->name, "rtv"))
			gtk_combo_box_set_active(GTK_COMBO_BOX(glimmer.modules_list), i);
	}
	gtk_container_add(GTK_CONTAINER(vbox), glimmer.modules_list);

	gtk_box_set_child_packing(
			GTK_BOX(vbox),
			glimmer.modules_list,
			FALSE,
			FALSE,
			BOX_SPACING * 4, /* FIXME: having the combo box too near the window edge puts the pointer into the scroll-up arrow on click :/ */
			GTK_PACK_START);

	/* TODO: below the combobox, present framebuffer and the selected module's settings */
	settings = gtk_label_new("TODO: fb/module settings here");
	gtk_container_add(GTK_CONTAINER(vbox), settings);
	gtk_box_set_child_packing(
			GTK_BOX(vbox),
			settings,
			TRUE,
			TRUE,
			BOX_SPACING,
			GTK_PACK_START);

	/* button to rototill as configured */
	button = gtk_button_new_with_label("Go!");
	gtk_container_add(GTK_CONTAINER(vbox), button);
	g_signal_connect(button, "clicked", G_CALLBACK(glimmer_go), NULL);

	gtk_widget_show_all(window);
}


int main(int argc, char **argv)
{
	GtkApplication	*app;
	int		status;

	rototiller_init();
	app = gtk_application_new("com.pengaru.glimmer", G_APPLICATION_FLAGS_NONE);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	rototiller_shutdown();

	return status;
}
