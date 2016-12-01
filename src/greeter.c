/*
 * greeter.c
 *
 * Copyright © 2014-2016 Antergos Developers <dev@antergos.com>
 *
 * Includes Code Contributed By:
 * Copyright © 2016 Scott Balneaves <sbalneav@ltsp.org>
 *
 * Based on code from lightdm-webkit-greeter:
 * Copyright © 2010-2015 Robert Ancell <robert.ancell@canonical.com>
 *
 * This file is part of lightdm-webkit2-greeter.
 *
 * lightdm-webkit2-greeter is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * lightdm-webkit2-greeter is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * The following additional terms are in effect as per Section 7 of the license:
 *
 * The preservation of all legal notices and author attributions in
 * the material or in the Appropriate Legal Notices displayed
 * by works containing it is required.
 *
 * You should have received a copy of the GNU General Public License
 * along with lightdm-webkit2-greeter; If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib/gprintf.h>
#include <glib-unix.h>
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>
#include <glib/gi18n.h>
#include <sys/mman.h>

#include "config.h"
#include "greeter-resources.h"

/* Work-around CLion bug */
#ifndef CONFIG_DIR
#include "../build/src/config.h"
#include "../build/src/greeter-resources.h"
#endif


static GtkWidget *web_view;
static GtkWidget *window;
static WebKitSettings *webkit_settings;
static GdkDisplay *default_display;
static GResource *greeter_resources;
static WebKitUserContentManager *manager;

/* Screensaver values */
static int
	timeout,
	interval,
	prefer_blanking,
	allow_exposures;

static gint config_timeout;

static gboolean
	debug_mode,
	heartbeat,
	heartbeat_exit;


static void
initialize_web_extensions_cb(WebKitWebContext *context, gpointer user_data) {

	webkit_web_context_set_web_extensions_directory(context, WEBEXT_DIR);

}


static void
create_new_webkit_settings_object(void) {
	webkit_settings = webkit_settings_new_with_settings(
		"enable-developer-extras", TRUE,
		"javascript-can-open-windows-automatically", TRUE,
		"allow-file-access-from-file-urls", TRUE,
		"enable-write-console-messages-to-stdout", TRUE,
	#ifdef HAS_WEBKITGTK_2_14
		"allow-universal-access-from-file-urls", TRUE,
	#endif
		NULL
	);
}


static gboolean
context_menu_cb(WebKitWebView *view,
				WebKitContextMenu *context_menu,
				GdkEvent *event,
				WebKitHitTestResult *hit_test_result,
				gpointer user_data) {

	/* Returning true without creating a custom context menu results in no context
	 * menu being shown. Thus, we are returning the opposite of debug_mode to get
	 * desired result (which is only show menu when debug_mode is enabled.
	 */
	return (! debug_mode);
}


static gboolean
check_theme_heartbeat(void) {
	if (! heartbeat && ! heartbeat_exit) {
		/* Theme heartbeat not received. We assume that an error has occurred
		 * which broke script execution. We will fallback to the simple theme
		 * so the user won't be stuck with a broken login screen.
		 */
		g_warning("[ERROR] :: A problem was detected with the current theme. Falling back to simple theme...");
		webkit_web_view_load_uri(
			WEBKIT_WEB_VIEW(web_view),
			g_strdup_printf("file://%ssimple/index.html", THEME_DIR)
		);
	}

	heartbeat = FALSE;

	return heartbeat;
}


/**
 * Callback for Theme Heartbeat.
 */
static void
theme_heartbeat_handler(void) {
	if (! heartbeat) {
		/* Setup g_timeout callback for theme heartbeat check */
		g_timeout_add_seconds(8, (GSourceFunc) check_theme_heartbeat, NULL);
		heartbeat = TRUE;
		heartbeat_exit = FALSE;
	}
}


/**
 * Before starting the user's session, the web process will send a signal to exit the hearbeat
 * in order to prevent a race condition while the greeter is shutting down.
 */
static void
theme_heartbeat_exit_handler(void) {
	heartbeat_exit = TRUE;
}


/**
 * Makes the greeter behave a bit more like a screensaver if it was launched as
 * a lock-screen by blanking the screen.
 */
static void
lock_hint_enabled_handler(void) {
	Display *display = gdk_x11_display_get_xdisplay(default_display);
	config_timeout = (0 != config_timeout) ? config_timeout : 300;

	XGetScreenSaver(display, &timeout, &interval, &prefer_blanking, &allow_exposures);
	XForceScreenSaver(display, ScreenSaverActive);
	XSetScreenSaver(display, config_timeout, 0, PreferBlanking, DefaultExposures);
}


