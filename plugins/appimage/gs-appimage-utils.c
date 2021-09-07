#include "gs-appimage-utils.h"

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
	} else {
		g_autoptr (GIcon) g_icon =
			g_themed_icon_new ("application-x-executable");
		gs_app_add_icon (app, g_icon);
	}
	g_key_file_free (key_file_structure);

	gs_app_set_management_plugin (app, "appimage");
	gs_app_set_kind (app, AS_COMPONENT_KIND_DESKTOP_APP);
	gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_APPIMAGE);

	return success;
}