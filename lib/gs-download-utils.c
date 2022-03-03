/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2021, 2022 Endless OS Foundation LLC
 *
 * Author: Philip Withnall <pwithnall@endlessos.org>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

/**
 * SECTION:gs-download-utils
 * @short_description: Download and HTTP utilities
 *
 * A set of utilities for downloading things and doing HTTP requests.
 *
 * Since: 42
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <libsoup/soup.h>

#include "gs-download-utils.h"
#include "gs-utils.h"

/**
 * gs_build_soup_session:
 *
 * Build a new #SoupSession configured with the gnome-software user agent.
 *
 * A new #SoupSession should be used for each independent download context, such
 * as in different plugins. Each #SoupSession caches HTTP connections and
 * authentication information, and these likely needn’t be shared between
 * plugins. Using separate sessions reduces thread contention.
 *
 * Returns: (transfer full): a new #SoupSession
 * Since: 42
 */
SoupSession *
gs_build_soup_session (void)
{
	return soup_session_new_with_options ("user-agent", gs_user_agent (),
					      "timeout", 10,
					      NULL);
}

typedef struct {
	/* Input data. */
	gchar *uri;  /* (not nullable) (owned) */
	GInputStream *input_stream;  /* (nullable) (owned) */
	GOutputStream *output_stream;  /* (nullable) (owned) */
	gsize buffer_size_bytes;
	int io_priority;
	GsDownloadProgressCallback progress_callback;  /* (nullable) */
	gpointer progress_user_data;

	/* In-progress state. */
	SoupMessage *message;  /* (nullable) (owned) */
	gboolean close_input_stream;
	gboolean close_output_stream;
	gboolean discard_output_stream;
	gsize total_read_bytes;
	gsize total_written_bytes;
	gsize expected_stream_size_bytes;
	GBytes *currently_unwritten_chunk;  /* (nullable) (owned) */

	/* Output data. */
	gchar *new_etag;  /* (nullable) (owned) */
	GError *error;  /* (nullable) (owned) */
} DownloadData;

static void
download_data_free (DownloadData *data)
{
	g_assert (data->input_stream == NULL || g_input_stream_is_closed (data->input_stream));
	g_assert (data->output_stream == NULL || g_output_stream_is_closed (data->output_stream));

	g_assert (data->currently_unwritten_chunk == NULL || data->error != NULL);

	g_clear_object (&data->input_stream);
	g_clear_object (&data->output_stream);

	g_clear_object (&data->message);
	g_clear_pointer (&data->uri, g_free);
	g_clear_pointer (&data->new_etag, g_free);
	g_clear_pointer (&data->currently_unwritten_chunk, g_bytes_unref);
	g_clear_error (&data->error);

	g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DownloadData, download_data_free)

static void open_input_stream_cb (GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data);
static void read_bytes_cb (GObject      *source_object,
                           GAsyncResult *result,
                           gpointer      user_data);
static void write_bytes_cb (GObject      *source_object,
                            GAsyncResult *result,
                            gpointer      user_data);
static void finish_download (GTask  *task,
                             GError *error);
static void close_stream_cb (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data);
static void download_progress (GTask *task);

/**
 * gs_download_stream_async:
 * @soup_session: a #SoupSession
 * @uri: (not nullable): the URI to download
 * @output_stream: (not nullable): an output stream to write the download to
 * @last_etag: (nullable): the last-known ETag of the URI, or %NULL if unknown
 * @io_priority: I/O priority to download and write at
 * @progress_callback: (nullable): callback to call with progress information
 * @progress_user_data: (nullable) (closure progress_callback): data to pass
 *   to @progress_callback
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @callback: callback to call once the operation is complete
 * @user_data: (closure callback): data to pass to @callback
 *
 * Download @uri and write it to @output_stream asynchronously.
 *
 * If @last_etag is non-%NULL, it will be sent to the server, which may return
 * a ‘not modified’ response. If so, @output_stream will not be written to, and
 * will be closed with a cancelled close operation. This will ensure that the
 * existing content of the output stream (if it’s a file, for example) will not
 * be overwritten.
 *
 * If specified, @progress_callback will be called zero or more times until
 * @callback is called, providing progress updates on the download.
 *
 * Since: 42
 */