/**
 * Message received callback.
 *
 * Receives messages from our web extension process and calls appropriate handlers.
 *
 * @param manager   The user content manager instance that was created in #main.
 * @param message   The message sent from web extension process.
 * @param user_data Data that is private to the current user.
 */
static void
message_received_cb(WebKitUserContentManager *manager,
					WebKitJavascriptResult *message,
					gpointer user_data) {

	gchar *message_str;
	JSGlobalContextRef context;
	JSValueRef message_val;
	JSStringRef js_str_val;
	gsize message_str_length;

	context = webkit_javascript_result_get_global_context(message);
	message_val = webkit_javascript_result_get_value(message);

	if (JSValueIsString(context, message_val)) {
		js_str_val = JSValueToStringCopy(context, message_val, NULL);
		message_str_length = JSStringGetMaximumUTF8CStringSize(js_str_val);
		message_str = (gchar *)g_malloc (message_str_length);
		JSStringGetUTF8CString(js_str_val, message_str, message_str_length);
		JSStringRelease(js_str_val);

	} else {
		message_str = "";
		printf("Error running javascript: unexpected return value");
	}

	if (strcmp(message_str, "LockHint") == 0) {
		lock_hint_enabled_handler();

	} else if (strcmp(message_str, "Heartbeat") == 0) {
		theme_heartbeat_handler();

	} else if (strcmp(message_str, "Heartbeat::Exit") == 0) {
		theme_heartbeat_exit_handler();
	} else {
		printf("UI PROCESS - message_received_cb(): no match!");
	}

	g_free(message_str);

}


static void
quit_cb(void) {
	gtk_widget_destroy(window);
	gtk_main_quit();
}


static gchar *
rtrim_comments(gchar *str) {
	gchar *ptr = NULL;

	ptr = strchr(str, '#');

	if (NULL != ptr) {
		*ptr = '\0';
	}

	return g_strstrip(str);
}


static void
javascript_bundle_injection_setup() {
	WebKitUserScript *bundle;
	GBytes *data;
	guint8 *data_as_guint;
	gsize data_size;
	gchar *script;

	data = g_resource_lookup_data(
		greeter_resources,
		GRESOURCE_PATH "/js/bundle.js",
		G_RESOURCE_LOOKUP_FLAGS_NONE,
		NULL
	);

	data_as_guint = g_bytes_unref_to_data(data, &data_size);
	script = g_strdup_printf("%s", data_as_guint);

	bundle = webkit_user_script_new(
		script,
		WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
		WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
		NULL, /* URL whitelist pattern */
		NULL  /* URL blacklist pattern */
	);

	webkit_user_content_manager_add_script(WEBKIT_USER_CONTENT_MANAGER(manager), bundle);
}


