/* Copyright (C) 2006-2007 Peter Zelezny. */

#include <string.h>
#include "../common/hexchat-plugin.h"
#include "../common/hexchat.h"
#include "../common/hexchatc.h"
#include "../common/inbound.h"
#include "../common/server.h"
#include "../common/fe.h"
#include "../common/util.h"
#include "../common/outbound.h"
#include "fe-gtk.h"
#include "pixmaps.h"
#include "maingui.h"
#include "menu.h"
#include <gtk/gtk.h>

#ifndef WIN32
#include <unistd.h>
#endif

#ifdef USE_LIBNOTIFY
#include <libnotify/notify.h>
#ifndef NOTIFY_CHECK_VERSION
#define NOTIFY_CHECK_VERSION(x,y,z) 0
#endif
#if NOTIFY_CHECK_VERSION(0,7,0)
#define XC_NOTIFY_NEW(a,b,c,d) notify_notification_new(a,b,c)
#else
#define XC_NOTIFY_NEW(a,b,c,d) notify_notification_new(a,b,c,d)
#endif
#endif

typedef enum	/* current icon status */
{
	TS_NONE,
	TS_MESSAGE,
	TS_HIGHLIGHT,
	TS_FILEOFFER,
	TS_CUSTOM /* plugin */
} TrayStatus;

typedef enum
{
	WS_FOCUSED,
	WS_NORMAL,
	WS_HIDDEN
} WinStatus;

typedef GdkPixbuf* TrayIcon;
#define tray_icon_from_file(f) gdk_pixbuf_new_from_file(f,NULL)
#define tray_icon_free(i) g_object_unref(i)

#define ICON_NORMAL pix_hexchat
#define ICON_MSG pix_tray_message
#define ICON_HILIGHT pix_tray_highlight
#define ICON_FILE pix_tray_fileoffer
#define TIMEOUT 500

static GtkStatusIcon *sticon;
static gint flash_tag;
static TrayStatus tray_status;
#ifdef WIN32
static guint tray_menu_timer;
static gint64 tray_menu_inactivetime;
#endif
static hexchat_plugin *ph;

static TrayIcon custom_icon1;
static TrayIcon custom_icon2;

static int tray_priv_count = 0;
static int tray_pub_count = 0;
static int tray_hilight_count = 0;
static int tray_file_count = 0;


void tray_apply_setup (void);


static WinStatus
tray_get_window_status (void)
{
	const char *st;

	st = hexchat_get_info (ph, "win_status");

	if (!st)
		return WS_HIDDEN;

	if (!strcmp (st, "active"))
		return WS_FOCUSED;

	if (!strcmp (st, "hidden"))
		return WS_HIDDEN;

	return WS_NORMAL;
}

static int
tray_count_channels (void)
{
	int cons = 0;
	GSList *list;
	session *sess;

	for (list = sess_list; list; list = list->next)
	{
		sess = list->data;
		if (sess->server->connected && sess->channel[0] &&
			 sess->type == SESS_CHANNEL)
			cons++;
	}
	return cons;
}

static int
tray_count_networks (void)
{
	int cons = 0;
	GSList *list;

	for (list = serv_list; list; list = list->next)
	{
		if (((server *)list->data)->connected)
			cons++;
	}
	return cons;
}

void
fe_tray_set_tooltip (const char *text)
{
	if (sticon)
		gtk_status_icon_set_tooltip (sticon, text);
}

