#include "gs-appimage-utils.h"

#include <sys/stat.h>

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

gboolean
refine_appimage_file (GsApp *app, GError **error, gchar *file_appimage_path)
{
	g_debug ("Refining app using data from AppImage file");
	if (file_appimage_path == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "No path to AppImage file is set");
		return FALSE;
	}

	/* Extract the desktop file from the AppImage */
	char **files = appimage_list_files (file_appimage_path);
	gchar *desktop_file = NULL;
	gchar *extracted_desktop_file =
		"/tmp/gs-plugin-appimage/application.desktop";
	for (int i = 0; files[i] != NULL; i++) {
		// g_debug("AppImage file: %s", files[i]);
		if (g_str_has_suffix (files[i], ".desktop")) {
			desktop_file = files[i];
			g_debug ("AppImage desktop file: %s", desktop_file);
			break;
		}
	}
	/* Exit if we cannot find the desktop file */
	if (desktop_file == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "AppImage desktop file not found");
		appimage_string_list_free (files);
		return FALSE;
	}
	/* Extract desktop file to temporary location */
	appimage_extract_file_following_symlinks (
		file_appimage_path, desktop_file, extracted_desktop_file);


	load_from_desktop_file (
		app,
		extracted_desktop_file,
		error,
		appimage_is_registered_in_system (file_appimage_path));

	g_autofree gchar *md5 = appimage_get_md5 (file_appimage_path);

	gchar *extracted_appstream_file =
		"/tmp/gs-plugin-appimage/application.metainfo.xml";
	g_debug ("AppImage AppStream file to be extracted to: %s",
		 extracted_appstream_file);

	// Extract the AppStream file from the AppImage
	gchar *appstream_file = NULL;

	for (int i = 0; files[i] != NULL; i++) {
		// g_debug ("AppImage file: %s", files[i]);
		if (g_str_has_suffix (files[i], ".metainfo.xml")
		    || g_str_has_suffix (files[i], ".appdata.xml")) {
			appstream_file = files[i];
			g_debug ("AppImage AppStream file: %s", appstream_file);
			break;
		}
	}

	if (appstream_file == NULL) {
		g_debug (
			"AppImage AppStream file not found in AppImage; hence not extracting");
	} else {
		// Extract AppStream file
		g_debug ("AppImage extracting AppStream file");
		appimage_extract_file_following_symlinks (
			file_appimage_path,
			appstream_file,
			extracted_appstream_file);
	}

	gs_app_set_scope (app, AS_COMPONENT_SCOPE_USER);
	gs_app_set_kind (app, AS_COMPONENT_KIND_DESKTOP_APP);
	gs_app_set_local_file (
		app, g_file_new_build_filename (file_appimage_path, NULL));
	// gs_app_add_quirk (app, GS_APP_QUIRK_PROVENANCE); // QUESTION: How to
	// mark "3rd party"? hughsie: PROVENANCE usually means the opposite,
	// e.g. it's from the distro gs_app_set_state (app,
	// GS_APP_STATE_AVAILABLE_LOCAL);


	/* Get the size of the AppImage on disk */
	struct stat st;
	if (lstat (file_appimage_path, &st) == -1) {
		g_debug ("Could not determine AppImage file size");
		return FALSE;
	}
	gs_app_set_size_installed(app, GS_SIZE_TYPE_VALID, st.st_size);
	gs_app_set_size_download(app, GS_SIZE_TYPE_VALID, st.st_size);

	g_autofree gchar *partial_path =
		g_strdup_printf ("applications/appimagekit_%s-%s",
				 md5,
				 g_path_get_basename (desktop_file));

	gs_app_set_metadata (app, META_KEY_APPIMAGE_ID, partial_path + 13);
	g_autofree gchar *appimage_integrated_desktop_file_path =
		g_build_filename (g_get_user_data_dir(), partial_path, NULL);
	g_autofree gchar *appimage_id =
		get_id_from_desktop_filename (partial_path);
	gs_app_set_id (app,
		       appimage_id); // This makes it
				     // use the desktop
				     // file and icon
				     // already
				     // integrated into
				     // the system

	/* Check if this AppImage is already integrated in the system in which
	 * case we treat it as "installed */
	if (appimage_is_registered_in_system (file_appimage_path)) {
		g_debug ("AppImage is already integrated at %s",
			 appimage_integrated_desktop_file_path);
		gs_app_set_state (app, GS_APP_STATE_INSTALLED);
	} else {
		g_debug ("AppImage is not integrated yet");
		gs_app_set_state (app, GS_APP_STATE_AVAILABLE_LOCAL);
	}

	appimage_string_list_free (files);
	return TRUE;
}

gboolean refine_installed_app (GsApp *app, GError **error)
{
	g_debug ("Refining registered AppImage app");
	gchar *desktop_id =
		gs_app_get_metadata_item (app, META_KEY_APPIMAGE_ID);
	if (desktop_id == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Missing %s metadata item in GsApp object",
			     META_KEY_APPIMAGE_ID);
		return FALSE;
	}

	g_autofree gchar *desktop_file_path =
		g_strdup_printf ("%s%s%s",
				 g_get_user_data_dir(),
				 "/applications/",
				 desktop_id,
				 NULL);

	g_autofree gchar *app_id = get_id_from_desktop_filename (desktop_id);

	gs_app_set_id (app, app_id);

	gs_app_set_launchable (app, AS_LAUNCHABLE_KIND_DESKTOP_ID, desktop_id);
	gs_app_set_state (app, GS_APP_STATE_INSTALLED);

	load_from_desktop_file (app, desktop_file_path, error, TRUE);

	return TRUE;
}
