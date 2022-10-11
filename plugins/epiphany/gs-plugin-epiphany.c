/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2021-2022 Matthew Leeds <mwleeds@protonmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <config.h>
#include <glib/gi18n.h>
#include <gnome-software.h>
#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>

#include "gs-epiphany-generated.h"
#include "gs-plugin-epiphany.h"
#include "gs-plugin-private.h"

/*
 * SECTION:
 * This plugin uses Epiphany to install, launch, and uninstall web applications.
 *
 * If the org.gnome.Epiphany.WebAppProvider D-Bus interface is not present or
 * the DynamicLauncher portal is not available then it self-disables. This
 * should work with both Flatpak'd and traditionally packaged Epiphany, for new
 * enough versions of Epiphany.
 *
 * It's worth noting that this plugin has to deal with two different app IDs
 * for installed web apps:
 *
 * 1. The app ID used in the <id> element in the AppStream metainfo file, which
 *    looks like "org.gnome.Software.WebApp_527a2dd6729c3574227c145bbc447997f0048537.desktop"
 *    See https://gitlab.gnome.org/mwleeds/gnome-pwa-list/-/blob/6e8b17b018f99dbf00b1fa956ed75c4a0ccbf389/pwa-metainfo-generator.py#L84-89
 *    This app ID is used for gs_app_new() so that the appstream plugin
 *    refines the apps created here, and used for the plugin cache.
 *
 * 2. The app ID generated by Epiphany when installing a web app, which looks
 *    like "org.gnome.Epiphany.WebApp_e9d0e1e4b0a10856aa3b38d9eb4375de4070d043.desktop"
 *    though it can have a different prefix if Epiphany was built with, for
 *    example, a development profile. Throughout this plugin this type of app
 *    ID is handled with a variable called "installed_app_id". This app ID is
 *    used in method calls to the org.gnome.Epiphany.WebAppProvider interface,
 *    and used for gs_app_set_launchable() and g_desktop_app_info_new().
 *
 * Since: 43
 */

struct _GsPluginEpiphany
{
	GsPlugin parent;

	GsWorkerThread *worker;  /* (owned) */

	GsEphyWebAppProvider *epiphany_proxy;  /* (owned) */
	GDBusProxy *launcher_portal_proxy;  /* (owned) */
	GFileMonitor *monitor; /* (owned) */
	guint changed_id;
	/* protects installed_apps_cached, url_id_map, and the plugin cache */
	GMutex installed_apps_mutex;
	/* installed_apps_cached: whether the plugin cache has all installed apps */
	gboolean installed_apps_cached;
	GHashTable *url_id_map; /* (owned) (not nullable) (element-type utf8 utf8) */

	/* default permissions, shared between all applications */
	GsAppPermissions *permissions; /* (owned) (not nullable) */
};

G_DEFINE_TYPE (GsPluginEpiphany, gs_plugin_epiphany, GS_TYPE_PLUGIN)

#define assert_in_worker(self) \
	g_assert (gs_worker_thread_is_in_worker_context (self->worker))

static void
gs_epiphany_error_convert (GError **perror)
{
	GError *error = perror != NULL ? *perror : NULL;

	/* not set */
	if (error == NULL)
		return;

	/* parse remote epiphany-webapp-provider error */
	if (g_dbus_error_is_remote_error (error)) {
		g_autofree gchar *remote_error = g_dbus_error_get_remote_error (error);

		g_dbus_error_strip_remote_error (error);

		if (g_str_equal (remote_error, "org.freedesktop.DBus.Error.ServiceUnknown")) {
			error->code = GS_PLUGIN_ERROR_NOT_SUPPORTED;
		} else if (g_str_has_prefix (remote_error, "org.gnome.Epiphany.WebAppProvider.Error")) {
			error->code = GS_PLUGIN_ERROR_FAILED;
		} else {
			g_warning ("Can’t reliably fixup remote error ‘%s’", remote_error);
			error->code = GS_PLUGIN_ERROR_FAILED;
		}
		error->domain = GS_PLUGIN_ERROR;
		return;
	}

	/* this is allowed for low-level errors */
	if (gs_utils_error_convert_gio (perror))
		return;

	/* this is allowed for low-level errors */
	if (gs_utils_error_convert_gdbus (perror))
		return;
}

/* Run in the main thread. */
static void
gs_plugin_epiphany_changed_cb (GFileMonitor      *monitor,
                               GFile             *file,
                               GFile             *other_file,
                               GFileMonitorEvent  event_type,
                               gpointer           user_data)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (user_data);

	{
	  g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->installed_apps_mutex);
	  gs_plugin_cache_invalidate (GS_PLUGIN (self));
	  g_hash_table_remove_all (self->url_id_map);
	  self->installed_apps_cached = FALSE;
	}

	/* FIXME: With the current API this is the only way to reload the list
	 * of installed apps.
	 */
	gs_plugin_reload (GS_PLUGIN (self));
}

static void
epiphany_web_app_provider_proxy_created_cb (GObject      *source_object,
                                            GAsyncResult *result,
                                            gpointer      user_data);