void
fe_tray_set_balloon (const char *title, const char *text)
{
#ifndef WIN32
#if 0
	const char *argv[8];
	const char *path;
	char time[16];
#endif
	WinStatus ws;

	/* no balloons if the window is focused */
	ws = tray_get_window_status ();
	if (ws == WS_FOCUSED)
		return;

	/* bit 1 of flags means "no balloons unless hidden/iconified" */
	if (ws != WS_HIDDEN && prefs.hex_gui_tray_quiet)
		return;

	/* FIXME: this should close the current balloon */
	if (!text)
		return;

#ifdef USE_LIBNOTIFY
	NotifyNotification *notification;
	char *notify_text, *notify_title;

	if (!notify_is_initted())
		notify_init(PACKAGE_NAME);

	notify_text = strip_color (text, -1, STRIP_ALL|STRIP_ESCMARKUP);
	notify_title = strip_color (title, -1, STRIP_ALL);

	notification = XC_NOTIFY_NEW (notify_title, notify_text, HEXCHATSHAREDIR "/pixmaps/hexchat.png", NULL);

	g_free ((char *)notify_title);
	g_free ((char *)notify_text);

	notify_notification_set_timeout (notification, prefs.hex_input_balloon_time*1000);
	notify_notification_show (notification, NULL);

	g_object_unref (notification);
#endif
#endif
}

static void
tray_set_balloonf (const char *text, const char *format, ...)
{
	va_list args;
	char *buf;

	va_start (args, format);
	buf = g_strdup_vprintf (format, args);
	va_end (args);

	fe_tray_set_balloon (buf, text);
	g_free (buf);
}

static void
tray_set_tipf (const char *format, ...)
{
	va_list args;
	char *buf;

	va_start (args, format);
	buf = g_strdup_vprintf (format, args);
	va_end (args);

	fe_tray_set_tooltip (buf);
	g_free (buf);
}

static void
tray_stop_flash (void)
{
	int nets, chans;

	if (flash_tag)
	{
		g_source_remove (flash_tag);
		flash_tag = 0;
	}

	if (sticon)
	{
		gtk_status_icon_set_from_pixbuf (sticon, ICON_NORMAL);
		nets = tray_count_networks ();
		chans = tray_count_channels ();
		if (nets)
			tray_set_tipf (_(DISPLAY_NAME": Connected to %u networks and %u channels"),
								nets, chans);
		else
			tray_set_tipf (DISPLAY_NAME": %s", _("Not connected."));
	}

	if (custom_icon1)
	{
		tray_icon_free (custom_icon1);
		custom_icon1 = NULL;
	}

	if (custom_icon2)
	{
		tray_icon_free (custom_icon2);
		custom_icon2 = NULL;
	}

	tray_status = TS_NONE;
}

static void
tray_reset_counts (void)
{
	tray_priv_count = 0;
	tray_pub_count = 0;
	tray_hilight_count = 0;
	tray_file_count = 0;
}

static int
tray_timeout_cb (TrayIcon icon)
{
	if (custom_icon1)
	{
		if (gtk_status_icon_get_pixbuf (sticon) == custom_icon1)
		{
			if (custom_icon2)
				gtk_status_icon_set_from_pixbuf (sticon, custom_icon2);
			else
				gtk_status_icon_set_from_pixbuf (sticon, ICON_NORMAL);
		}
		else
		{
			gtk_status_icon_set_from_pixbuf (sticon, custom_icon1);
		}
	}
	else
	{
		if (gtk_status_icon_get_pixbuf (sticon) == ICON_NORMAL)
			gtk_status_icon_set_from_pixbuf (sticon, icon);
		else
			gtk_status_icon_set_from_pixbuf (sticon, ICON_NORMAL);
	}
	return 1;
}

static void
tray_set_flash (TrayIcon icon)
{
	if (!sticon)
		return;

	/* already flashing the same icon */
	if (flash_tag && gtk_status_icon_get_pixbuf (sticon) == icon)
		return;

	/* no flashing if window is focused */
	if (tray_get_window_status () == WS_FOCUSED)
		return;

	tray_stop_flash ();

	gtk_status_icon_set_from_pixbuf (sticon, icon);
	if (prefs.hex_gui_tray_blink)
		flash_tag = g_timeout_add (TIMEOUT, (GSourceFunc) tray_timeout_cb, icon);
}

