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

#include <til.h>
#include <til_args.h>

/* glimmer is a GTK+-3.0 frontend for rototiller */

extern til_fb_ops_t gtk_fb_ops;

#define DEFAULT_WIDTH	320
#define DEFAULT_HEIGHT	480

#define DEFAULT_MODULE	"rtv"
#define BOX_SPACING	1
#define FRAME_MARGIN	8
#define LABEL_MARGIN	4
#define NUM_FB_PAGES	3

static struct glimmer_t {
	GtkComboBox		*modules_combobox;
	GtkWidget		*module_box, *module_frame;

	til_args_t		args;
	til_settings_t		*video_settings;
	til_settings_t		*module_settings;

	til_fb_t		*fb;
	const til_module_t	*module;
	void			*module_context;
	pthread_t		thread;
	struct timeval		start_tv;
	unsigned		ticks_offset;	/* XXX: this isn't leveraged currently */
} glimmer;


static void glimmer_module_setup(const til_module_t *module, til_settings_t *settings);


static unsigned glimmer_get_ticks(const struct timeval *start, const struct timeval *now, unsigned offset)
{
	return (unsigned)((now->tv_sec - start->tv_sec) * 1000 + (now->tv_usec - start->tv_usec) / 1000) + offset;
}


static void glimmer_active_module(const til_module_t **res_module, til_settings_t **res_settings)
{
	GtkTreeIter iter;

	if (gtk_combo_box_get_active_iter(glimmer.modules_combobox, &iter)) {
		GtkTreeModel *model = gtk_combo_box_get_model(glimmer.modules_combobox);
		char		*name;

		gtk_tree_model_get(model, &iter,
				0, &name,
				1, res_module,
				2, res_settings,
				-1);

		g_free(name);
	}
}


static void glimmer_active_module_setup(void)
{
	const til_module_t	*module;
	til_settings_t	*settings;

	glimmer_active_module(&module, &settings);
	glimmer_module_setup(module, settings);
}


/* TODO: this should probably move into libtil */
static void * glimmer_thread(void *foo)
{
	struct timeval	now;

	for (;;) {
		til_fb_page_t	*page;
		unsigned	ticks;

		page = til_fb_page_get(glimmer.fb);
		gettimeofday(&now, NULL);
		ticks = glimmer_get_ticks(&glimmer.start_tv, &now, glimmer.ticks_offset);
		til_module_render(glimmer.module, glimmer.module_context, ticks, &page->fragment);
		til_fb_page_put(glimmer.fb, page);
	}
}