static void
dynamic_launcher_portal_proxy_created_cb (GObject      *source_object,
                                          GAsyncResult *result,
                                          gpointer      user_data);

static void
gs_plugin_epiphany_setup_async (GsPlugin            *plugin,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (plugin);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree char *portal_apps_path = NULL;
	g_autoptr(GFile) portal_apps_file = NULL;
	GDBusConnection *connection;

	task = g_task_new (plugin, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_epiphany_setup_async);

	g_debug ("%s", G_STRFUNC);

	self->installed_apps_cached = FALSE;

	/* This is a mapping from URL to app ID, where the app ID comes from
	 * Epiphany. This allows us to use that app ID rather than the
	 * AppStream app ID in certain contexts (see the comment at the top of
	 * this file).
	 */
	self->url_id_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	/* Watch for changes to the set of installed apps in the main thread.
	 * This will also trigger when other apps' dynamic launchers are
	 * installed or removed but that is expected to be infrequent.
	 */
	portal_apps_path = g_build_filename (g_get_user_data_dir (), "xdg-desktop-portal", "applications", NULL);
	portal_apps_file = g_file_new_for_path (portal_apps_path);
	/* Monitoring the directory works even if it doesn't exist yet */
	self->monitor = g_file_monitor_directory (portal_apps_file, G_FILE_MONITOR_WATCH_MOVES,
			                          cancellable, &local_error);
	if (self->monitor == NULL) {
		gs_epiphany_error_convert (&local_error);
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	self->changed_id = g_signal_connect (self->monitor, "changed",
					     G_CALLBACK (gs_plugin_epiphany_changed_cb), self);

	connection = gs_plugin_get_session_bus_connection (GS_PLUGIN (self));
	g_assert (connection != NULL);

	gs_ephy_web_app_provider_proxy_new (connection,
					    G_DBUS_PROXY_FLAGS_NONE,
					    "org.gnome.Epiphany.WebAppProvider",
					    "/org/gnome/Epiphany/WebAppProvider",
					    cancellable,
					    epiphany_web_app_provider_proxy_created_cb,
					    g_steal_pointer (&task));
}

static void
epiphany_web_app_provider_proxy_created_cb (GObject      *source_object,
                                            GAsyncResult *result,
                                            gpointer      user_data)
{
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autofree gchar *name_owner = NULL;
	g_autoptr(GError) local_error = NULL;
	GsPluginEpiphany *self = g_task_get_source_object (task);
	GDBusConnection *connection;
	GCancellable *cancellable;

	/* Check that the proxy exists (and is owned; it should auto-start) so
	 * we can disable the plugin for systems which don’t have new enough
	 * Epiphany.
	 */
	self->epiphany_proxy = gs_ephy_web_app_provider_proxy_new_finish (result, &local_error);

	if (self->epiphany_proxy == NULL) {
		gs_epiphany_error_convert (&local_error);
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->epiphany_proxy));
	if (name_owner == NULL) {
		g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NOT_SUPPORTED,
					 "Couldn’t create Epiphany WebAppProvider proxy: couldn’t get name owner");
		return;
	}

	connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (self->epiphany_proxy));
	cancellable = g_task_get_cancellable (task);

	g_dbus_proxy_new (connection,
			  G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
			  NULL,
			  "org.freedesktop.portal.Desktop",
			  "/org/freedesktop/portal/desktop",
			  "org.freedesktop.portal.DynamicLauncher",
			  cancellable,
			  dynamic_launcher_portal_proxy_created_cb,
			  g_steal_pointer (&task));
}