void
fe_tray_set_flash (const char *filename1, const char *filename2, int tout)
{
	tray_apply_setup ();
	if (!sticon)
		return;

	tray_stop_flash ();

	if (tout == -1)
		tout = TIMEOUT;

	custom_icon1 = tray_icon_from_file (filename1);
	if (filename2)
		custom_icon2 = tray_icon_from_file (filename2);

	gtk_status_icon_set_from_pixbuf (sticon, custom_icon1);
	flash_tag = g_timeout_add (tout, (GSourceFunc) tray_timeout_cb, NULL);
	tray_status = TS_CUSTOM;
}

void
fe_tray_set_icon (feicon icon)
{
	tray_apply_setup ();
	if (!sticon)
		return;

	tray_stop_flash ();

	switch (icon)
	{
	case FE_ICON_NORMAL:
		break;
	case FE_ICON_MESSAGE:
	case FE_ICON_PRIVMSG:
		tray_set_flash (ICON_MSG);
		break;
	case FE_ICON_HIGHLIGHT:
		tray_set_flash (ICON_HILIGHT);
		break;
	case FE_ICON_FILEOFFER:
		tray_set_flash (ICON_FILE);
	}
}

void
fe_tray_set_file (const char *filename)
{
	tray_apply_setup ();
	if (!sticon)
		return;

	tray_stop_flash ();

	if (filename)
	{
		custom_icon1 = tray_icon_from_file (filename);
		gtk_status_icon_set_from_pixbuf (sticon, custom_icon1);
		tray_status = TS_CUSTOM;
	}
}

gboolean
tray_toggle_visibility (gboolean force_hide)
{
	static int x, y;
	static GdkScreen *screen;
	GtkWindow *win;

	if (!sticon)
		return FALSE;

	/* ph may have an invalid context now */
	hexchat_set_context (ph, hexchat_find_context (ph, NULL, NULL));

	win = GTK_WINDOW (hexchat_get_info (ph, "gtkwin_ptr"));

	tray_stop_flash ();
	tray_reset_counts ();

	if (!win)
		return FALSE;

#if GTK_CHECK_VERSION(2,20,0)
	if (force_hide || gtk_widget_get_visible (GTK_WIDGET (win)))
#else
	if (force_hide || GTK_WIDGET_VISIBLE (win))
#endif
	{
		if (prefs.hex_gui_tray_away)
			hexchat_command (ph, "ALLSERV AWAY");
		gtk_window_get_position (win, &x, &y);
		screen = gtk_window_get_screen (win);
		gtk_widget_hide (GTK_WIDGET (win));
	}
	else
	{
		if (prefs.hex_gui_tray_away)
			hexchat_command (ph, "ALLSERV BACK");
		gtk_window_set_screen (win, screen);
		gtk_window_move (win, x, y);
		gtk_widget_show (GTK_WIDGET (win));
		gtk_window_present (win);
	}

	return TRUE;
}

static void
tray_menu_restore_cb (GtkWidget *item, gpointer userdata)
{
	tray_toggle_visibility (FALSE);
}

static void
tray_menu_quit_cb (GtkWidget *item, gpointer userdata)
{
	mg_open_quit_dialog (FALSE);
}

/* returns 0-mixed 1-away 2-back */

static int
tray_find_away_status (void)
{
	GSList *list;
	server *serv;
	int away = 0;
	int back = 0;

	for (list = serv_list; list; list = list->next)
	{
		serv = list->data;

		if (serv->is_away || serv->reconnect_away)
			away++;
		else
			back++;
	}

	if (away && back)
		return 0;

	if (away)
		return 1;

	return 2;
}

static void
tray_foreach_server (GtkWidget *item, char *cmd)
{
	GSList *list;
	server *serv;

	for (list = serv_list; list; list = list->next)
	{
		serv = list->data;
		if (serv->connected)
			handle_command (serv->server_session, cmd, FALSE);
	}
}

static GtkWidget *
tray_make_item (GtkWidget *menu, char *label, void *callback, void *userdata)
{
	GtkWidget *item;

	if (label)
		item = gtk_menu_item_new_with_mnemonic (label);
	else
		item = gtk_menu_item_new ();
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (G_OBJECT (item), "activate",
							G_CALLBACK (callback), userdata);
	gtk_widget_show (item);

	return item;
}