static void glimmer_go(GtkButton *button, gpointer user_data)
{
	til_settings_t	*settings;
	int		r;

	if (glimmer.fb) {
		pthread_cancel(glimmer.thread);
		pthread_join(glimmer.thread, NULL);
		til_quiesce();

		glimmer.fb = til_fb_free(glimmer.fb);
		glimmer.module_context = til_module_destroy_context(glimmer.module, glimmer.module_context);
	}

	/* TODO: prolly stop recreating fb on every go, or maybe only if the settings changed */
	r = til_fb_new(&gtk_fb_ops, glimmer.video_settings, NUM_FB_PAGES, &glimmer.fb);
	if (r < 0) {
		puts("fb no go!");
		return;
	}

	gettimeofday(&glimmer.start_tv, NULL);
	glimmer_active_module(&glimmer.module, &settings);
	r = til_module_create_context(
					glimmer.module,
					glimmer_get_ticks(
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


static void glimmer_combobox_setting_changed_cb(GtkComboBoxText *combobox, gpointer user_data)
{
	til_setting_t	*setting = user_data;

	/* XXX FIXME FIXME XXX */
	/* XXX FIXME FIXME XXX */
	/* I don't know gtk+ well enough to know what's the non-leaky way to do this:
	 * glimmer_active_module_setup() will destroy the module frame which encompasses the
	 * widget this signal emitted from.  It appears that there isn't a reference held across
	 * the signal callbacks so they can safely perform a queued destroy of the originating widget
	 * within the callback to then become realized at the end of all the signal deliveries and
	 * callback processing when the final reference gets removed.
	 *
	 * for now I'm working around this by simply adding a ref, leaking the memory, until I find
	 * the Right Way.
	 */
	g_object_ref(combobox);
	/* XXX FIXME FIXME XXX */
	/* XXX FIXME FIXME XXX */

	setting->value = gtk_combo_box_text_get_active_text(combobox);
	glimmer_active_module_setup();
}

static void glimmer_entry_setting_changed_cb(GtkEntry *entry, gpointer user_data)
{
	til_setting_t	*setting = user_data;

	/* XXX FIXME: see above comment for combobox, but oddly I'm only seeing
	 * errors printed for the combobox case.  I'm just assuming the problem exists here as well.
	 */
	g_object_ref(entry);

	/* FIXME TODO there needs to be some validation of the free-form input against setting->desc->regex,
	 * though that probably shouldn't happen here.
	 */
	setting->value = strdup(gtk_entry_get_text(entry));
	glimmer_active_module_setup();
}


/* (re)construct the gui settings pane to reflect *module and *settings */
static void glimmer_module_setup(const til_module_t *module, til_settings_t *settings)
{
	GtkWidget	*vbox, *label;

	if (glimmer.module_frame)
		gtk_widget_destroy(glimmer.module_frame);

	glimmer.module_frame = g_object_new(	GTK_TYPE_FRAME,
						"parent", GTK_CONTAINER(glimmer.module_box),
						"label", module->name,
						"label-xalign", .01f,
						"margin", FRAME_MARGIN,
						"visible", TRUE,
						NULL);
	gtk_box_set_child_packing(	GTK_BOX(glimmer.module_box),
					GTK_WIDGET(glimmer.module_frame),
					TRUE,
					FALSE,
					BOX_SPACING,
					GTK_PACK_START);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, BOX_SPACING);
	gtk_container_add(GTK_CONTAINER(glimmer.module_frame), vbox);

	label = g_object_new(	GTK_TYPE_LABEL,
				"parent", GTK_CONTAINER(vbox),
				"label", module->description,
				"halign", GTK_ALIGN_START,
				"margin", LABEL_MARGIN,
				"visible", TRUE,
				NULL);

	label = g_object_new(	GTK_TYPE_LABEL,
				"parent", GTK_CONTAINER(vbox),
				"label", module->author,
				"halign", GTK_ALIGN_START,
				"margin", LABEL_MARGIN,
				"visible", TRUE,
				NULL);

	if (module->setup) {
		GtkWidget			*frame, *svbox;
		til_setting_t			*setting;
		const til_setting_desc_t	*desc;

		frame = g_object_new(	GTK_TYPE_FRAME,
					"parent", GTK_CONTAINER(vbox),
					"label", "Settings",
					"label-xalign", .01f,
					"margin", FRAME_MARGIN,
					"visible", TRUE,
					NULL);
		gtk_box_set_child_packing(	GTK_BOX(vbox),
						GTK_WIDGET(frame),
						TRUE,
						TRUE,
						BOX_SPACING,
						GTK_PACK_START);

		svbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, BOX_SPACING);
		gtk_container_add(GTK_CONTAINER(frame), svbox);

		til_settings_reset_descs(settings);
		while (module->setup(settings, &setting, &desc) > 0) {
			GtkWidget	*shbox;

			if (!setting) {
				til_settings_add_value(settings, desc->key, desc->preferred, NULL);
				continue;
			}

			shbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
			gtk_container_add(GTK_CONTAINER(svbox), shbox);
			gtk_widget_set_halign(GTK_WIDGET(shbox), GTK_ALIGN_START);

			label = g_object_new(	GTK_TYPE_LABEL,
						"parent", GTK_CONTAINER(shbox),
						"label", desc->name,
						"halign", GTK_ALIGN_START,
						"margin", LABEL_MARGIN,
						"visible", TRUE,
						NULL);

			if (desc->values) {
				GtkWidget	*combobox;

				/* combo box */
				combobox = gtk_combo_box_text_new();
				gtk_container_add(GTK_CONTAINER(shbox), combobox);
				gtk_widget_set_halign(GTK_WIDGET(combobox), GTK_ALIGN_END);
				for (int i = 0; desc->values[i]; i++) {
					gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combobox), NULL, desc->values[i]);
					if (!strcmp(setting->value, desc->values[i]))
						gtk_combo_box_set_active(GTK_COMBO_BOX(combobox), i);
				}
				g_signal_connect(combobox, "changed", G_CALLBACK(glimmer_combobox_setting_changed_cb), setting);

			} else {
				GtkWidget	*entry;

				/* plain unstructured text input box */
				entry = gtk_entry_new();
				gtk_entry_set_text(GTK_ENTRY(entry), setting->value);
				gtk_container_add(GTK_CONTAINER(shbox), entry);
				gtk_widget_set_halign(GTK_WIDGET(entry), GTK_ALIGN_END);

				/* XXX FIXME */
				/* XXX FIXME */
				/* XXX FIXME */
				/* "activate" only occurs on hitting Enter in the GtkEntry.  So we'll miss
				 * edits that are visible but not Entered before hitting Go!  We likely need
				 * to catch more signals...
				 */
				/* XXX FIXME */
				/* XXX FIXME */
				/* XXX FIXME */

				g_signal_connect(entry, "activate", G_CALLBACK(glimmer_entry_setting_changed_cb), setting);
			}

			if (!setting->desc)
				setting->desc = desc;
		}
	}

	gtk_widget_show_all(glimmer.module_frame);
}