static void
dynamic_launcher_portal_proxy_created_cb (GObject      *source_object,
                                          GAsyncResult *result,
                                          gpointer      user_data)
{
	g_autoptr(GTask) task = G_TASK (user_data);
	g_autoptr(GVariant) version = NULL;
	g_autoptr(GError) local_error = NULL;
	GsPluginEpiphany *self = g_task_get_source_object (task);

	/* Check that the proxy exists (and is owned; it should auto-start) so
	 * we can disable the plugin for systems which don’t have new enough
	 * Epiphany.
	 */
	self->launcher_portal_proxy = g_dbus_proxy_new_finish (result, &local_error);

	if (self->launcher_portal_proxy == NULL) {
		gs_epiphany_error_convert (&local_error);
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	version = g_dbus_proxy_get_cached_property (self->launcher_portal_proxy, "version");
	if (version == NULL) {
		g_task_return_new_error (task, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NOT_SUPPORTED,
					 "Dynamic launcher portal not available");
		return;
	} else {
		g_debug ("Found version %" G_GUINT32_FORMAT " of the dynamic launcher portal",
			 g_variant_get_uint32 (version));
	}

	/* Start up a worker thread to process all the plugin’s function calls. */
	self->worker = gs_worker_thread_new ("gs-plugin-epiphany");

	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_epiphany_setup_finish (GsPlugin      *plugin,
                                 GAsyncResult  *result,
                                 GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void shutdown_cb (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data);

static void
gs_plugin_epiphany_shutdown_async (GsPlugin            *plugin,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (plugin);
	g_autoptr(GTask) task = NULL;

	task = g_task_new (self, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_epiphany_shutdown_async);

	/* Stop the worker thread. */
	gs_worker_thread_shutdown_async (self->worker, cancellable, shutdown_cb, g_steal_pointer (&task));
}

static void
shutdown_cb (GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
	g_autoptr(GTask) task = G_TASK (user_data);
	GsPluginEpiphany *self = g_task_get_source_object (task);
	g_autoptr(GsWorkerThread) worker = NULL;
	g_autoptr(GError) local_error = NULL;

	worker = g_steal_pointer (&self->worker);

	if (!gs_worker_thread_shutdown_finish (worker, result, &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_epiphany_shutdown_finish (GsPlugin      *plugin,
                                    GAsyncResult  *result,
                                    GError       **error)
{
	return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_epiphany_init (GsPluginEpiphany *self)
{
	/* Re-used permissions by all GsApp instances; do not modify it out
	   of this place. */
	self->permissions = gs_app_permissions_new ();
	gs_app_permissions_set_flags (self->permissions, GS_APP_PERMISSIONS_FLAGS_NETWORK);
	gs_app_permissions_seal (self->permissions);

	/* set name of MetaInfo file */
	gs_plugin_set_appstream_id (GS_PLUGIN (self), "org.gnome.Software.Plugin.Epiphany");

	/* need help from appstream */
	gs_plugin_add_rule (GS_PLUGIN (self), GS_PLUGIN_RULE_RUN_AFTER, "appstream");

	/* prioritize over packages */
	gs_plugin_add_rule (GS_PLUGIN (self), GS_PLUGIN_RULE_BETTER_THAN, "packagekit");
}

static void
gs_plugin_epiphany_dispose (GObject *object)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (object);

	if (self->changed_id > 0) {
		g_signal_handler_disconnect (self->monitor, self->changed_id);
		self->changed_id = 0;
	}

	g_clear_object (&self->epiphany_proxy);
	g_clear_object (&self->launcher_portal_proxy);
	g_clear_object (&self->monitor);
	g_clear_object (&self->worker);
	g_clear_pointer (&self->url_id_map, g_hash_table_unref);

	G_OBJECT_CLASS (gs_plugin_epiphany_parent_class)->dispose (object);
}

static void
gs_plugin_epiphany_finalize (GObject *object)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (object);

	g_mutex_clear (&self->installed_apps_mutex);
	g_clear_object (&self->permissions);

	G_OBJECT_CLASS (gs_plugin_epiphany_parent_class)->finalize (object);
}

static gboolean ensure_installed_apps_cache (GsPluginEpiphany  *self,
					     GCancellable      *cancellable,
					     GError           **error);

/* Run in @worker. The caller must have already done ensure_installed_apps_cache() */
static void
gs_epiphany_refine_app_state (GsPlugin *plugin,
			      GsApp    *app)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (plugin);

	assert_in_worker (self);

	if (gs_app_get_state (app) == GS_APP_STATE_UNKNOWN) {
		g_autoptr(GsApp) cached_app = NULL;
		const char *appstream_source;

		/* If we have a cached app, set the state from there. Otherwise
		 * only set the state to available if the app came from
		 * appstream data, because there's no way to re-install an app
		 * in Software that was originally installed from Epiphany,
		 * unless we have appstream metainfo for it.
		 */
		cached_app = gs_plugin_cache_lookup (plugin, gs_app_get_id (app));
		appstream_source = gs_app_get_metadata_item (app, "appstream::source-file");
		if (cached_app)
			gs_app_set_state (app, gs_app_get_state (cached_app));
		else if (appstream_source)
			gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
		else {
			gs_app_set_state (app, GS_APP_STATE_UNAVAILABLE);
			gs_app_set_url_missing (app,
						"https://gitlab.gnome.org/GNOME/gnome-software/-/wikis/How-to-reinstall-a-web-app");
		}
	}
}

void
gs_plugin_adopt_app (GsPlugin *plugin,
		     GsApp    *app)
{
	if (gs_app_get_kind (app) == AS_COMPONENT_KIND_WEB_APP &&
	    gs_app_get_bundle_kind (app) != AS_BUNDLE_KIND_PACKAGE) {
		gs_app_set_management_plugin (app, plugin);
	}
}

static gint
get_priority_for_interactivity (gboolean interactive)
{
	return interactive ? G_PRIORITY_DEFAULT : G_PRIORITY_LOW;
}

static void list_apps_thread_cb (GTask        *task,
                                 gpointer      source_object,
                                 gpointer      task_data,
                                 GCancellable *cancellable);

static void
gs_plugin_epiphany_list_apps_async (GsPlugin              *plugin,
                                    GsAppQuery            *query,
                                    GsPluginListAppsFlags  flags,
                                    GCancellable          *cancellable,
                                    GAsyncReadyCallback    callback,
                                    gpointer               user_data)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (plugin);
	g_autoptr(GTask) task = NULL;
	gboolean interactive = (flags & GS_PLUGIN_LIST_APPS_FLAGS_INTERACTIVE);

	task = gs_plugin_list_apps_data_new_task (plugin, query, flags,
						  cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_epiphany_list_apps_async);

	/* Queue a job to get the apps. */
	gs_worker_thread_queue (self->worker, get_priority_for_interactivity (interactive),
				list_apps_thread_cb, g_steal_pointer (&task));
}

/* Run in @worker */
static void
refine_app (GsPluginEpiphany    *self,
	    GsApp               *app,
	    GsPluginRefineFlags  flags,
	    GUri                *uri,
	    const char          *url)
{
	const char *hostname;
	const char *installed_app_id;
	const struct {
		const gchar *hostname;
		const gchar *license_spdx;
	} app_licenses[] = {
	/* Keep in alphabetical order by hostname */
	{ "app.diagrams.net", "Apache-2.0" },
	{ "devdocs.io", "MPL-2.0" },
	{ "discourse.flathub.org", "GPL-2.0-or-later" },
	{ "discourse.gnome.org", "GPL-2.0-or-later" },
	{ "excalidraw.com", "MIT" },
	{ "pinafore.social", "AGPL-3.0-only" },
	{ "snapdrop.net", "GPL-3.0-only" },
	{ "stackedit.io", "Apache-2.0" },
	{ "squoosh.app", "Apache-2.0" },
	};

	g_return_if_fail (GS_IS_APP (app));
	g_return_if_fail (uri != NULL);
	g_return_if_fail (url != NULL);

	gs_app_set_origin (app, "gnome-web");
	gs_app_set_origin_ui (app, _("GNOME Web"));

	gs_app_set_scope (app, AS_COMPONENT_SCOPE_USER);
	gs_app_set_launchable (app, AS_LAUNCHABLE_KIND_URL, url);

	installed_app_id = g_hash_table_lookup (self->url_id_map, url);
	if (installed_app_id) {
		gs_app_set_launchable (app, AS_LAUNCHABLE_KIND_DESKTOP_ID, installed_app_id);
	}

	/* Hard-code the licenses as it's hard to get them programmatically. We
	 * can move them to an AppStream file if needed.
	 */
	hostname = g_uri_get_host (uri);
	if (gs_app_get_license (app) == NULL && hostname != NULL) {
		for (gsize i = 0; i < G_N_ELEMENTS (app_licenses); i++) {
			if (g_str_equal (hostname, app_licenses[i].hostname)) {
				gs_app_set_license (app, GS_APP_QUALITY_NORMAL,
						    app_licenses[i].license_spdx);
				break;
			}
		}
	}

	gs_app_set_size_download (app, GS_SIZE_TYPE_VALID, 0);

	/* Use the default permissions */
	gs_app_set_permissions (app, self->permissions);

	if (gs_app_get_url (app, AS_URL_KIND_HOMEPAGE) == NULL)
		gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, url);

	/* Use the domain name (e.g. "discourse.gnome.org") as a fallback summary.
	 * FIXME: Fetch the summary from the site's webapp manifest.
	 */
	if (gs_app_get_summary (app) == NULL) {
		if (hostname != NULL && *hostname != '\0')
			gs_app_set_summary (app, GS_APP_QUALITY_LOWEST, hostname);
		else
			gs_app_set_summary (app, GS_APP_QUALITY_LOWEST, url);
	}

	if (installed_app_id == NULL)
		return;

	{
		const gchar *name;
		g_autofree char *icon_path = NULL;
		goffset desktop_size = 0, icon_size = 0;
		g_autoptr(GDesktopAppInfo) desktop_info = NULL;
		g_autoptr(GFileInfo) file_info = NULL;
		g_autoptr(GFile) icon_file = NULL;

		desktop_info = g_desktop_app_info_new (installed_app_id);

		if (desktop_info == NULL) {
			g_warning ("Couldn't get GDesktopAppInfo for app %s", installed_app_id);
			return;
		}

		name = g_app_info_get_name (G_APP_INFO (desktop_info));
		gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);

		if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE) {
			g_autoptr(GFile) desktop_file = NULL;
			const gchar *desktop_path;
			guint64 install_date = 0;

			desktop_path = g_desktop_app_info_get_filename (desktop_info);
			g_assert (desktop_path);
			desktop_file = g_file_new_for_path (desktop_path);

			file_info = g_file_query_info (desktop_file,
						       G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
						       0, NULL, NULL);
			if (file_info) {
				install_date = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_CREATED);
				desktop_size = g_file_info_get_size (file_info);
			}
			if (install_date) {
				gs_app_set_install_date (app, install_date);
			}
		}

		icon_path = g_desktop_app_info_get_string (desktop_info, "Icon");
		if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE &&
		    icon_path) {
			icon_file = g_file_new_for_path (icon_path);

			g_clear_object (&file_info);
			file_info = g_file_query_info (icon_file,
						       G_FILE_ATTRIBUTE_STANDARD_SIZE,
						       0, NULL, NULL);
			if (file_info)
				icon_size = g_file_info_get_size (file_info);
		}
		if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON &&
		    gs_app_get_icons (app) == NULL &&
		    icon_path) {
			g_autoptr(GIcon) icon = g_file_icon_new (icon_file);
			g_autofree char *icon_dir = g_path_get_dirname (icon_path);
			g_autofree char *icon_dir_basename = g_path_get_basename (icon_dir);
			const char *x;
			guint64 width = 0;

			/* dir should be either scalable or e.g. 512x512 */
			if (g_strcmp0 (icon_dir_basename, "scalable") == 0) {
				/* Ensure scalable icons are preferred */
				width = 4096;
			} else if ((x = strchr (icon_dir_basename, 'x')) != NULL) {
				g_ascii_string_to_unsigned (x + 1, 10, 1, G_MAXINT, &width, NULL);
			}
			if (width > 0 && width <= 4096) {
				gs_icon_set_width (icon, width);
				gs_icon_set_height (icon, width);
			} else {
				g_warning ("Unexpectedly unable to determine width of icon %s", icon_path);
			}

			gs_app_add_icon (app, icon);
		}
		if (desktop_size > 0 || icon_size > 0) {
			gs_app_set_size_installed (app, GS_SIZE_TYPE_VALID, desktop_size + icon_size);
		}
	}
}