static void
tray_toggle_cb (GtkCheckMenuItem *item, unsigned int *setting)
{
	*setting = item->active;
}

static void
blink_item (unsigned int *setting, GtkWidget *menu, char *label)
{
	menu_toggle_item (label, menu, tray_toggle_cb, setting, *setting);
}

static void
tray_menu_destroy (GtkWidget *menu, gpointer userdata)
{
	gtk_widget_destroy (menu);
	g_object_unref (menu);
#ifdef WIN32
	g_source_remove (tray_menu_timer);
#endif
}

#ifdef WIN32
static void
tray_menu_enter_cb (GtkWidget *menu)
{
	tray_menu_inactivetime = 0;
}

static void
tray_menu_left_cb (GtkWidget *menu)
{
	tray_menu_inactivetime = g_get_real_time ();
}

static void
tray_check_hide (GtkWidget *menu)
{
	if (tray_menu_inactivetime && g_get_real_time () - tray_menu_inactivetime  >= 2000000)
	{
		tray_menu_destroy (menu, NULL);
	}
}

static void
tray_menu_settings (GtkWidget * wid, gpointer none)
{
	extern void setup_open (void);
	setup_open ();
}
#endif

static void
tray_menu_cb (GtkWidget *widget, guint button, guint time, gpointer userdata)
{
	static GtkWidget *menu;
	GtkWidget *submenu;
	GtkWidget *item;
	int away_status;

	/* ph may have an invalid context now */
	hexchat_set_context (ph, hexchat_find_context (ph, NULL, NULL));

	/* close any old menu */
	if (G_IS_OBJECT (menu))
	{
		tray_menu_destroy (menu, NULL);
	}

	menu = gtk_menu_new ();
	/*gtk_menu_set_screen (GTK_MENU (menu), gtk_widget_get_screen (widget));*/

	if (tray_get_window_status () == WS_HIDDEN)
		tray_make_item (menu, _("_Restore Window"), tray_menu_restore_cb, NULL);
	else
		tray_make_item (menu, _("_Hide Window"), tray_menu_restore_cb, NULL);
	tray_make_item (menu, NULL, tray_menu_quit_cb, NULL);

#ifndef WIN32 /* submenus are buggy on win32 */
	submenu = mg_submenu (menu, _("_Blink on"));
	blink_item (&prefs.hex_input_tray_chans, submenu, _("Channel Message"));
	blink_item (&prefs.hex_input_tray_priv, submenu, _("Private Message"));
	blink_item (&prefs.hex_input_tray_hilight, submenu, _("Highlighted Message"));
	/*blink_item (BIT_FILEOFFER, submenu, _("File Offer"));*/

	submenu = mg_submenu (menu, _("_Change status"));
#else /* so show away/back in main tray menu */
	submenu = menu;
#endif

	away_status = tray_find_away_status ();
	item = tray_make_item (submenu, _("_Away"), tray_foreach_server, "away");
	if (away_status == 1)
		gtk_widget_set_sensitive (item, FALSE);
	item = tray_make_item (submenu, _("_Back"), tray_foreach_server, "back");
	if (away_status == 2)
		gtk_widget_set_sensitive (item, FALSE);

	menu_add_plugin_items (menu, "\x5$TRAY", NULL);
#ifdef WIN32
	tray_make_item (menu, NULL, tray_menu_quit_cb, NULL);
	mg_create_icon_item (_("_Preferences"), GTK_STOCK_PREFERENCES, menu, tray_menu_settings, NULL);
#endif
	tray_make_item (menu, NULL, tray_menu_quit_cb, NULL);
	mg_create_icon_item (_("_Quit"), GTK_STOCK_QUIT, menu, tray_menu_quit_cb, NULL);

	g_object_ref (menu);
	g_object_ref_sink (menu);
	g_object_unref (menu);
	g_signal_connect (G_OBJECT (menu), "selection-done",
							G_CALLBACK (tray_menu_destroy), NULL);
#ifdef WIN32
	g_signal_connect (G_OBJECT (menu), "leave-notify-event",
							G_CALLBACK (tray_menu_left_cb), NULL);
	g_signal_connect (G_OBJECT (menu), "enter-notify-event",
							G_CALLBACK (tray_menu_enter_cb), NULL);

	tray_menu_timer = g_timeout_add(500, (GSourceFunc) tray_check_hide, menu);
#endif

	gtk_menu_popup (GTK_MENU (menu), NULL, NULL, gtk_status_icon_position_menu,
						 userdata, button, time);
}