int
main(int argc, char **argv) {
	GdkScreen *screen;
	GdkWindow *root_window;
	GdkRectangle geometry;
	GKeyFile *keyfile;
	gchar *theme;
	GError *err = NULL;
	GdkRGBA bg_color;
	WebKitWebContext *context;
	GtkCssProvider *css_provider;
	WebKitCookieManager *cookie_manager;

	/* Prevent memory from being swapped out, since we see unencrypted passwords. */
	mlockall (MCL_CURRENT | MCL_FUTURE);

	/* https://goo.gl/vDFwFe */
	g_setenv ("GDK_CORE_DEVICE_EVENTS", "1", TRUE);

	/* Initialize i18n */
	bindtextdomain(GETTEXT_PACKAGE, LOCALE_DIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
	textdomain(GETTEXT_PACKAGE);

	gtk_init(&argc, &argv);

	g_unix_signal_add(SIGTERM, (GSourceFunc) quit_cb, NULL);
	g_unix_signal_add(SIGINT, (GSourceFunc) quit_cb, NULL);
	g_unix_signal_add(SIGHUP, (GSourceFunc) quit_cb, NULL);

	/* BEGIN Greeter Config File */
	keyfile = g_key_file_new();

	g_key_file_load_from_file(
		keyfile,
		CONFIG_DIR "/lightdm-webkit2-greeter.conf",
		G_KEY_FILE_NONE,
		NULL
	);

	/* TODO: Handle config values and fallbacks some other way, this is garbage! */
	theme = g_key_file_get_string(keyfile, "greeter", "webkit_theme", &err);

	if ( NULL != err) {
		g_clear_error(&err);
		theme = g_key_file_get_string(keyfile, "greeter", "webkit-theme", &err);

		if ( NULL != err) {
			g_clear_error(&err);
			theme = "antergos";
		}
	}

	theme = rtrim_comments(theme);
	config_timeout = g_key_file_get_integer(keyfile, "greeter", "screensaver_timeout", &err);

	if ( NULL != err) {
		g_clear_error(&err);
		config_timeout = g_key_file_get_integer(keyfile, "greeter", "screensaver-timeout", &err);

		if ( NULL != err) {
			g_error_free(err);
			config_timeout = 300;
		}
	}

	debug_mode = g_key_file_get_boolean(keyfile, "greeter", "debug_mode", NULL);

	if ( NULL != err) {
		g_clear_error(&err);
		debug_mode = FALSE;
	}
	/* END Greeter Config File */

	/* Set default cursor */
	root_window = gdk_get_default_root_window();
	default_display = gdk_display_get_default();

	gdk_window_set_cursor(root_window, gdk_cursor_new_for_display(default_display, GDK_LEFT_PTR));

	/* Set the GTK theme to Adwaita */
	g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita", NULL);

	/* Setup the main window */
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	screen = gtk_window_get_screen(GTK_WINDOW(window));

	gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

	#ifdef HAS_GTK_3_22
		GdkMonitor *monitor = gdk_display_get_primary_monitor(default_display);
		gdk_monitor_get_geometry(monitor, &geometry);
	#else
		gdk_screen_get_monitor_geometry(screen, gdk_screen_get_primary_monitor(screen), &geometry);
	#endif

	gtk_window_set_default_size(GTK_WINDOW(window), geometry.width, geometry.height);
	gtk_window_move(GTK_WINDOW(window), geometry.x, geometry.y);

	/* Setup CSS provider. We use CSS to set the window background to black instead
	 * of default white so the screen doesnt flash during startup.
	 */
	greeter_resources = greeter_resources_get_resource();
	css_provider = gtk_css_provider_new();

	g_resources_register(greeter_resources);
	gtk_css_provider_load_from_resource(
		GTK_CSS_PROVIDER(css_provider),
		"/com/antergos/lightdm-webkit2-greeter/css/style.css"
	);
	gtk_style_context_add_provider_for_screen(
		screen,
		GTK_STYLE_PROVIDER(css_provider),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
	);

	/* Register and connect handler that will set the web extensions directory
	 * so webkit can find our extension.
	 */
	context = webkit_web_context_get_default();
	g_signal_connect(WEBKIT_WEB_CONTEXT(context),
					 "initialize-web-extensions",
					 G_CALLBACK(initialize_web_extensions_cb), NULL);

	/* Set cookie policy */
	cookie_manager = webkit_web_context_get_cookie_manager(context);
	webkit_cookie_manager_set_accept_policy(cookie_manager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);

	/* Register and connect handler of any messages we send from our web extension process. */
	manager = webkit_user_content_manager_new();
	g_signal_connect(manager, "script-message-received::GreeterBridge", G_CALLBACK(message_received_cb), NULL);
	webkit_user_content_manager_register_script_message_handler(manager, "GreeterBridge");

	javascript_bundle_injection_setup();

	/* Create the web_view */
	web_view = webkit_web_view_new_with_user_content_manager(manager);

	/* Set the web_view's settings. */
	create_new_webkit_settings_object();
	webkit_web_view_set_settings(WEBKIT_WEB_VIEW(web_view), WEBKIT_SETTINGS(webkit_settings));

	/* The default background color of the web_view is white which causes a flash effect when the greeter starts.
	 * We make it black instead. This only applies when the theme hasn't set the body background via CSS.
	 */
	gdk_rgba_parse(&bg_color, "#000000");
	webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(web_view), gdk_rgba_copy(&bg_color));

	/* Maybe disable the context (right-click) menu. */
	g_signal_connect(WEBKIT_WEB_VIEW(web_view), "context-menu", G_CALLBACK(context_menu_cb), NULL);

	/* There's no turning back now, let's go! */
	gtk_container_add(GTK_CONTAINER(window), web_view);
	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(web_view),
							 g_strdup_printf("file://%s/%s/index.html", THEME_DIR, theme));

	gtk_widget_show_all(window);
	gtk_widget_set_can_focus(GTK_WIDGET(web_view), TRUE);
	gtk_widget_grab_focus(GTK_WIDGET(web_view));

	g_debug("Entering Gtk loop...");
	gtk_main();
	g_debug("Exited Gtk loop.");

	return 0;
}