/* Run in @worker */
static GsApp *
gs_epiphany_create_app (GsPluginEpiphany *self,
			const char       *id)
{
	g_autoptr(GsApp) app = NULL;

	assert_in_worker (self);

	app = gs_plugin_cache_lookup (GS_PLUGIN (self), id);
	if (app != NULL)
		return g_steal_pointer (&app);

	app = gs_app_new (id);
	gs_app_set_management_plugin (app, GS_PLUGIN (self));
	gs_app_set_kind (app, AS_COMPONENT_KIND_WEB_APP);
	gs_app_set_metadata (app, "GnomeSoftware::Creator",
			     gs_plugin_get_name (GS_PLUGIN (self)));

	gs_plugin_cache_add (GS_PLUGIN (self), id, app);
	return g_steal_pointer (&app);
}

static gchar * /* (transfer full) */
generate_app_id_for_url (const gchar *url)
{
	/* Generate the app ID used in the AppStream data using the
	 * same method as pwa-metainfo-generator.py in
	 * https://gitlab.gnome.org/mwleeds/gnome-pwa-list
	 * Using this app ID rather than the one provided by Epiphany
	 * makes it possible for the appstream plugin to refine the
	 * GsApp we create (see the comment at the top of this file).
	 */
	g_autofree gchar *url_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, url, -1);
	return g_strconcat ("org.gnome.Software.WebApp_", url_hash, ".desktop", NULL);
}