static void
tray_init (void)
{
	flash_tag = 0;
	tray_status = TS_NONE;
	custom_icon1 = NULL;
	custom_icon2 = NULL;

	sticon = gtk_status_icon_new_from_pixbuf (ICON_NORMAL);
	if (!sticon)
		return;

	g_signal_connect (G_OBJECT (sticon), "popup-menu",
							G_CALLBACK (tray_menu_cb), sticon);

	g_signal_connect (G_OBJECT (sticon), "activate",
							G_CALLBACK (tray_menu_restore_cb), NULL);
}

static int
tray_hilight_cb (char *word[], void *userdata)
{
	/*if (tray_status == TS_HIGHLIGHT)
		return HEXCHAT_EAT_NONE;*/

	if (prefs.hex_input_tray_hilight && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
	{
		tray_set_flash (ICON_HILIGHT);

		/* FIXME: hides any previous private messages */
		tray_hilight_count++;
		if (tray_hilight_count == 1)
			tray_set_tipf (_(DISPLAY_NAME": Highlighted message from: %s (%s)"),
								word[1], hexchat_get_info (ph, "channel"));
		else
			tray_set_tipf (_(DISPLAY_NAME": %u highlighted messages, latest from: %s (%s)"),
								tray_hilight_count, word[1], hexchat_get_info (ph, "channel"));
	}

	if (prefs.hex_input_balloon_hilight && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
		tray_set_balloonf (word[2], _(DISPLAY_NAME": Highlighted message from: %s (%s)"),
								 word[1], hexchat_get_info (ph, "channel"));

	return HEXCHAT_EAT_NONE;
}

static int
tray_message_cb (char *word[], void *userdata)
{
	if (/*tray_status == TS_MESSAGE ||*/ tray_status == TS_HIGHLIGHT)
		return HEXCHAT_EAT_NONE;

	if (prefs.hex_input_tray_chans && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
	{
		tray_set_flash (ICON_MSG);

		tray_pub_count++;
		if (tray_pub_count == 1)
			tray_set_tipf (_(DISPLAY_NAME": New public message from: %s (%s)"),
								word[1], hexchat_get_info (ph, "channel"));
		else
			tray_set_tipf (_(DISPLAY_NAME": %u new public messages."), tray_pub_count);
	}

	if (prefs.hex_input_balloon_chans && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
		tray_set_balloonf (word[2], _(DISPLAY_NAME": New public message from: %s (%s)"),
								 word[1], hexchat_get_info (ph, "channel"));

	return HEXCHAT_EAT_NONE;
}

static void
tray_priv (char *from, char *text)
{
	const char *network;

	if (alert_match_word (from, prefs.hex_irc_no_hilight))
		return;

	tray_set_flash (ICON_MSG);

	network = hexchat_get_info (ph, "network");
	if (!network)
		network = hexchat_get_info (ph, "server");

	tray_priv_count++;
	if (tray_priv_count == 1)
		tray_set_tipf (_(DISPLAY_NAME": Private message from: %s (%s)"),
							from, network);
	else
		tray_set_tipf (_(DISPLAY_NAME": %u private messages, latest from: %s (%s)"),
							tray_priv_count, from, network);

	if (prefs.hex_input_balloon_priv && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
		tray_set_balloonf (text, _(DISPLAY_NAME": Private message from: %s (%s)"),
								 from, network);
}

static int
tray_priv_cb (char *word[], void *userdata)
{
	/*if (tray_status == TS_HIGHLIGHT)
		return HEXCHAT_EAT_NONE;*/

	if (prefs.hex_input_tray_priv && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
		tray_priv (word[1], word[2]);

	return HEXCHAT_EAT_NONE;
}

static int
tray_invited_cb (char *word[], void *userdata)
{
	/*if (tray_status == TS_HIGHLIGHT)
		return HEXCHAT_EAT_NONE;*/

	if (prefs.hex_input_tray_priv && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
		tray_priv (word[2], "Invited");

	return HEXCHAT_EAT_NONE;
}

static int
tray_dcc_cb (char *word[], void *userdata)
{
	const char *network;

/*	if (tray_status == TS_FILEOFFER)
		return HEXCHAT_EAT_NONE;*/

	network = hexchat_get_info (ph, "network");
	if (!network)
		network = hexchat_get_info (ph, "server");

	if (prefs.hex_input_tray_priv && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
	{
		tray_set_flash (ICON_FILE);

		tray_file_count++;
		if (tray_file_count == 1)
			tray_set_tipf (_(DISPLAY_NAME": File offer from: %s (%s)"),
								word[1], network);
		else
			tray_set_tipf (_(DISPLAY_NAME": %u file offers, latest from: %s (%s)"),
								tray_file_count, word[1], network);
	}

	if (prefs.hex_input_balloon_priv && (!prefs.hex_away_omit_alerts || tray_find_away_status () != 1))
		tray_set_balloonf ("", _(DISPLAY_NAME": File offer from: %s (%s)"),
								word[1], network);

	return HEXCHAT_EAT_NONE;
}

static int
tray_focus_cb (char *word[], void *userdata)
{
	tray_stop_flash ();
	tray_reset_counts ();
	return HEXCHAT_EAT_NONE;
}

static void
tray_cleanup (void)
{
	tray_stop_flash ();

	if (sticon)
	{
		g_object_unref ((GObject *)sticon);
		sticon = NULL;
	}
}

void
tray_apply_setup (void)
{
	if (sticon)
	{
		if (!prefs.hex_gui_tray)
			tray_cleanup ();
	}
	else
	{
		if (prefs.hex_gui_tray && !hextray_mode ())
			tray_init ();
	}
}

int
tray_plugin_init (hexchat_plugin *plugin_handle, char **plugin_name,
				char **plugin_desc, char **plugin_version, char *arg)
{
	/* we need to save this for use with any hexchat_* functions */
	ph = plugin_handle;

	*plugin_name = "";
	*plugin_desc = "";
	*plugin_version = "";

	hexchat_hook_print (ph, "Channel Msg Hilight", -1, tray_hilight_cb, NULL);
	hexchat_hook_print (ph, "Channel Action Hilight", -1, tray_hilight_cb, NULL);

	hexchat_hook_print (ph, "Channel Message", -1, tray_message_cb, NULL);
	hexchat_hook_print (ph, "Channel Action", -1, tray_message_cb, NULL);
	hexchat_hook_print (ph, "Channel Notice", -1, tray_message_cb, NULL);

	hexchat_hook_print (ph, "Private Message", -1, tray_priv_cb, NULL);
	hexchat_hook_print (ph, "Private Message to Dialog", -1, tray_priv_cb, NULL);
	hexchat_hook_print (ph, "Notice", -1, tray_priv_cb, NULL);
	hexchat_hook_print (ph, "Invited", -1, tray_invited_cb, NULL);

	hexchat_hook_print (ph, "DCC Offer", -1, tray_dcc_cb, NULL);

	hexchat_hook_print (ph, "Focus Window", -1, tray_focus_cb, NULL);

	if (prefs.hex_gui_tray && !hextray_mode ())
		tray_init ();

	return 1;       /* return 1 for success */
}

int
tray_plugin_deinit (hexchat_plugin *plugin_handle)
{
#ifdef WIN32
	tray_cleanup ();
#elif defined(USE_LIBNOTIFY)
	notify_uninit ();
#endif
	return 1;
}
