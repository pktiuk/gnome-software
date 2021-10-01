#pragma once

#include <gnome-software.h>

/**
 * @brief Length of prefix of AppImage desktop files
 * example:
 * appimagekit_f61d8209e0838b3a9e4ee664ea1e4ffd-io.github.antimicrox.antimicrox.desktop
 *
 */
#define APPIMAGE_NAME_PREFIX_LEN 45

gboolean load_from_desktop_file (GsApp *app,
				 gchar *desktop_file_path,
				 GError **error,
				 gboolean is_installed);

gchar *get_id_from_desktop_filename (gchar *desktop_file_path);