#include "gs-appimage-utils.h"

GsApp *gs_app_new_appimage (const gchar *appimage_id)
{
	GsApp *app = NULL;
	if (appimage_id) {
		app = gs_app_new (get_id_from_desktop_filename (appimage_id));
		gs_app_set_metadata (app, META_KEY_APPIMAGE_ID, appimage_id);
	} else {
		app = gs_app_new ("NULL");
	}
	gs_app_set_management_plugin (app, "appimage");
	gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_APPIMAGE);
	gs_app_set_origin (app, "AppImage");

	return app;
}

gboolean load_from_desktop_file (GsApp *app,
				 gchar *desktop_file_path,
				 GError **error,
				 gboolean is_installed)
{
	gboolean success = FALSE;
	GKeyFile *key_file_structure = g_key_file_new();
	g_debug ("Loading AppImage desktop file from %s", desktop_file_path);
	success = g_key_file_load_from_file (
		key_file_structure,
		desktop_file_path,
		G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
		NULL);
	if (!success) {
		g_debug ("AppImage desktop file could not be loaded");
		return FALSE;
	} else {
		g_debug ("Loaded AppImage desktop file"); // QUESTION: Can we
							  // directly
							  // gs_app_new() from
							  // this? hughsie: use
							  // appstream-glib
	}

	gs_app_set_name (app,
			 GS_APP_QUALITY_NORMAL,
			 g_key_file_get_value (key_file_structure,
					       G_KEY_FILE_DESKTOP_GROUP,
					       G_KEY_FILE_DESKTOP_KEY_NAME,
					       error));
	gs_app_set_summary (
		app,
		GS_APP_QUALITY_NORMAL,
		g_key_file_get_value (key_file_structure,
				      G_KEY_FILE_DESKTOP_GROUP,
				      G_KEY_FILE_DESKTOP_KEY_COMMENT,
				      error));
	gs_app_set_description (
		app,
		GS_APP_QUALITY_NORMAL,
		g_key_file_get_value (key_file_structure,
				      G_KEY_FILE_DESKTOP_GROUP,
				      G_KEY_FILE_DESKTOP_KEY_COMMENT,
				      error));

	/* these are all optional, but make details page look better */
	gs_app_set_version (app,
			    g_key_file_get_value (key_file_structure,
						  G_KEY_FILE_DESKTOP_GROUP,
						  "X-AppImage-Version",
						  NULL));
	if (is_installed) {
		g_autoptr (GIcon) g_icon = g_themed_icon_new (
			g_key_file_get_value (key_file_structure,
					      G_KEY_FILE_DESKTOP_GROUP,
					      G_KEY_FILE_DESKTOP_KEY_ICON,
					      error));
		gs_app_add_icon (app, g_icon);
		// Get executable without arguments
		g_auto (GStrv) split = g_strsplit (
			g_key_file_get_value (key_file_structure,
					      G_KEY_FILE_DESKTOP_GROUP,
					      G_KEY_FILE_DESKTOP_KEY_EXEC,
					      error),
			" ",
			-1);
		gchar *appimage_filename = split[0];
		GFile *appimage_file =
			g_file_new_build_filename (appimage_filename, NULL);

		gs_app_set_local_file (app, appimage_file);
	} else {
		g_autoptr (GIcon) g_icon =
			g_themed_icon_new ("application-x-executable");
		gs_app_add_icon (app, g_icon);
	}
	g_key_file_free (key_file_structure);

	gs_app_set_kind (app, AS_COMPONENT_KIND_DESKTOP_APP);

	return success;
}
gchar *get_id_from_desktop_filename (gchar *desktop_file_path)
{
	g_autofree gchar *basename = g_path_get_basename (desktop_file_path);
	g_autoptr (GString) appimage_id = g_string_new (basename);
	appimage_id = g_string_truncate (
		appimage_id,
		appimage_id->len - 8); // remove .desktop from the end
	if (g_str_has_prefix (appimage_id->str, "appimagekit_"))
		appimage_id = g_string_erase (
			appimage_id, 0, APPIMAGE_NAME_PREFIX_LEN);

	return g_strdup (appimage_id->str);
}