void
gs_download_stream_async (SoupSession                *soup_session,
                          const gchar                *uri,
                          GOutputStream              *output_stream,
                          const gchar                *last_etag,
                          int                         io_priority,
                          GsDownloadProgressCallback  progress_callback,
                          gpointer                    progress_user_data,
                          GCancellable               *cancellable,
                          GAsyncReadyCallback         callback,
                          gpointer                    user_data)
{
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(SoupMessage) msg = NULL;
	DownloadData *data;
	g_autoptr(DownloadData) data_owned = NULL;

	g_return_if_fail (SOUP_IS_SESSION (soup_session));
	g_return_if_fail (uri != NULL);
	g_return_if_fail (G_IS_OUTPUT_STREAM (output_stream));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	task = g_task_new (soup_session, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_download_stream_async);

	data = data_owned = g_new0 (DownloadData, 1);
	data->uri = g_strdup (uri);
	data->output_stream = g_object_ref (output_stream);
	data->close_output_stream = TRUE;
	data->buffer_size_bytes = 8192;  /* arbitrarily chosen */
	data->io_priority = io_priority;
	data->progress_callback = progress_callback;
	data->progress_user_data = progress_user_data;

	g_task_set_task_data (task, g_steal_pointer (&data_owned), (GDestroyNotify) download_data_free);

	/* local */
	if (g_str_has_prefix (uri, "file://")) {
		g_autoptr(GFile) local_file = g_file_new_for_path (uri + strlen ("file://"));
		g_file_read_async (local_file, io_priority, cancellable, open_input_stream_cb, g_steal_pointer (&task));
		return;
	}

	/* remote */
	g_debug ("Downloading %s to %s", uri, G_OBJECT_TYPE_NAME (output_stream));
	msg = soup_message_new (SOUP_METHOD_GET, uri);
	if (msg == NULL) {
		finish_download (task,
				 g_error_new (G_IO_ERROR,
					      G_IO_ERROR_INVALID_ARGUMENT,
					      "Failed to parse URI ‘%s’", uri));
		return;
	}

	data->message = g_object_ref (msg);

	if (last_etag != NULL && *last_etag != '\0') {
#if SOUP_CHECK_VERSION(3, 0, 0)
		soup_message_headers_append (soup_message_get_request_headers (msg), "If-None-Match", last_etag);
#else
		soup_message_headers_append (msg->request_headers, "If-None-Match", last_etag);
#endif
	}

#if SOUP_CHECK_VERSION(3, 0, 0)
	soup_session_send_async (soup_session, msg, data->io_priority, cancellable, open_input_stream_cb, g_steal_pointer (&task));
#else
	soup_session_send_async (soup_session, msg, cancellable, open_input_stream_cb, g_steal_pointer (&task));
#endif
}

