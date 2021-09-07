#pragma once

#include <gnome-software.h>

gboolean load_from_desktop_file (GsApp *app,
				 gchar *desktop_file_path,
				 GError **error,
				 gboolean is_installed);