/* Run in @worker */
static gboolean
ensure_installed_apps_cache (GsPluginEpiphany  *self,
			     GCancellable      *cancellable,
			     GError           **error)
{
	g_autoptr(GVariant) webapps_v = NULL;
	g_auto(GStrv) webapps = NULL;
	guint n_webapps;
	g_autoptr(GsAppList) installed_cache = gs_app_list_new ();
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->installed_apps_mutex);

	assert_in_worker (self);

	if (self->installed_apps_cached)
		return TRUE;

	if (!gs_ephy_web_app_provider_call_get_installed_apps_sync (self->epiphany_proxy,
								    &webapps,
								    cancellable,
								    error)) {
		gs_epiphany_error_convert (error);
		return FALSE;
	}

	n_webapps = g_strv_length (webapps);
	g_debug ("%s: epiphany-webapp-provider returned %u installed web apps", G_STRFUNC, n_webapps);
	for (guint i = 0; i < n_webapps; i++) {
		const gchar *desktop_file_id = webapps[i];
		const gchar *url = NULL;
		g_autofree char *metainfo_app_id = NULL;
		const gchar *exec;
		int argc;
		GsPluginRefineFlags refine_flags;
		g_auto(GStrv) argv = NULL;
		g_autoptr(GsApp) app = NULL;
		g_autoptr(GDesktopAppInfo) desktop_info = NULL;
		g_autoptr(GUri) uri = NULL;

		g_debug ("%s: Working on installed web app %s", G_STRFUNC, desktop_file_id);

		desktop_info = g_desktop_app_info_new (desktop_file_id);

		if (desktop_info == NULL) {
			g_warning ("Epiphany returned a non-existent or invalid desktop ID %s", desktop_file_id);
			continue;
		}

		/* This way of getting the URL is a bit hacky but it's what
		 * Epiphany does, specifically in
		 * ephy_web_application_for_profile_directory() which lives in
		 * https://gitlab.gnome.org/GNOME/epiphany/-/blob/master/lib/ephy-web-app-utils.c
		 */
		exec = g_app_info_get_commandline (G_APP_INFO (desktop_info));
		if (g_shell_parse_argv (exec, &argc, &argv, NULL)) {
			g_assert (argc > 0);
			url = argv[argc - 1];
		}
		if (!url || !(uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL))) {
			g_warning ("Failed to parse URL for web app %s: %s",
				   desktop_file_id, url ? url : "(null)");
			continue;
		}

		/* Store the installed app id for use in refine_app() */
		g_hash_table_insert (self->url_id_map, g_strdup (url),
				     g_strdup (desktop_file_id));

		metainfo_app_id = generate_app_id_for_url (url);
		g_debug ("Creating GsApp for webapp with URL %s using app ID %s (desktop file id: %s)",
			 url, metainfo_app_id, desktop_file_id);

		/* App gets added to the plugin cache here */
		app = gs_epiphany_create_app (self, metainfo_app_id);

		gs_app_set_state (app, GS_APP_STATE_INSTALLED);

		refine_flags = GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON |
			       GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE |
			       GS_PLUGIN_REFINE_FLAGS_REQUIRE_ID;
		refine_app (self, app, refine_flags, uri, url);
	}

	/* Update the state on any apps that were uninstalled outside
	 * gnome-software
	 */
	gs_plugin_cache_lookup_by_state (GS_PLUGIN (self), installed_cache, GS_APP_STATE_INSTALLED);
	for (guint i = 0; i < gs_app_list_length (installed_cache); i++) {
		GsApp *app = gs_app_list_index (installed_cache, i);
		const char *installed_app_id;
		const char *appstream_source;

		installed_app_id = gs_app_get_launchable (app, AS_LAUNCHABLE_KIND_DESKTOP_ID);
		if (installed_app_id == NULL) {
			g_warning ("Installed app unexpectedly has no desktop id: %s", gs_app_get_id (app));
			continue;
		}

		if (g_strv_contains ((const char * const *)webapps, installed_app_id))
			continue;

		gs_plugin_cache_remove (GS_PLUGIN (self), gs_app_get_id (app));

		appstream_source = gs_app_get_metadata_item (app, "appstream::source-file");
		if (appstream_source)
			gs_app_set_state (app, GS_APP_STATE_AVAILABLE);
		else
			gs_app_set_state (app, GS_APP_STATE_UNKNOWN);
	}

	self->installed_apps_cached = TRUE;
	return TRUE;
}