static void
open_input_stream_cb (GObject      *source_object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	DownloadData *data = g_task_get_task_data (task);
	GCancellable *cancellable = g_task_get_cancellable (task);
	g_autoptr(GInputStream) input_stream = NULL;
	g_autoptr(GError) local_error = NULL;

	/* This function can be called as a result of either reading a local
	 * file, or sending an HTTP request, so @source_object’s type can vary. */
	if (G_IS_FILE (source_object)) {
		GFile *local_file = G_FILE (source_object);

		/* Local file. */
		input_stream = G_INPUT_STREAM (g_file_read_finish (local_file, result, &local_error));

		if (input_stream == NULL) {
			g_prefix_error (&local_error, "Failed to read ‘%s’: ",
					g_file_peek_path (local_file));
			finish_download (task, g_steal_pointer (&local_error));
			return;
		}

		g_assert (data->input_stream == NULL);
		data->input_stream = g_object_ref (input_stream);
		data->close_input_stream = TRUE;
	} else if (SOUP_IS_SESSION (source_object)) {
		SoupSession *soup_session = SOUP_SESSION (source_object);
		guint status_code;
		const gchar *new_etag;

		/* HTTP request. */
#if SOUP_CHECK_VERSION(3, 0, 0)
		input_stream = soup_session_send_finish (soup_session, result, &local_error);
		status_code = soup_message_get_status (data->message);
#else
		input_stream = soup_session_send_finish (soup_session, result, &local_error);
		status_code = data->message->status_code;
#endif

		if (input_stream != NULL) {
			g_assert (data->input_stream == NULL);
			data->input_stream = g_object_ref (input_stream);
			data->close_input_stream = TRUE;
		}

		if (status_code == SOUP_STATUS_NOT_MODIFIED) {
			/* If the file has not been modified from the ETag we
			 * have, finish the download early. Ensure to close the
			 * output stream so that its existing content is *not*
			 * overwritten. */
			data->discard_output_stream = TRUE;
			finish_download (task, NULL);
			return;
		} else if (status_code != SOUP_STATUS_OK) {
			g_autoptr(GString) str = g_string_new (NULL);
			g_string_append (str, soup_status_get_phrase (status_code));

			if (local_error != NULL) {
				g_string_append (str, ": ");
				g_string_append (str, local_error->message);
			}

			finish_download (task,
					 g_error_new (G_IO_ERROR,
						      G_IO_ERROR_FAILED,
						      "Failed to download ‘%s’: %s",
						      data->uri, str->str));
			return;
		}

		g_assert (input_stream != NULL);

		/* Get the expected download size. */
#if SOUP_CHECK_VERSION(3, 0, 0)
		data->expected_stream_size_bytes = soup_message_headers_get_content_length (soup_message_get_response_headers (data->message));
#else
		data->expected_stream_size_bytes = soup_message_headers_get_content_length (data->message->response_headers);
#endif

		/* Store the new ETag for later use. */
#if SOUP_CHECK_VERSION(3, 0, 0)
		new_etag = soup_message_headers_get_one (soup_message_get_response_headers (data->message), "ETag");
#else
		new_etag = soup_message_headers_get_one (data->message->response_headers, "ETag");
#endif
		if (new_etag != NULL && *new_etag == '\0')
			new_etag = NULL;
		data->new_etag = g_strdup (new_etag);
	} else {
		g_assert_not_reached ();
	}

	/* Splice in an asynchronous loop. We unfortunately can’t use
	 * g_output_stream_splice_async() here, as it doesn’t provide a progress
	 * callback. The approach is the same though. */
	g_input_stream_read_bytes_async (input_stream, data->buffer_size_bytes, data->io_priority,
					 cancellable, read_bytes_cb, g_steal_pointer (&task));
}

static void
read_bytes_cb (GObject      *source_object,
               GAsyncResult *result,
               gpointer      user_data)
{
	GInputStream *input_stream = G_INPUT_STREAM (source_object);
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	DownloadData *data = g_task_get_task_data (task);
	GCancellable *cancellable = g_task_get_cancellable (task);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) local_error = NULL;

	bytes = g_input_stream_read_bytes_finish (input_stream, result, &local_error);

	if (bytes == NULL) {
		finish_download (task, g_steal_pointer (&local_error));
		return;
	}

	/* Report progress. */
	data->total_read_bytes += g_bytes_get_size (bytes);
	data->expected_stream_size_bytes = MAX (data->expected_stream_size_bytes, data->total_read_bytes);
	download_progress (task);

	/* Write the downloaded data. */
	if (g_bytes_get_size (bytes) > 0) {
		g_clear_pointer (&data->currently_unwritten_chunk, g_bytes_unref);
		data->currently_unwritten_chunk = g_bytes_ref (bytes);

		g_output_stream_write_bytes_async (data->output_stream, bytes, data->io_priority,
						   cancellable, write_bytes_cb, g_steal_pointer (&task));
	} else {
		finish_download (task, NULL);
	}
}

