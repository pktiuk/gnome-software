/* GNOME Software AppImage plugin
 * Licensing to be determined (MIT like AppImageKit or GPLv2 like GNOME
 * Software)
 */

#include <appimage/appimage.h> // From https://github.com/AppImage/AppImageKit
#include <gnome-software.h>

#include "gs-appimage-utils.h"

void gs_plugin_initialize (GsPlugin *plugin)
{
	g_debug ("AppImage gs_plugin_initialize");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "appstream");

	/* prioritize over packages */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_BETTER_THAN, "packagekit");
}

/* Claim AppImages that should be handled by this plugin.
 */
void gs_plugin_adopt_app (GsPlugin *plugin, GsApp *app)
{
	if (gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_APPIMAGE)
		gs_app_set_management_plugin (app, gs_plugin_get_name (plugin));
}

gboolean gs_plugin_launch (GsPlugin *plugin,
			   GsApp *app,
			   GCancellable *cancellable,
			   GError **error)
{

	g_debug ("Launching app");
	if (!gs_app_has_management_plugin(app, plugin))
		return TRUE;
	g_debug ("Launching gs_plugin_app_launch");
	return gs_plugin_app_launch (plugin, app, error);
}

gboolean gs_plugin_app_install (GsPlugin *plugin,
				GsApp *app,
				GCancellable *cancellable,
				GError **error)
{
	/* only process this app if was created by this plugin */
	if (!gs_app_has_management_plugin(app, plugin))
		return TRUE;

	const gchar *install_package = NULL;
	install_package = gs_app_get_source_default (app);
	g_debug ("Installing file: %s", install_package);
	if (install_package == NULL) {
		return FALSE;
	}

	gs_app_set_state (app, GS_APP_STATE_INSTALLING);
	int result = appimage_register_in_system (install_package, false);
	if (result != 0) {
		g_debug ("registering failed %d", result);
		gs_app_set_state_recover (app);
		return FALSE;
	}
	gs_app_set_state (app, GS_APP_STATE_PENDING_INSTALL);

	return TRUE;
}

gboolean gs_plugin_app_remove (GsPlugin *plugin,
			       GsApp *app,
			       GCancellable *cancellable,
			       GError **error)
{
	GFile *appimage_file = gs_app_get_local_file (app);
	g_debug ("Unregistering appimage file: %s",
		 g_file_get_path (appimage_file));
	if (appimage_file == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Missing AppImage file");
		return FALSE;
	}
	gs_app_set_state (app, GS_APP_STATE_REMOVING);
	int result = appimage_unregister_in_system (
		g_file_get_path (appimage_file), false);
	if (result != 0) {
		gs_app_set_state_recover (app);
		return FALSE;
	}
	gs_app_set_state (app, GS_APP_STATE_UNAVAILABLE);
	return TRUE;
}

/*
 * Handle AppImages "opened" with GNOME Software
 * This works; it does show the product detail page when launched like this:
 * XDG_DATA_DIRS=install/share:$XDG_DATA_DIRS ../install/bin/gnome-software
 * --verbose
 * --local-filename=/isodevice/Applications/XChat-2.8.8-x86_64.AppImage 2>&1 |
 * grep AppImage
 *
 */