/* Run in @worker */
static void
list_apps_thread_cb (GTask        *task,
                     gpointer      source_object,
                     gpointer      task_data,
                     GCancellable *cancellable)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (source_object);
	g_autoptr(GsAppList) list = gs_app_list_new ();
	GsPluginListAppsData *data = task_data;
	GsAppQueryTristate is_installed = GS_APP_QUERY_TRISTATE_UNSET;
	g_autoptr(GError) local_error = NULL;

	assert_in_worker (self);

	if (data->query != NULL) {
		is_installed = gs_app_query_get_is_installed (data->query);
	}

	/* Currently only support a subset of query properties, and only one set at once.
	 * Also don’t currently support GS_APP_QUERY_TRISTATE_FALSE. */
	if (is_installed == GS_APP_QUERY_TRISTATE_UNSET ||
	    is_installed == GS_APP_QUERY_TRISTATE_FALSE ||
	    gs_app_query_get_n_properties_set (data->query) != 1) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
					 "Unsupported query");
		return;
	}

	/* Ensure the cache is up to date. */
	if (!ensure_installed_apps_cache (self, cancellable, &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	if (is_installed == GS_APP_QUERY_TRISTATE_TRUE)
		gs_plugin_cache_lookup_by_state (GS_PLUGIN (self), list, GS_APP_STATE_INSTALLED);

	g_task_return_pointer (task, g_steal_pointer (&list), g_object_unref);
}

static GsAppList *
gs_plugin_epiphany_list_apps_finish (GsPlugin      *plugin,
                                     GAsyncResult  *result,
                                     GError       **error)
{
	g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gs_plugin_epiphany_list_apps_async, FALSE);
	return g_task_propagate_pointer (G_TASK (result), error);
}

static void
gs_epiphany_refine_app (GsPluginEpiphany    *self,
			GsApp               *app,
			GsPluginRefineFlags  refine_flags,
			const char          *url)
{
	g_autoptr(GUri) uri = NULL;

	g_return_if_fail (url != NULL && *url != '\0');

	if (!(uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL))) {
		g_warning ("Failed to parse URL for web app %s: %s", gs_app_get_id (app), url);
		return;
	}

	refine_app (self, app, refine_flags, uri, url);
}

static void refine_thread_cb (GTask        *task,
                              gpointer      source_object,
                              gpointer      task_data,
                              GCancellable *cancellable);

static void
gs_plugin_epiphany_refine_async (GsPlugin            *plugin,
                                 GsAppList           *list,
                                 GsPluginRefineFlags  flags,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (plugin);
	g_autoptr(GTask) task = NULL;
	gboolean interactive = gs_plugin_has_flags (GS_PLUGIN (self), GS_PLUGIN_FLAGS_INTERACTIVE);

	task = gs_plugin_refine_data_new_task (plugin, list, flags, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_plugin_epiphany_refine_async);

	/* Queue a job for the refine. */
	gs_worker_thread_queue (self->worker, get_priority_for_interactivity (interactive),
				refine_thread_cb, g_steal_pointer (&task));
}