static void glimmer_module_changed_cb(GtkComboBox *box, G_GNUC_UNUSED gpointer user_data)
{
	glimmer_active_module_setup();
}


static void glimmer_activate(GtkApplication *app, gpointer user_data)
{
	GtkWidget	*window, *vbox, *button;

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "glimmer");
	gtk_window_set_default_size(GTK_WINDOW(window), DEFAULT_WIDTH, DEFAULT_HEIGHT);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, BOX_SPACING);
	gtk_container_add(GTK_CONTAINER(window), vbox);

	{ /* construct modules list combobox, associating a name, module, and settings per entry */
		const til_module_t	**modules;
		size_t			n_modules;
		const char		*module;
		GtkComboBox		*combobox;
		GtkListStore		*store;
		GtkCellRenderer		*text;

		til_get_modules(&modules, &n_modules);
		module = til_settings_get_key(glimmer.module_settings, 0, NULL);

		combobox = g_object_new(GTK_TYPE_COMBO_BOX, "visible", TRUE, NULL);
		store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_POINTER);
		for (size_t i = 0; i < n_modules; i++) {
			GtkTreeIter iter;

			gtk_list_store_append(store, &iter);
			gtk_list_store_set(	store, &iter,
						0, modules[i]->name,
						1, modules[i],
						2, (module && !strcmp(module, modules[i]->name)) ? glimmer.module_settings : til_settings_new(NULL),
						-1);
		}

		gtk_combo_box_set_model(combobox, GTK_TREE_MODEL(store));
		gtk_combo_box_set_id_column(combobox, 0);
		gtk_combo_box_set_active_id(combobox, module ? : DEFAULT_MODULE);

		g_signal_connect(combobox, "changed", G_CALLBACK(glimmer_module_changed_cb), NULL);

		text = gtk_cell_renderer_text_new();
		gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combobox), text, TRUE);
		gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(combobox), text, "text", 0);

		glimmer.modules_combobox = combobox;
	}

	gtk_container_add(GTK_CONTAINER(vbox), GTK_WIDGET(glimmer.modules_combobox));
	gtk_box_set_child_packing(	GTK_BOX(vbox),
					GTK_WIDGET(glimmer.modules_combobox),
					FALSE,
					FALSE,
					BOX_SPACING * 4,
					GTK_PACK_START);

	glimmer.module_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
	gtk_container_add(GTK_CONTAINER(vbox), glimmer.module_box);
	gtk_box_set_child_packing(	GTK_BOX(vbox),
					glimmer.module_box,
					TRUE,
					TRUE,
					BOX_SPACING,
					GTK_PACK_START);

	glimmer_active_module_setup();

	/* button to rototill as configured */
	button = g_object_new(	GTK_TYPE_BUTTON,
				"parent", GTK_CONTAINER(vbox),
				"label", "Go!",
				"visible", TRUE,
				NULL);
	g_signal_connect(button, "clicked", G_CALLBACK(glimmer_go), NULL);

	gtk_widget_show_all(window);
}


int main(int argc, const char *argv[])
{
	int		r, status, pruned_argc;
	const char	**pruned_argv;
	GtkApplication	*app;

	til_init();

	r = til_args_pruned_parse(argc, argv, &glimmer.args, &pruned_argc, &pruned_argv);
	if (r < 0) {
		fprintf(stderr, "Unable to parse args: %s\n", strerror(-r));
		return EXIT_FAILURE;
	}

	glimmer.module_settings = til_settings_new(glimmer.args.module);
	/* TODO: glimmer doesn't currently handle video settings, gtk_fb doesn't even
	 * implement a .setup() method.  It would be an interesting exercise to bring
	 * in support for rototiller's sdl and drm fb backends, but it immediately
	 * becomes awkward with obvious conflicts like drm wanting to own the display
	 * implicitly being shared when glimmer's already using gtk if not on distinct
	 * devices.
	 *
	 * But it'd be nice to at least support window sizing/fullscreen
	 * startup via args w/gtk_fb, so at some point I should add a
	 * gtk_fb.setup() method for filling in the blanks of what the args omit.
	 * For now these statically defined comprehensive settings simply skirt
	 * the issue.
	 */
	//glimmer.video_settings = til_settings_new(glimmer.args.video);
	glimmer.video_settings = til_settings_new("fullscreen=off,size=640x480");

	app = gtk_application_new("com.pengaru.glimmer", G_APPLICATION_FLAGS_NONE);
	g_signal_connect(app, "activate", G_CALLBACK(glimmer_activate), NULL);
	status = g_application_run(G_APPLICATION(app), pruned_argc, (char **)pruned_argv);
	g_object_unref(app);

	til_shutdown();

	return status;
}