gboolean gs_plugin_file_to_app (GsPlugin *plugin,
				GsAppList *list,
				GFile *file,
				GCancellable *cancellable,
				GError **error)
{
	g_autofree char *file_appimage_path = g_file_get_path (file);
	g_debug ("AppImage gs_plugin_url_to_app");
	g_debug ("file: %s", file_appimage_path);

	int appimage_type = appimage_get_type (file_appimage_path, FALSE);
	g_debug ("AppImage type: %i", appimage_type);

	/* Error if we cannot determine the type of the AppImage */
	if (appimage_type < 0) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Invalid AppImage type: %d",
			     appimage_type);
		g_debug ("AppImage type");
		return FALSE;
	}

	g_autoptr (GsApp) app = NULL;

	if (appimage_is_registered_in_system (file_appimage_path)) {
		g_autofree gchar *md5 = appimage_get_md5 (file_appimage_path);
		g_autofree gchar *desktop_file_path =
			appimage_registered_desktop_file_path (
				file_appimage_path, md5, false);
		g_debug ("desktop_id: %s",
			 g_path_get_basename (desktop_file_path));
		app = gs_app_new_appimage (
			g_path_get_basename (desktop_file_path));
		if (!refine_installed_app (app, error))
			return FALSE;
	} else {
		app = gs_app_new_appimage (
			NULL); // NOTE: We set the ID down below, including the
			       // md5 from appimage_get_md5. hughsie recommends
			       // reverse DNS, and it should match the desktop
			       // file and the id in the AppStream XML
		if (!refine_appimage_file (app, error, file_appimage_path))
			return FALSE;
	}


	/* TODO: AppStream
	 *    Save an AppStream-format XML file in either
	 * /usr/share/app-info/xmls/, /var/cache/app-info/xmls/ or
	 * ~/.local/share/app-info/xmls/. GNOME Software will immediately notice
	 * any new files, or changes to existing files as it has set up the
	 * various inotify watches.
	 *
	 *    QUESTION: Do we need to copy XML there for AppImages that are not
	 * integrated into the system? Can we load in data from an xml file
	 * which we delete immediately afterwards? Because we don't want to
	 * litter the system with XML files. hughsie: Don't need to copy; just
	 * use the helpers in gs-appstream.c; e.g. the Flatpak plugin loads the
	 * AppStream xml from a nonstandard path
	 *
	 *    QUESTION: If we need to ensure that our AppStream information is
	 * not mixed with AppStream information from other locations or from
	 * version to version of an application, is it OK to place it in
	 *    /home/me/.local/share/app-info/xmls/appimagekit_98080cfc981c9098c6a5e41794add640-deepin-screenshot.appdata.xml
	 *    hughsie: the filename is almost unimportant, it's the <id> that
	 * has to match
	 *
	 *    QUESTION: If we have
	 * appimagekit_98080cfc981c9098c6a5e41794add640-deepin-screenshot.desktop
	 * in one of the well-known locations for desktop files, will it
	 * automatically be associated with the AppStream metadata from above?
	 * hughsie: the filename is almost unimportant, it's the <id> that has
	 * to match
	 *
	 *    QUESTION: Is the AppStream ID the same as the gs ID?
	 *    hughsie: In most cases, yes. The appstream file can also have a
	 * <launchable> tab pointing to the desktop file. If there's no
	 * launchable then we use a heuristic to try and create one
	 *
	 *    QUESTION: Is there a screenshot where i could see the effect of
	 * using different IDs for gs_app_new() vs. gs_app_set_branch()?
	 * hughsie: Not really, but you can use gnome-software-cmd like this:
	 * /usr/libexec/gnome-software-cmd search --refine-flags=icon
	 * --show-results
	 *
	 *    I see:
	 *    08:20:32:0181 Gs  searching appstream for user / * / * / desktop /
	 * appimagekit_98080cfc981c9098c6a5e41794add640-deepin-screenshot.desktop
	 * / *
	 *
	 *    QUESTION: Should we have appimaged (also) integrate AppStream
	 * files to
	 * ~/.local/share/app-info/xmls/? hughsie: I think using reverse DNS
	 * style IDs everywhere is a very good idea; I also think it's too early
	 * to optimise anything
	 */

	g_debug ("AppImage gs_app_is_installed: %i", gs_app_is_installed (app));
	if (!gs_app_is_installed (app)) {
		gs_app_add_source_id (app, "AppImage");
		gs_app_add_source (app, g_file_get_path (file));
	}

	// QUESTION: "Install" doesn't really cut it. For AppImages, we would
	// want "Move to /Applications", "Move to $HOME/Applications", etc. Is
	// there a way to cusomize this? hughsie: In gnome-software they'd be
	// different "scope" and therefore different GsApp others want the same
	// kind of feature, so it's probably something we ought to support

	/* return new app */
	gs_app_list_add (list, app);

	return TRUE;
}

gboolean gs_plugin_add_installed (GsPlugin *plugin,
				  GsAppList *list,
				  GCancellable *cancellable,
				  GError **error)
{
	g_debug ("AppImage gs_plugin_add_installed");

	g_autofree gchar *searched_path =
		g_build_filename (g_get_user_data_dir(), "applications", NULL);
	g_debug ("AppImage gs_plugin_add_installed");
	g_autoptr (GDir) dir = g_dir_open (searched_path, 0, &error);
	g_autofree gchar *filename;

	g_debug ("AppImage gs_plugin_add_installed");
	while ((filename = g_dir_read_name (dir))) {
		g_debug ("File: %s", filename);
		if (g_str_has_prefix (filename, "appimagekit_")
		    && g_str_has_suffix (filename, ".desktop")) {

			g_autofree gchar *file_path = g_build_filename (
				searched_path, filename, NULL);

			g_autoptr (GsApp) app = gs_app_new_appimage (filename);

			g_debug ("Figs_app_new filename: %s", filename);
			refine_installed_app (app, error);
			gs_app_set_scope (
				app,
				AS_COMPONENT_SCOPE_USER); // TODO: Distinguish
							  // system-wide
							  // AppImages. Those
							  // which are read-only
							  // for the current
							  // user?

			gs_app_list_add (list, app);
		}
	}
	return TRUE;
}

/*
 * QUESTION: Can plugins have their own settings, and where would the GUI for
 * these be? hughsie: There is a gsetting key to be shared by plugins for this
 *
 * QUESTION: How do I populate the list of installable apps?
 * hughsie: You just return the GsApps when the frontend wants results;
 * typically that'll be add_popular, add_featured or search()
 *
 * QUESTION: I assume it uses the XDG desktop categories?
 * hughsie: It does; just make sure it has the right "categories"
 *
 * QUESTION: How to make all of https://appimage.github.io/feed.json
 * show up in gs in the respective categories?
 * hughsie: Is that data available as appstream xml too? That's the easiest way;
 * otherwise you have to handle the add_categories()
 * and add_categories_ap()
 *
 * QUESTION: do you have a minimal viable example of the kind of distro
 * appstream xml needed that you were referring to? hughsie: yaml is supported,
 * but it's had nowhere near the memory optimisation work it's the same XML as
 * an appdata file, wraped up in <components>
 *
 * Just return them as results for the various query methods
 * e.g. we never show "all" the apps that can be installed
 *
 * What most plugins do is use gs_plugin_search() and then if they need some
 * shared resource call into some plugin_specific_ensure() thing to open store,
 * parse xml etc., e.g. not do it in gs_plugin_setup() if it's going to take
 * time
 *
 * QUESTION: How to handle multiple versions properly?
 */