/* Run in @worker. */
static void
refine_thread_cb (GTask        *task,
                  gpointer      source_object,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (source_object);
	GsPluginRefineData *data = task_data;
	GsPluginRefineFlags flags = data->flags;
	GsAppList *list = data->list;
	g_autoptr(GError) local_error = NULL;

	assert_in_worker (self);

	if (!ensure_installed_apps_cache (self, cancellable, &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		const char *url;

		/* not us */
		if (gs_app_get_kind (app) != AS_COMPONENT_KIND_WEB_APP ||
		    gs_app_get_bundle_kind (app) == AS_BUNDLE_KIND_PACKAGE)
			continue;

		url = gs_app_get_launchable (app, AS_LAUNCHABLE_KIND_URL);
		if (url == NULL || *url == '\0') {
			/* A launchable URL is required by the AppStream spec */
			g_warning ("Web app %s missing launchable url", gs_app_get_id (app));
			continue;
		}

		g_debug ("epiphany: refining app %s", gs_app_get_id (app));
		gs_epiphany_refine_app (self, app, flags, url);
		gs_epiphany_refine_app_state (GS_PLUGIN (self), app);

		/* Usually the way to refine wildcard apps is to create a new
		 * GsApp and add it to the results list, but in this case we
		 * need to use the app that was refined by the appstream plugin
		 * as it has all the metadata set already, and this is the only
		 * plugin for dealing with web apps, so it should be safe to
		 * adopt the wildcard app.
		 */
		if (gs_app_has_quirk (app, GS_APP_QUIRK_IS_WILDCARD)) {
			gs_app_remove_quirk (app, GS_APP_QUIRK_IS_WILDCARD);
			gs_app_set_management_plugin (app, GS_PLUGIN (self));
			gs_plugin_cache_add (GS_PLUGIN (self), gs_app_get_id (app), app);
		}
	}

	/* success */
	g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_epiphany_refine_finish (GsPlugin      *plugin,
                                  GAsyncResult  *result,
                                  GError       **error)
{
	g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gs_plugin_epiphany_refine_async, FALSE);
	return g_task_propagate_boolean (G_TASK (result), error);
}

static GVariant *
get_serialized_icon (GsApp *app,
		     GIcon *icon)
{
	g_autofree char *icon_path = NULL;
	g_autoptr(GInputStream) stream = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GIcon) bytes_icon = NULL;
	g_autoptr(GVariant) icon_v = NULL;
	guint icon_width;

	/* Note: GsRemoteIcon will work on this GFileIcon code path.
	 * The icons plugin should have called
	 * gs_app_ensure_icons_downloaded() for us.
	 */
	if (!G_IS_FILE_ICON (icon))
		return NULL;

	icon_path = g_file_get_path (g_file_icon_get_file (G_FILE_ICON (icon)));
	if (!g_str_has_suffix (icon_path, ".png") &&
	    !g_str_has_suffix (icon_path, ".svg") &&
	    !g_str_has_suffix (icon_path, ".jpeg") &&
	    !g_str_has_suffix (icon_path, ".jpg")) {
		g_warning ("Icon for app %s has unsupported file extension: %s",
			   gs_app_get_id (app), icon_path);
		return NULL;
	}

	/* Scale down to the portal's size limit if needed */
	icon_width = gs_icon_get_width (icon);
	if (icon_width > 512)
		icon_width = 512;

	/* Serialize the icon as a #GBytesIcon since that's
	 * what the dynamic launcher portal requires.
	 */
	stream = g_loadable_icon_load (G_LOADABLE_ICON (icon), icon_width, NULL, NULL, NULL);

	/* Icons are usually smaller than 1 MiB. Set a 10 MiB
	 * limit so we can't use a huge amount of memory or hit
	 * the D-Bus message size limit
	 */
	if (stream)
		bytes = g_input_stream_read_bytes (stream, 10485760 /* 10 MiB */, NULL, NULL);
	if (bytes)
		bytes_icon = g_bytes_icon_new (bytes);
	if (bytes_icon)
		icon_v = g_icon_serialize (bytes_icon);

	return g_steal_pointer (&icon_v);
}