static void
write_bytes_cb (GObject      *source_object,
                GAsyncResult *result,
                gpointer      user_data)
{
	GOutputStream *output_stream = G_OUTPUT_STREAM (source_object);
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	DownloadData *data = g_task_get_task_data (task);
	GCancellable *cancellable = g_task_get_cancellable (task);
	gssize bytes_written_signed;
	gsize bytes_written;
	g_autoptr(GError) local_error = NULL;

	bytes_written_signed = g_output_stream_write_bytes_finish (output_stream, result, &local_error);

	if (bytes_written_signed < 0) {
		finish_download (task, g_steal_pointer (&local_error));
		return;
	}

	/* We know this is non-negative now. */
	bytes_written = (gsize) bytes_written_signed;

	/* Report progress. */
	data->total_written_bytes += bytes_written;
	download_progress (task);

	g_assert (data->currently_unwritten_chunk != NULL);

	if (bytes_written < g_bytes_get_size (data->currently_unwritten_chunk)) {
		/* Partial write; try again with the remaining bytes. */
		g_autoptr(GBytes) sub_bytes = g_bytes_new_from_bytes (data->currently_unwritten_chunk, bytes_written, g_bytes_get_size (data->currently_unwritten_chunk) - bytes_written);
		g_assert (bytes_written > 0);

		g_clear_pointer (&data->currently_unwritten_chunk, g_bytes_unref);
		data->currently_unwritten_chunk = g_bytes_ref (sub_bytes);

		g_output_stream_write_bytes_async (output_stream, sub_bytes, data->io_priority,
						   cancellable, write_bytes_cb, g_steal_pointer (&task));
	} else {
		/* Full write succeeded. Start the next read. */
		g_clear_pointer (&data->currently_unwritten_chunk, g_bytes_unref);

		g_input_stream_read_bytes_async (data->input_stream, data->buffer_size_bytes, data->io_priority,
						 cancellable, read_bytes_cb, g_steal_pointer (&task));
	}
}

/* error is (transfer full) */
static void
finish_download (GTask  *task,
                 GError *error)
{
	DownloadData *data = g_task_get_task_data (task);
	GCancellable *cancellable = g_task_get_cancellable (task);

	/* Final progress update. */
	if (error == NULL) {
		data->expected_stream_size_bytes = data->total_read_bytes;
		download_progress (task);
	}

	/* Record the error from the operation, if set. */
	g_assert (data->error == NULL);
	data->error = g_steal_pointer (&error);

	g_assert (!data->discard_output_stream || data->close_output_stream);

	if (data->close_output_stream) {
		g_autoptr(GCancellable) output_cancellable = NULL;

		g_assert (data->output_stream != NULL);

		/* If there’s been a prior error, or we are aborting writing the
		 * output stream (perhaps because of a cache hit), close the
		 * output stream but cancel the close operation so that the old
		 * output file is not overwritten. */
		if (data->error != NULL || data->discard_output_stream) {
			output_cancellable = g_cancellable_new ();
			g_cancellable_cancel (output_cancellable);
		} else if (g_task_get_cancellable (task) != NULL) {
			output_cancellable = g_object_ref (g_task_get_cancellable (task));
		}

		g_output_stream_close_async (data->output_stream, data->io_priority, output_cancellable, close_stream_cb, g_object_ref (task));
	}

	if (data->close_input_stream && data->input_stream != NULL) {
		g_input_stream_close_async (data->input_stream, data->io_priority, cancellable, close_stream_cb, g_object_ref (task));
	}

	/* Check in case both streams are already closed. */
	close_stream_cb (NULL, NULL, g_object_ref (task));
}

