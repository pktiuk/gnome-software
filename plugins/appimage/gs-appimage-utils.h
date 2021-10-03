#pragma once

#include <appimage/appimage.h>
#include <gnome-software.h>

/**
 * @brief Length of prefix of AppImage desktop files
 * example:
 * appimagekit_f61d8209e0838b3a9e4ee664ea1e4ffd-io.github.antimicrox.antimicrox.desktop
 *
 */
#define APPIMAGE_NAME_PREFIX_LEN 45

#define META_KEY_APPIMAGE_ID "appimage:registered_desktop_name"

/**
 * @brief Generate basic AppImage app
 *
 * @param appimage_id - desktop-id of AppImage when integrated, can be NULL
 * (example:
 * appimagekit_f61d8209e0838b3a9e4ee664ea1e4ffd-io.github.antimicrox.antimicrox.desktop)
 * @return GsApp*
 */
GsApp *gs_app_new_appimage (const gchar *appimage_id);

gboolean load_from_desktop_file (GsApp *app,
				 gchar *desktop_file_path,
				 GError **error,
				 gboolean is_installed);

gchar *get_id_from_desktop_filename (gchar *desktop_file_path);

/**
 * @brief refine GsApp using data from AppImage file
 *
 * @param app
 * @param error
 * @param appimage_file_path - path to AppImage file
 * @return gboolean
 */
gboolean
refine_appimage_file (GsApp *app, GError **error, gchar *file_appimage_path);