gboolean
gs_plugin_app_install (GsPlugin      *plugin,
		       GsApp         *app,
		       GCancellable  *cancellable,
		       GError       **error)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (plugin);
	const char *url;
	const char *name;
	const char *token = NULL;
	g_autofree char *installed_app_id = NULL;
	g_autoptr(GVariant) token_v = NULL;
	g_autoptr(GVariant) icon_v = NULL;
	GVariantBuilder opt_builder;
	const int icon_sizes[] = {512, 192, 128, 1};

	if (!gs_app_has_management_plugin (app, plugin))
		return TRUE;

	url = gs_app_get_url (app, AS_URL_KIND_HOMEPAGE);
	if (url == NULL || *url == '\0') {
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			     "Can't install web app %s without url",
			     gs_app_get_id (app));
		return FALSE;
	}
	name = gs_app_get_name (app);
	if (name == NULL || *name == '\0') {
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			     "Can't install web app %s without name",
			     gs_app_get_id (app));
		return FALSE;
	}
	for (guint i = 0; i < G_N_ELEMENTS (icon_sizes); i++) {
		GIcon *icon = gs_app_get_icon_for_size (app, icon_sizes[i], 1, NULL);
		if (icon != NULL)
			icon_v = get_serialized_icon (app, icon);
		if (icon_v != NULL)
			break;
	}
	if (icon_v == NULL) {
		g_set_error (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
			     "Can't install web app %s without icon",
			     gs_app_get_id (app));
		return FALSE;
	}

	gs_app_set_state (app, GS_APP_STATE_INSTALLING);
	/* First get a token from xdg-desktop-portal so Epiphany can do the
	 * installation without user confirmation
	 */
	g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
	token_v = g_dbus_proxy_call_sync (self->launcher_portal_proxy,
					  "RequestInstallToken",
					  g_variant_new ("(sva{sv})",
							 name, icon_v, &opt_builder),
					  G_DBUS_CALL_FLAGS_NONE,
					  -1, cancellable, error);
	if (token_v == NULL) {
		gs_epiphany_error_convert (error);
		gs_app_set_state_recover (app);
		return FALSE;
	}

	/* Then pass the token to Epiphany which will use xdg-desktop-portal to
	 * complete the installation
	 */
	g_variant_get (token_v, "(&s)", &token);
	if (!gs_ephy_web_app_provider_call_install_sync (self->epiphany_proxy,
							 url, name, token,
							 &installed_app_id,
							 cancellable,
							 error)) {
		gs_epiphany_error_convert (error);
		gs_app_set_state_recover (app);
		return FALSE;
	}

	{
		g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->installed_apps_mutex);
		g_hash_table_insert (self->url_id_map, g_strdup (url),
				     g_strdup (installed_app_id));
	}

	gs_app_set_launchable (app, AS_LAUNCHABLE_KIND_DESKTOP_ID, installed_app_id);
	gs_app_set_state (app, GS_APP_STATE_INSTALLED);

	return TRUE;
}

gboolean
gs_plugin_app_remove (GsPlugin      *plugin,
		      GsApp         *app,
		      GCancellable  *cancellable,
		      GError       **error)
{
	GsPluginEpiphany *self = GS_PLUGIN_EPIPHANY (plugin);
	const char *installed_app_id;
	const char *url;

	if (!gs_app_has_management_plugin (app, plugin))
		return TRUE;

	installed_app_id = gs_app_get_launchable (app, AS_LAUNCHABLE_KIND_DESKTOP_ID);
	if (installed_app_id == NULL) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "App can't be uninstalled without installed app ID");
		gs_app_set_state_recover (app);
		return FALSE;
	}

	gs_app_set_state (app, GS_APP_STATE_REMOVING);
	if (!gs_ephy_web_app_provider_call_uninstall_sync (self->epiphany_proxy,
							   installed_app_id,
							   cancellable,
							   error)) {
		gs_epiphany_error_convert (error);
		gs_app_set_state_recover (app);
		return FALSE;
	}

	url = gs_app_get_launchable (app, AS_LAUNCHABLE_KIND_URL);
	if (url != NULL && *url != '\0') {
		g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&self->installed_apps_mutex);
		g_hash_table_remove (self->url_id_map, url);
	}

	/* The app is not necessarily available; it may have been installed
	 * directly in Epiphany
	 */
	gs_app_set_state (app, GS_APP_STATE_UNKNOWN);

	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin      *plugin,
		  GsApp         *app,
		  GCancellable  *cancellable,
		  GError       **error)
{
	if (!gs_app_has_management_plugin (app, plugin))
		return TRUE;

	return gs_plugin_app_launch (plugin, app, error);
}

static void
gs_plugin_epiphany_class_init (GsPluginEpiphanyClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPluginClass *plugin_class = GS_PLUGIN_CLASS (klass);

	object_class->dispose = gs_plugin_epiphany_dispose;
	object_class->finalize = gs_plugin_epiphany_finalize;

	plugin_class->setup_async = gs_plugin_epiphany_setup_async;
	plugin_class->setup_finish = gs_plugin_epiphany_setup_finish;
	plugin_class->shutdown_async = gs_plugin_epiphany_shutdown_async;
	plugin_class->shutdown_finish = gs_plugin_epiphany_shutdown_finish;
	plugin_class->refine_async = gs_plugin_epiphany_refine_async;
	plugin_class->refine_finish = gs_plugin_epiphany_refine_finish;
	plugin_class->list_apps_async = gs_plugin_epiphany_list_apps_async;
	plugin_class->list_apps_finish = gs_plugin_epiphany_list_apps_finish;
}

GType
gs_plugin_query_type (void)
{
	return GS_TYPE_PLUGIN_EPIPHANY;
}