static void
close_stream_cb (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	DownloadData *data = g_task_get_task_data (task);
	g_autoptr(GError) local_error = NULL;

	if (G_IS_INPUT_STREAM (source_object)) {
		/* Errors in closing the input stream are not fatal. */
		if (!g_input_stream_close_finish (G_INPUT_STREAM (source_object),
						  result, &local_error))
			g_debug ("Error closing input stream: %s", local_error->message);
		g_clear_error (&local_error);

		data->close_input_stream = FALSE;
	} else if (G_IS_OUTPUT_STREAM (source_object)) {
		/* Errors in closing the output stream are fatal, but don’t
		 * overwrite errors set earlier in the operation. */
		if (!g_output_stream_close_finish (G_OUTPUT_STREAM (source_object),
						   result, &local_error)) {
			if (data->error == NULL)
				data->error = g_steal_pointer (&local_error);
			else if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				g_debug ("Error closing output stream: %s", local_error->message);
		}
		g_clear_error (&local_error);

		data->close_output_stream = FALSE;
		data->discard_output_stream = FALSE;
	} else {
		/* finish_download() calls this with a NULL source_object */
	}

	/* Still waiting for one of the streams to close? */
	if (data->close_input_stream || data->close_output_stream)
		return;

	if (data->error != NULL) {
		g_task_return_error (task, g_error_copy (data->error));
	} else {
		g_task_return_boolean (task, TRUE);
	}
}

static void
download_progress (GTask *task)
{
	DownloadData *data = g_task_get_task_data (task);

	if (data->progress_callback != NULL) {
		/* This should be guaranteed by the rest of the download code. */
		g_assert (data->expected_stream_size_bytes >= data->total_written_bytes);

		data->progress_callback (data->total_written_bytes, data->expected_stream_size_bytes,
					 data->progress_user_data);
	}
}

/**
 * gs_download_stream_finish:
 * @soup_session: a #SoupSession
 * @result: result of the asynchronous operation
 * @new_etag_out: (out callee-allocates) (transfer full) (optional) (nullable):
 *   return location for the ETag of the downloaded file (which may be %NULL),
 *   or %NULL to ignore it
 * @error: return location for a #GError
 *
 * Finish an asynchronous download operation started with
 * gs_download_stream_async().
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: 42
 */
gboolean
gs_download_stream_finish (SoupSession   *soup_session,
                           GAsyncResult  *result,
                           gchar        **new_etag_out,
                           GError       **error)
{
	DownloadData *data;

	g_return_val_if_fail (g_task_is_valid (result, soup_session), FALSE);
	g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gs_download_stream_async, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	data = g_task_get_task_data (G_TASK (result));

	if (new_etag_out != NULL)
		*new_etag_out = g_strdup (data->new_etag);

	return g_task_propagate_boolean (G_TASK (result), error);
}

typedef struct {
	/* Input data. */
	gchar *uri;  /* (not nullable) (owned) */
	GFile *output_file;  /* (not nullable) (owned) */
	int io_priority;
	GsDownloadProgressCallback progress_callback;
	gpointer progress_user_data;

	/* In-progress data. */
	gchar *last_etag;  /* (nullable) (owned) */
} DownloadFileData;

static void
download_file_data_free (DownloadFileData *data)
{
	g_free (data->uri);
	g_clear_object (&data->output_file);
	g_free (data->last_etag);
	g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DownloadFileData, download_file_data_free)

static void download_replace_file_cb (GObject      *source_object,
                                      GAsyncResult *result,
                                      gpointer      user_data);
static void download_file_cb (GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data);

/**
 * gs_download_file_async:
 * @soup_session: a #SoupSession
 * @uri: (not nullable): the URI to download
 * @output_file: (not nullable): an output file to write the download to
 * @io_priority: I/O priority to download and write at
 * @progress_callback: (nullable): callback to call with progress information
 * @progress_user_data: (nullable) (closure progress_callback): data to pass
 *   to @progress_callback
 * @cancellable: (nullable): a #GCancellable, or %NULL
 * @callback: callback to call once the operation is complete
 * @user_data: (closure callback): data to pass to @callback
 *
 * Download @uri and write it to @output_file asynchronously, overwriting the
 * existing content of @output_file.
 *
 * The ETag of @output_file will be queried and, if known, used to skip the
 * download if @output_file is already up to date.
 *
 * If specified, @progress_callback will be called zero or more times until
 * @callback is called, providing progress updates on the download.
 *
 * Since: 42
 */
void
gs_download_file_async (SoupSession                *soup_session,
                        const gchar                *uri,
                        GFile                      *output_file,
                        int                         io_priority,
                        GsDownloadProgressCallback  progress_callback,
                        gpointer                    progress_user_data,
                        GCancellable               *cancellable,
                        GAsyncReadyCallback         callback,
                        gpointer                    user_data)
{
	g_autoptr(GTask) task = NULL;
	DownloadFileData *data;
	g_autoptr(DownloadFileData) data_owned = NULL;
	g_autoptr(GFile) output_file_parent = NULL;
	g_autoptr(GError) local_error = NULL;

	g_return_if_fail (SOUP_IS_SESSION (soup_session));
	g_return_if_fail (uri != NULL);
	g_return_if_fail (G_IS_FILE (output_file));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	task = g_task_new (soup_session, cancellable, callback, user_data);
	g_task_set_source_tag (task, gs_download_file_async);

	data = data_owned = g_new0 (DownloadFileData, 1);
	data->uri = g_strdup (uri);
	data->output_file = g_object_ref (output_file);
	data->io_priority = io_priority;
	data->progress_callback = progress_callback;
	data->progress_user_data = progress_user_data;
	g_task_set_task_data (task, g_steal_pointer (&data_owned), (GDestroyNotify) download_file_data_free);

	/* Create the destination file’s directory.
	 * FIXME: This should be made async; it hasn’t done for now as it’s
	 * likely to be fast. */
	output_file_parent = g_file_get_parent (output_file);

	if (output_file_parent != NULL &&
	    !g_file_make_directory_with_parents (output_file_parent, cancellable, &local_error) &&
	    !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	g_clear_error (&local_error);

	/* Query the old ETag if the file already exists. */
	data->last_etag = gs_utils_get_file_etag (output_file, cancellable);

	/* Create the output file. */
	g_file_replace_async (output_file,
			      data->last_etag,
			      FALSE,  /* make_backup */
			      G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
			      io_priority,
			      cancellable,
			      download_replace_file_cb,
			      g_steal_pointer (&task));
}

static void
download_replace_file_cb (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
	GFile *output_file = G_FILE (source_object);
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	SoupSession *soup_session = g_task_get_source_object (task);
	GCancellable *cancellable = g_task_get_cancellable (task);
	DownloadFileData *data = g_task_get_task_data (task);
	g_autoptr(GFileOutputStream) output_stream = NULL;
	g_autoptr(GError) local_error = NULL;

	output_stream = g_file_replace_finish (output_file, result, &local_error);

	if (output_stream == NULL) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	/* Do the download. */
	gs_download_stream_async (soup_session, data->uri, G_OUTPUT_STREAM (output_stream),
				  data->last_etag, data->io_priority,
				  data->progress_callback, data->progress_user_data,
				  cancellable, download_file_cb, g_steal_pointer (&task));
}

static void
download_file_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
	SoupSession *soup_session = SOUP_SESSION (source_object);
	g_autoptr(GTask) task = g_steal_pointer (&user_data);
	GCancellable *cancellable = g_task_get_cancellable (task);
	DownloadFileData *data = g_task_get_task_data (task);
	g_autofree gchar *new_etag = NULL;
	g_autoptr(GError) local_error = NULL;

	if (!gs_download_stream_finish (soup_session, result, &new_etag, &local_error)) {
		g_task_return_error (task, g_steal_pointer (&local_error));
		return;
	}

	/* Update the ETag. */
	gs_utils_set_file_etag (data->output_file, new_etag, cancellable);

	g_task_return_boolean (task, TRUE);
}

/**
 * gs_download_file_finish:
 * @soup_session: a #SoupSession
 * @result: result of the asynchronous operation
 * @error: return location for a #GError
 *
 * Finish an asynchronous download operation started with
 * gs_download_file_async().
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: 42
 */
gboolean
gs_download_file_finish (SoupSession   *soup_session,
                         GAsyncResult  *result,
                         GError       **error)
{
	g_return_val_if_fail (g_task_is_valid (result, soup_session), FALSE);
	g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) == gs_download_file_async, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	return g_task_propagate_boolean (G_TASK (result), error);
}