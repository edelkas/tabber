/*
 * main.cpp - Entry point of the graphical front-end.
 *
 * Dear ImGui on GLFW + OpenGL 3, both built from source under vendor/. One
 * full-viewport panel: a button that refreshes the catalogue, a sortable table
 * of every custom tab with a button per row that downloads, installs or
 * uninstalls it, and a bar along the bottom saying what has just happened and
 * what the game has in it.
 *
 * This is a second front-end onto the library in src/, not a second copy of
 * it. Every button runs the same call the matching CLI command runs, and the
 * two agree on what a listing looks like because the column headers and the
 * cells that are not shown verbatim live in digest.h.
 *
 * The window has no frame from the system. Its title bar is drawn with the
 * rest of the panel, and the three things a frame used to do â€” the buttons,
 * dragging the window, pulling on its edges â€” are done by hand under "The
 * window's own frame" below.
 *
 * A frame is drawn when there is a reason to draw one â€” an event, or the
 * timer under IDLE_SECONDS below â€” and the loop sleeps between them rather
 * than redrawing an unchanging window sixty times a second. What it sleeps in
 * is worth a look too: see "Pacing the frames".
 *
 * The work happens on this thread, so a download or an install freezes the
 * window while it runs. What the frame before it draws is a "working" overlay,
 * so at least the reason is on screen; moving it off the main thread is the
 * next thing this file needs.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

/* Brings in the system's OpenGL header, so it goes after the backends. */
#include <GLFW/glfw3.h>

/* Baked ForkAwesome font and codepoint names */
#include "font_forkawesome.h"
#include "IconsForkAwesome.h"

/* The tool's own library. Its headers carry their own extern "C". */
#include "config.h"
#include "digest.h"
#include "install.h"
#include "log.h"
#include "paths.h"
#include "platform.h"
#include "tabs.h"
#include "update.h"
#include "usage.h"
#include "util.h"
#include "version.h"

/* Window and context. GL 3.2 is the floor: it is what the OpenGL 3 backend
 * asks for and the oldest core profile macOS will hand out. */
static const char *WINDOW_TITLE      = TABBER_NAME " " TABBER_VERSION;
static const int   WINDOW_WIDTH      = 800;
static const int   WINDOW_HEIGHT     = 400;
static const int   GL_VERSION_MAJOR  = 3;
static const int   GL_VERSION_MINOR  = 2;
static const int   SWAP_INTERVAL     = 1;  /* vsync, where the driver is trusted with it */

/* ProggyClean's own size, and what the icons merged into it are measured
 * against. Not the size anything is drawn at: FontScaleDpi settles that. */
static const float FONT_SIZE         = 13.0f;

/*
 * Waiting instead of spinning. With nothing happening the loop sleeps until
 * something does, waking on its own every IDLE_SECONDS to notice what changes
 * without an event to announce it â€” the savefile the game rewrites, and the
 * clock LAST USED counts from.
 *
 * SETTLE_FRAMES is how many frames follow the last thing that happened. Dear
 * ImGui answers a click over the frames after the one that received it, and
 * the work a click asks for only starts once the overlay saying so has been
 * drawn, so a woken loop owes a few frames before it may sleep again.
 */
static const double IDLE_SECONDS  = 0.25;
static const int    SETTLE_FRAMES = 3;

/*
 * The window wears no frame of its own: GLFW is asked for an undecorated one
 * and the bar across the top is drawn like everything else in it. That buys
 * one look on every platform, and costs the three things the frame did for
 * free â€” the buttons, dragging the window about, and pulling on its edges â€”
 * all of which are put back by hand further down.
 *
 * These are in the units the style is written in and are scaled with it.
 */
static const float TITLE_BUTTON_ASPECT = 1.6f;  /* button width, in bar heights */
static const float TITLE_ICON_SIZE     = 0.28f; /* glyph size, likewise        */
static const float RESIZE_BORDER       = 6.0f;  /* how near an edge grabs it   */
static const int   MIN_WIDTH           = 480;
static const int   MIN_HEIGHT          = 240;

/* Milliseconds to idle for per frame while the window is minimised, when there
 * is nothing to draw and polling would otherwise burn a core. */
static const int   ICONIFIED_SLEEP_MS = 10;

/*
 * How often the per-tab state is worked out again. Everything in it can change
 * without tabber being told â€” the game rewrites the savefile as it is played,
 * and the CLI can install a tab from another window â€” so it is read afresh on
 * a timer rather than only when this program does something.
 */
static const long long REFRESH_SECONDS = 5 * 60;

/*
 * How often the state file is asked whether a look for a newer tabber is due.
 * Only the asking happens this often: what it answers is governed by
 * UPDATE_CHECK_HOURS, so GitHub is reached at most once a day. The window is
 * the sort of thing that gets left open for a week, which is why this is on a
 * timer at all and not only done at startup.
 */
static const long long UPDATE_POLL_SECONDS = 30 * 60;

/*
 * The binary an update replaced cannot be deleted until the process that was
 * running it has gone â€” and on Windows that process is the one that started
 * this one, still shutting down as this starts. So the sweep at startup is
 * made once more this many seconds in, and only then left to the next run.
 */
static const double SWEEP_RETRY_SECONDS = 2.0;

/*
 * How wide the text in a dialog may run before it wraps. A dialog sizes itself
 * to what it holds, and what it holds can be a message from anywhere down the
 * library â€” a URL, a path, a hash â€” which without this would push the box
 * wider than the window it is centred in.
 */
static const float DIALOG_WRAP_WIDTH = 420.0f;

/*
 * How many rows the corner opposite the banner holds: the About box, tabber's
 * own updates, and the catalogue of custom tabs. Named because it is also how
 * far down the corner reaches, which is not always as far as the banner does.
 */
static const int CORNER_ROWS = 3;

/*
 * How many buttons the widest of those rows holds â€” the strip along the top,
 * which is the theme, the settings and the About box. Also what keeps the
 * rows' buttons apart from one another as far as Dear ImGui is concerned:
 * every seat in the corner has its own number, row by row.
 */
static const int CORNER_SLOTS = 3;

/* ASCII art banner */
static const char* BANNERS[] = {
    " _______    _     _               \n"
    "|__   __|  | |   | |              \n"
    "   | | __ _| |__ | |__   ___ _ __ \n"
    "   | |/ _` | '_ \\| '_ \\ / _ \\ '__|\n"
    "   | | (_| | |_) | |_) |  __/ |   \n"
    "   |_|\\__,_|_.__/|_.__/ \\___|_|   ",
    "___________     ___.  ___.                 \n"
    "\\__    ___/____ \\_ |__\\_ |__   ___________ \n"
    "  |    |  \\__  \\ | __ \\| __ \\_/ __ \\_  __ \\\n"
    "  |    |   / __ \\| \\_\\ \\ \\_\\ \\  ___/|  | \\/\n"
    "  |____|  (____  /___  /___  /\\___  >__|   \n"
    "               \\/    \\/    \\/     \\/       ",
    " .---.  .--.  .----. .----. .----..----. \n"
    "{_   _}/ {} \\ | {}  }| {}  }| {_  | {}  }\n"
    "  | | /  /\\  \\| {}  }| {}  }| {__ | .-. \\\n"
    "  `-' `-'  `-'`----' `----' `----'`-' `-'",
    " _____      _     _               \n"
    "/__   \\__ _| |__ | |__   ___ _ __ \n"
    "  / /\\/ _` | '_ \\| '_ \\ / _ \\ '__|\n"
    " / / | (_| | |_) | |_) |  __/ |   \n"
    " \\/   \\__,_|_.__/|_.__/ \\___|_|   ",
    " _______  _______  _______  _______  _______  ______   \n"
    "|       ||   _   ||  _    ||  _    ||       ||    _ |  \n"
    "|_     _||  |_|  || |_|   || |_|   ||    ___||   | ||  \n"
    "  |   |  |       ||       ||       ||   |___ |   |_||_ \n"
    "  |   |  |       ||  _   | |  _   | |    ___||    __  |\n"
    "  |   |  |   _   || |_|   || |_|   ||   |___ |   |  | |\n"
    "  |___|  |__| |__||_______||_______||_______||___|  |_|",
    " _____     _   _           \n"
    "|_   _|___| |_| |_ ___ ___ \n"
    "  | | | .'| . | . | -_|  _|\n"
    "  |_| |__,|___|___|___|_|  ",
    " ____   __    ____  ____  ____  ____ \n"
    "(_  _) /__\\  (  _ \\(  _ \\( ___)(  _ \\\n"
    "  )(  /(__)\\  ) _ < ) _ < )__)  )   /\n"
    " (__)(__)(__)(____/(____/(____)(_)\\_)",
    " _____     _     _               \n"
    "|_   _|   | |   | |              \n"
    "  | | __ _| |__ | |__   ___ _ __ \n"
    "  | |/ _` | '_ \\| '_ \\ / _ \\ '__|\n"
    "  | | (_| | |_) | |_) |  __/ |   \n"
    "  \\_/\\__,_|_.__/|_.__/ \\___|_|   ",
    "  ______      __    __             \n"
    " /_  __/___ _/ /_  / /_  ___  _____\n"
    "  / / / __ `/ __ \\/ __ \\/ _ \\/ ___/\n"
    " / / / /_/ / /_/ / /_/ /  __/ /    \n"
    "/_/  \\__,_/_.___/_.___/\\___/_/     ",
    "_/_/_/_/_/          _/        _/                            \n"
    "   _/      _/_/_/  _/_/_/    _/_/_/      _/_/    _/  _/_/   \n"
    "  _/    _/    _/  _/    _/  _/    _/  _/_/_/_/  _/_/        \n"
    " _/    _/    _/  _/    _/  _/    _/  _/        _/           \n"
    "_/      _/_/_/  _/_/_/    _/_/_/      _/_/_/  _/            ",
    " _____     _    _             \n"
    "|_   _|_ _| |__| |__  ___ _ _ \n"
    "  | |/ _` | '_ \\ '_ \\/ -_) '_|\n"
    "  |_|\\__,_|_.__/_.__/\\___|_|  ",
    " ______     __   __          \n"
    "/_  __/__ _/ /  / /  ___ ____\n"
    " / / / _ `/ _ \\/ _ \\/ -_) __/\n"
    "/_/  \\_,_/_.__/_.__/\\__/_/   ",
    "  ___       ___       ___       ___       ___       ___   \n"
    " /\\  \\     /\\  \\     /\\  \\     /\\  \\     /\\  \\     /\\  \\  \n"
    " \\:\\  \\   /::\\  \\   /::\\  \\   /::\\  \\   /::\\  \\   /::\\  \\ \n"
    " /::\\__\\ /::\\:\\__\\ /::\\:\\__\\ /::\\:\\__\\ /::\\:\\__\\ /::\\:\\__\\\n"
    "/:/\\/__/ \\/\\::/  / \\:\\::/  / \\:\\::/  / \\:\\:\\/  / \\;:::/  /\n"
    "\\/__/      /:/  /   \\::/  /   \\::/  /   \\:\\/  /   |:\\/__/ \n"
    "           \\/__/     \\/__/     \\/__/     \\/__/     \\|__|  ",
    " _____     _     _               \n"
    "|_   _|_ _| |__ | |__   ___ _ __ \n"
    "  | |/ _` | '_ \\| '_ \\ / _ \\ '__|\n"
    "  | | (_| | |_) | |_) |  __/ |   \n"
    "  |_|\\__,_|_.__/|_.__/ \\___|_|   ",
    " ______   ______     ______     ______     ______     ______    \n"
    "/\\__  _\\ /\\  __ \\   /\\  == \\   /\\  == \\   /\\  ___\\   /\\  == \\   \n"
    "\\/_/\\ \\/ \\ \\  __ \\  \\ \\  __<   \\ \\  __<   \\ \\  __\\   \\ \\  __<   \n"
    "   \\ \\_\\  \\ \\_\\ \\_\\  \\ \\_____\\  \\ \\_____\\  \\ \\_____\\  \\ \\_\\ \\_\\ \n"
    "    \\/_/   \\/_/\\/_/   \\/_____/   \\/_____/   \\/_____/   \\/_/ /_/ ",
    " _________  ________    _______    _______   ______   ______       \n"
    "/________/\\/_______/\\ /_______/\\ /_______/\\ /_____/\\ /_____/\\      \n"
    "\\__.::.__\\/\\::: _  \\ \\\\::: _  \\ \\\\::: _  \\ \\\\::::_\\/_\\:::_ \\ \\     \n"
    "   \\::\\ \\   \\::(_)  \\ \\\\::(_)  \\/_\\::(_)  \\/_\\:\\/___/\\\\:(_) ) )_   \n"
    "    \\::\\ \\   \\:: __  \\ \\\\::  _  \\ \\\\::  _  \\ \\\\::___\\/_\\: __ `\\ \\  \n"
    "     \\::\\ \\   \\:.\\ \\  \\ \\\\::(_)  \\ \\\\::(_)  \\ \\\\:\\____/\\\\ \\ `\\ \\ \\ \n"
    "      \\__\\/    \\__\\/\\__\\/ \\_______\\/ \\_______\\/ \\_____\\/ \\_\\/ \\_\\/ ",
    " _________  ________  ________  ________  _______   ________     \n"
    "|\\___   ___\\\\   __  \\|\\   __  \\|\\   __  \\|\\  ___ \\ |\\   __  \\    \n"
    "\\|___ \\  \\_\\ \\  \\|\\  \\ \\  \\|\\ /\\ \\  \\|\\ /\\ \\   __/|\\ \\  \\|\\  \\   \n"
    "     \\ \\  \\ \\ \\   __  \\ \\   __  \\ \\   __  \\ \\  \\_|/_\\ \\   _  _\\  \n"
    "      \\ \\  \\ \\ \\  \\ \\  \\ \\  \\|\\  \\ \\  \\|\\  \\ \\  \\_|\\ \\ \\  \\\\  \\| \n"
    "       \\ \\__\\ \\ \\__\\ \\__\\ \\_______\\ \\_______\\ \\_______\\ \\__\\\\ _\\ \n"
    "        \\|__|  \\|__|\\|__|\\|_______|\\|_______|\\|_______|\\|__|\\|__|",
    " ______          __       __                      \n"
    "/\\__  _\\        /\\ \\     /\\ \\                     \n"
    "\\/_/\\ \\/    __  \\ \\ \\____\\ \\ \\____     __   _ __  \n"
    "   \\ \\ \\  /'__`\\ \\ \\ '__`\\\\ \\ '__`\\  /'__`\\/\\`'__\\\n"
    "    \\ \\ \\/\\ \\L\\.\\_\\ \\ \\L\\ \\\\ \\ \\L\\ \\/\\  __/\\ \\ \\/ \n"
    "     \\ \\_\\ \\__/.\\_\\\\ \\_,__/ \\ \\_,__/\\ \\____\\\\ \\_\\ \n"
    "      \\/_/\\/__/\\/_/ \\/___/   \\/___/  \\/____/ \\/_/ "
};
static int GetBannerID(){
    srand((unsigned int)(time(0)));
    return rand() % IM_COUNTOF(BANNERS);
}

/* Background behind the panel, straight from the Dear ImGui example. */
static const ImVec4 CLEAR_COLOR(0.45f, 0.55f, 0.60f, 1.00f);
static ImVec4 THEME_COLOR;

/*
 * Dear ImGui's own state (column widths, the sort order) goes in the tool's
 * folder, beside config.json and the tab store, rather than in whichever
 * directory the program happened to be started from.
 */
static const char *IMGUI_INI_NAME = "imgui.ini";

/* The panel fills the viewport, so none of what a floating window offers
 * applies: it cannot be moved, resized, collapsed or brought forward. */
static const ImGuiWindowFlags PANEL_FLAGS =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

static const ImGuiTableFlags TABLE_FLAGS =
    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
    ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
    ImGuiTableFlags_Sortable;

static const ImGuiWindowFlags OVERLAY_FLAGS =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoSavedSettings;

/* The two columns the CLI has no use for; the other four are in digest.h. */
#define COL_LAST_USED   "LAST USED"
#define COL_INSTALL     "INSTALL"

/* The longest value each fixed-width column can hold, for sizing it. */
#define WIDEST_DATE      "0000-00-00"
#define WIDEST_LAST_USED "00 months ago"

/* How many rows the table shows before it starts scrolling. */
static const size_t VISIBLE_ROWS = 10;

/* Column order, which is also what a sort spec reports. */
enum {
    COLUMN_CODE, COLUMN_NAME, COLUMN_AUTHORS, COLUMN_DATE,
    COLUMN_LAST_USED, COLUMN_INSTALL, COLUMN_COUNT
};

/* Everything the user reads, in one place. */
static const char *LABEL_NEVER    = "Never";
static const char *LABEL_DOWNLOAD = "Download";
static const char *LABEL_INSTALL  = "Install";
static const char *LABEL_UNINSTALL = "Uninstall";
static const char *LABEL_YES      = "Yes";
static const char *LABEL_NO       = "No";
static const char *LABEL_OK       = "OK";
static const char *LABEL_ABOUT    = ICON_FK_INFO_CIRCLE;
static const char *LABEL_LOOK     = ICON_FK_REFRESH;
static const char *LABEL_PACKS    = ICON_FK_LIST_ALT;
static const char *LABEL_GET      = ICON_FK_DOWNLOAD;
static const char *LABEL_SETTINGS = ICON_FK_WRENCH;
static const char *LABEL_LOG      = ICON_FK_ALIGN_JUSTIFY;
static const char *LABEL_LIGHT    = ICON_FK_SUN;   /* what it would switch to */
static const char *LABEL_DARK     = ICON_FK_MOON;
static const char *TITLE_DONE     = "Done";
static const char *TITLE_FAILED   = "Failed";
static const char *TITLE_CONFIRM  = "One tab at a time";
static const char *TITLE_ABOUT    = "About";
static const char *TITLE_SETTINGS = "Settings";
static const char *TITLE_LOG      = "Session log";
static const char *TITLE_UPDATE   = "Update available";
static const char *TITLE_UPDATED  = "Updated";
static const char *TITLE_CURRENT  = "Up to date";

/* What the overlay says while the thread is away doing each of them. */
static const char *BUSY_CHECK     = "Looking for a newer " TABBER_NAME "...";
static const char *BUSY_UPGRADE   = "Updating " TABBER_NAME "...";
static const char *BUSY_TABS      = "Updating the mappack list...";

/* The corner: what each row says, and what its button offers. The version, the
 * count and the moment are filled in beside these. */
static const char *STATUS_CURRENT  = "Tabber is updated";
static const char *STATUS_WAITING  = "Tabber v%s is available!";
static const char *STATUS_TABS     = "%u custom tab%s";
static const char *HINT_DATE_CHECK = "Last checked: %s";
static const char *HINT_LOOK       = "Look for updates";
static const char *HINT_GET        = "Download update";
static const char *HINT_TABS       = "Look for new custom tabs";
static const char *HINT_SETTINGS   = "Settings";
static const char *HINT_LIGHT      = "Switch to the light theme";
static const char *HINT_DARK       = "Switch to the dark theme";

/* The bar along the bottom: what the game has in it, on the right. The left
 * of it is whatever the tool last had to say, which is not ours to word. */
static const char *STATUS_NO_TAB  = "No custom tabs installed";
static const char *STATUS_ONE_TAB = "%s tab installed";
static const char *HINT_LOG       = "Open session log";

/* What the log viewer says when there is nothing in it to show yet. */
static const char *LOG_EMPTY      = "Nothing has happened yet.";

/* How many lines it shows before it starts scrolling. */
static const int LOG_VIEW_ROWS = 12;

/* What the settings box holds: one line, so far. */
static const char *SETTING_THEME    = "Theme";
static const char *THEME_DARK_NAME  = "Dark";
static const char *THEME_LIGHT_NAME = "Light";

/* The update prompt. The version numbers are filled in beside these. */
static const char *UPDATE_QUESTION = "Update now?";
static const char *UPDATE_NO_BUILD =
    "That release ships no build this one can replace itself with. It can be "
    "installed by hand from the release page:";

/* What the About box says. The name, the version and the date it carries are
 * the release's own, from version.h, so that what is on screen is what was
 * built and not a second copy of it kept up to date by hand. */
static const char *ABOUT_BLURB   = "Installs custom tabs (mappacks) for N++.";
static const char *ABOUT_REPO    = "https://github.com/edelkas/tabber";
static const char *ABOUT_DISCORD = "https://www.discord.gg/nplusplus";
static const char *ABOUT_HINT    = "About " TABBER_NAME;

/* Identifiers Dear ImGui keys its state on. They are not shown to anyone. */
static const char *PANEL_ID   = "tabber-panel";
static const char *TABLE_ID   = "tabber-tabs";
static const char *BUSY_ID    = "tabber-busy";
static const char *DRAG_ID    = "##titlebar";
static const char *BUTTON_ID  = "##corner";
static const char *LOG_ID     = "##log";     /* the scrolling region  */   /* the icon goes on by hand */
static const char *MINIMISE_ID = "##minimise";
static const char *MAXIMISE_ID = "##maximise";
static const char *CLOSE_ID    = "##close";

/* Green for the step that puts a tab in, red for the one that takes it out. */
static const ImVec4 GREEN_BUTTON(0.16f, 0.44f, 0.20f, 1.00f);
static const ImVec4 GREEN_HOVER (0.22f, 0.58f, 0.27f, 1.00f);
static const ImVec4 GREEN_ACTIVE(0.12f, 0.35f, 0.16f, 1.00f);
static const ImVec4 RED_BUTTON  (0.52f, 0.16f, 0.16f, 1.00f);
static const ImVec4 RED_HOVER   (0.66f, 0.22f, 0.22f, 1.00f);
static const ImVec4 RED_ACTIVE  (0.42f, 0.12f, 0.12f, 1.00f);

/* A green for reading rather than for pressing: the button greens above are
 * too dark to put a line of text in on a dark background â€” and too pale for
 * one on a light background, so there is one of each, picked by the theme. */
static const ImVec4 GREEN_TEXT  (0.45f, 0.80f, 0.45f, 1.00f);
static const ImVec4 GREEN_ON_LIGHT(0.06f, 0.45f, 0.12f, 1.00f);

/* What goes on top of those button colours, which are dark in either theme. */
static const ImVec4 WHITE_TEXT  (1.00f, 1.00f, 1.00f, 1.00f);

/* The close button reddens under the pointer, as it does in every window. */
static const ImVec4 CLOSE_HOVER (0.75f, 0.16f, 0.16f, 1.00f);
static const ImVec4 CLOSE_ACTIVE(0.55f, 0.10f, 0.10f, 1.00f);
static const ImVec4 NO_COLOUR   (0.00f, 0.00f, 0.00f, 0.00f);

/* ---- State ------------------------------------------------------------- */

/* The window itself, which the drawn frame has to be able to order about. */
static GLFWwindow *g_window = NULL;
static float g_scale = 1.0f;    /* the monitor's, applied to the style too */

/* A drag of the title bar, and a drag of one of the edges. */
static int    g_dragging = 0;
static ImVec2 g_drag_grab;      /* where in the window it was taken hold of */
static int    g_resize_edges = 0;
static ImVec2 g_resize_grab;    /* the pointer on the desktop when it began */
static int    g_resize_box[4];  /* and the window's x, y, width and height  */

enum {
    EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8
};

/* What is on screen: the catalogue, or why there is none. */
static int     banner_id = GetBannerID();
static digest *g_digest = NULL;
static char    g_error[TB_ERR_LEN] = "";
static char    g_last_updated[TB_WHEN_LEN] = "";

/* One row of the table, worked out every REFRESH_SECONDS. */
typedef struct {
    int downloaded;
    int installed;
    long long last_played;          /* what the column sorts on */
    char last_used[TB_WHEN_LEN];    /* ...and what it shows      */
} tab_row;

static tab_row *g_rows = NULL;
static int     *g_order = NULL;     /* row indices, in the order shown */
static size_t   g_row_count = 0;
static char     g_installed_code[64] = "";
static long long g_rows_stamp = 0;  /* when the rows were last worked out */

static int g_sort_column = -1;      /* -1: the digest's own order */
static int g_sort_ascending = 1;

/* Work a click has asked for, run once the overlay announcing it is on screen.
 * ACT_CHECK is the one nothing clicks: see "Updating tabber itself". */
typedef enum {
    ACT_NONE, ACT_UPDATE, ACT_DOWNLOAD, ACT_INSTALL, ACT_UNINSTALL, ACT_REPLACE,
    ACT_CHECK, ACT_CHECK_ASKED, ACT_UPGRADE
} ui_action;

static ui_action g_pending = ACT_NONE;
static char g_pending_code[TAB_CODE_MAX_LEN + 1] = "";
static int  g_pending_drawn = 0;
static char g_busy_text[128] = "";

/* The dialogs: what a result says, and what a replacement asks. */
static char g_result_title[64] = "";
static char g_result_text[TB_ERR_LEN + 256] = "";
static char g_confirm_text[512] = "";
static const char *g_open_popup = NULL;
static int  g_log_at_end = 0;       /* the log viewer opens at its foot */
static int  g_dialog_open = 0;      /* one is on screen and owns the window */

/* The release a check found and has yet to be answered about. Held rather than
 * copied out because applying it wants the URL, the size and the MD5 too. */
static update_info g_update;
static long long g_update_stamp = 0;   /* when the state file was last asked */

/*
 * What the corner says, mirrored out of the state file. A newer version is
 * known across runs â€” the check that found it wrote it down â€” so this is read
 * from there rather than from g_update, which is only filled by a check this
 * session made and is empty in a window opened after one.
 */
static char g_known_version[UPDATE_VERSION_MAX] = "";  /* empty: none newer */
static char g_checked_when[TB_WHEN_LEN] = "";

/* Which of the two themes is on. Dark is what the program has always looked
 * like, and is what an untouched state file leaves this at. */
static int g_light = 0;

static void on_glfw_error(int error, const char *description)
{
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

/* ---- Reading the world ------------------------------------------------- */

/* When the cached catalogue was last written, which is when we last updated. */
static void refresh_last_updated(void)
{
    char *path = digest_cache_path();
    long long when = 0;

    if (!path || plat_file_mtime(path, &when) != 0)
        when = 0;
    time_local_stamp(when, g_last_updated, sizeof g_last_updated);
    free(path);
}

static void apply_sort(void);

/*
 * Works out what every row should say: whether the tab is downloaded, whether
 * it is the one installed, and when it was last played. All of it can change
 * behind our back, so this is the only place any of it is believed from.
 */
static void refresh_rows(void)
{
    char err[TB_ERR_LEN];
    config *cfg;
    npp_paths paths;
    size_t i;
    long long now = (long long)time(NULL);

    memset(&paths, 0, sizeof paths);
    g_rows_stamp = now;

    free(g_rows);
    free(g_order);
    g_rows = NULL;
    g_order = NULL;
    g_installed_code[0] = '\0';
    g_row_count = g_digest ? g_digest->tab_count : 0;

    refresh_last_updated();
    if (g_row_count == 0)
        return;

    g_rows = (tab_row *)xmalloc(g_row_count * sizeof *g_rows);
    memset(g_rows, 0, g_row_count * sizeof *g_rows);
    g_order = (int *)xmalloc(g_row_count * sizeof *g_order);

    /* The game's folders are wanted for the savefile timestamps only. Without
     * them the dates fall back to what the state file remembers, so a machine
     * where the game cannot be found still shows a usable table. */
    npp_find_game_dirs(&paths, err, sizeof err);
    npp_find_personal_dir(&paths, err, sizeof err);

    cfg = config_load(err, sizeof err);
    if (cfg)
        install_detect(cfg, &paths, g_installed_code, sizeof g_installed_code);

    for (i = 0; i < g_row_count; i++) {
        const npp_tab *tab = &g_digest->tabs[i];
        tab_usage used;

        g_rows[i].downloaded = tab_is_downloaded(tab->code);
        g_rows[i].installed = g_installed_code[0] != '\0' &&
                              str_ieq(g_installed_code, tab->code);
        usage_last_played(cfg, &paths, tab->code, g_rows[i].installed, &used);
        g_rows[i].last_played = used.when;
        time_relative(used.when, now, LABEL_NEVER,
                      g_rows[i].last_used, sizeof g_rows[i].last_used);
    }

    config_free(cfg);
    npp_paths_free(&paths);
    apply_sort();
}

/*
 * Re-parses the cached catalogue. A parse that fails leaves the one already on
 * screen alone: a broken download must not empty the table.
 */
static int reload_digest(char *err, size_t errsz)
{
    digest *fresh = digest_load(err, errsz);

    if (!fresh)
        return -1;
    digest_free(g_digest);
    g_digest = fresh;
    g_error[0] = '\0';
    return 0;
}

/* Loads the catalogue at startup, refreshing it from the network first. */
static void load_digest(void)
{
    char err[TB_ERR_LEN];

    if (digest_ensure_fresh(0, err, sizeof err) != 0)
        fprintf(stderr, TABBER_NAME ": could not refresh the digest (%s), using the cached copy\n", err);
    if (reload_digest(err, sizeof err) != 0)
        snprintf(g_error, sizeof g_error, "%s", err);
}

/* ---- Sorting ----------------------------------------------------------- */

/* Case-insensitive, so a lowercase name does not sort below every capital. */
static int text_cmp(const char *a, const char *b)
{
    unsigned char ca, cb;

    if (!a) a = "";
    if (!b) b = "";
    for (; *a || *b; a++, b++) {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    return 0;
}

static int compare_rows(const void *pa, const void *pb)
{
    int ia = *(const int *)pa, ib = *(const int *)pb;
    const npp_tab *a = &g_digest->tabs[ia];
    const npp_tab *b = &g_digest->tabs[ib];
    long long da, db;
    int r = 0;

    switch (g_sort_column) {
    case COLUMN_CODE:    r = text_cmp(a->code, b->code); break;
    case COLUMN_NAME:    r = text_cmp(a->name, b->name); break;
    case COLUMN_AUTHORS: r = text_cmp(a->authors, b->authors); break;
    /* The dates are fixed-width ISO 8601, so comparing them as text orders
     * them; "never played" is 0, which sorts below every real date. */
    case COLUMN_DATE:    r = text_cmp(a->date, b->date); break;
    case COLUMN_LAST_USED:
        da = g_rows[ia].last_played;
        db = g_rows[ib].last_played;
        r = (da > db) - (da < db);
        break;
    default: break;
    }
    if (r != 0)
        return g_sort_ascending ? r : -r;
    return ia - ib;   /* ties keep the catalogue's own order, both ways */
}

static void apply_sort(void)
{
    size_t i;

    for (i = 0; i < g_row_count; i++)
        g_order[i] = (int)i;
    if (g_sort_column >= 0 && g_row_count > 1)
        qsort(g_order, g_row_count, sizeof *g_order, compare_rows);
}

/* ---- Doing the work ---------------------------------------------------- */

static void set_result(const char *title, const char *fmt, ...)
{
    va_list ap;

    snprintf(g_result_title, sizeof g_result_title, "%s", title);
    va_start(ap, fmt);
    vsnprintf(g_result_text, sizeof g_result_text, fmt, ap);
    va_end(ap);
    g_open_popup = g_result_title;

    /* What a dialog says is news whether or not anyone is looking at the
     * dialog, so it goes in the log too. The log folds it onto one line, which
     * is what the several-line ones become in the bar along the bottom. */
    log_line("%s", g_result_text);
}

/* The tab `code` names, or NULL with a reason when the catalogue has no such. */
static const npp_tab *find_tab(const char *code, char *err, size_t errsz)
{
    const npp_tab *tab = g_digest ? digest_find(g_digest, code) : NULL;

    if (!tab)
        err_set(err, errsz, "there is no custom tab with the code '%s'", code);
    return tab;
}

/* Both folders, as install and uninstall need them. */
static int find_game(npp_paths *paths, char *err, size_t errsz)
{
    memset(paths, 0, sizeof *paths);
    if (npp_find_game_dirs(paths, err, errsz) != 0 ||
        npp_find_personal_dir(paths, err, errsz) != 0) {
        npp_paths_free(paths);
        return -1;
    }
    return 0;
}

/* Downloads a tab into the local store, as `fetch` does. */
static int run_download(const npp_tab *tab, char *err, size_t errsz)
{
    tab_report report;

    if (tab_fetch(g_digest, tab, &report, err, errsz) != 0)
        return -1;
    tab_report_free(&report);
    return 0;
}

/* Puts a tab into the game, downloading it first if it is not in the store. */
static int run_install(const npp_tab *tab, char *err, size_t errsz)
{
    npp_paths paths;
    install_options opts;
    install_report report;
    config *cfg;
    char other[64];
    int busy;

    if (find_game(&paths, err, errsz) != 0)
        return -1;

    /* Downloading first changes nothing in the game folder, so it is safe to
     * do before the checks below. */
    if (!tab_is_downloaded(tab->code) && run_download(tab, err, errsz) != 0) {
        npp_paths_free(&paths);
        return -1;
    }

    /* Only one tab at a time. The button that led here was drawn from state
     * that may be minutes old, and the CLI may have installed something since,
     * so the answer is asked for again rather than assumed. */
    cfg = config_load(err, errsz);
    if (!cfg) {
        npp_paths_free(&paths);
        return -1;
    }
    busy = install_detect(cfg, &paths, other, sizeof other) &&
           !str_ieq(other, tab->code);
    config_free(cfg);
    if (busy) {
        char upper[DIGEST_CODE_BUF];

        digest_code_upper(upper, sizeof upper, other);
        err_set(err, errsz, "%s has been installed in the meantime; uninstall "
                            "it first", upper);
        npp_paths_free(&paths);
        return -1;
    }

    install_options_init(&opts);
    if (tab_install(g_digest, tab, &paths, &opts, &report, err, errsz) != 0) {
        install_report_free(&report);
        npp_paths_free(&paths);
        return -1;
    }
    install_report_free(&report);
    npp_paths_free(&paths);
    return 0;
}

/* Puts the game back as it was, as `uninstall` does. */
static int run_uninstall(const npp_tab *tab, char *err, size_t errsz)
{
    npp_paths paths;
    install_options opts;
    uninstall_report report;

    if (find_game(&paths, err, errsz) != 0)
        return -1;

    install_options_init(&opts);
    if (tab_uninstall(g_digest, tab, &paths, &opts, &report, err, errsz) != 0) {
        uninstall_report_free(&report);
        npp_paths_free(&paths);
        return -1;
    }
    uninstall_report_free(&report);
    npp_paths_free(&paths);
    return 0;
}

static void run_check(int asked);
static void run_upgrade(void);

/* Carries out whatever the last click asked for, and says how it went. */
static void run_pending(void)
{
    char err[TB_ERR_LEN];
    char upper[DIGEST_CODE_BUF], other[DIGEST_CODE_BUF];
    const npp_tab *tab, *installed;
    ui_action action = g_pending;

    if (action == ACT_NONE || !g_pending_drawn)
        return;
    g_pending = ACT_NONE;
    g_pending_drawn = 0;

    /* Neither of these is about a tab, so neither wants the code below. */
    if (action == ACT_CHECK || action == ACT_CHECK_ASKED) {
        run_check(action == ACT_CHECK_ASKED);
        return;
    }
    if (action == ACT_UPGRADE) {
        run_upgrade();
        return;
    }

    if (action == ACT_UPDATE) {
        /* Forced, as `update` is: an explicit refresh always goes out. */
        if (digest_ensure_fresh(1, err, sizeof err) != 0)
            set_result(TITLE_FAILED, "The mappack list could not be updated:\n%s", err);
        else if (reload_digest(err, sizeof err) != 0)
            set_result(TITLE_FAILED, "The mappack list was downloaded but could "
                                     "not be read:\n%s", err);
        else
            set_result(TITLE_DONE, "The mappack list is up to date: %u custom tab(s).",
                       (unsigned)g_digest->tab_count);
        refresh_rows();
        return;
    }

    tab = find_tab(g_pending_code, err, sizeof err);
    if (!tab) {
        set_result(TITLE_FAILED, "%s", err);
        return;
    }
    digest_code_upper(upper, sizeof upper, tab->code);

    switch (action) {
    case ACT_DOWNLOAD:
        if (run_download(tab, err, sizeof err) != 0)
            set_result(TITLE_FAILED, "%s could not be downloaded:\n%s", upper, err);
        else
            set_result(TITLE_DONE, "%s has been downloaded.", upper);
        break;

    case ACT_UNINSTALL:
        if (run_uninstall(tab, err, sizeof err) != 0)
            set_result(TITLE_FAILED, "%s could not be uninstalled:\n%s", upper, err);
        else
            set_result(TITLE_DONE, "%s has been uninstalled, and the game is "
                                   "back as it was.", upper);
        break;

    case ACT_REPLACE:
        /* Take the other one out first; if that fails nothing else is tried,
         * since installing over it is exactly what must not happen. */
        installed = find_tab(g_installed_code, err, sizeof err);
        digest_code_upper(other, sizeof other, g_installed_code);
        if (!installed) {
            set_result(TITLE_FAILED, "%s could not be uninstalled:\n%s", other, err);
            break;
        }
        if (run_uninstall(installed, err, sizeof err) != 0) {
            set_result(TITLE_FAILED, "%s could not be uninstalled, so %s was not "
                                     "installed:\n%s", other, upper, err);
            break;
        }
        if (run_install(tab, err, sizeof err) != 0)
            set_result(TITLE_FAILED, "%s was uninstalled, but %s could not be "
                                     "installed:\n%s", other, upper, err);
        else
            set_result(TITLE_DONE, "%s has been uninstalled and %s installed "
                                   "in its place.", other, upper);
        break;

    case ACT_INSTALL:
        if (run_install(tab, err, sizeof err) != 0)
            set_result(TITLE_FAILED, "%s could not be installed:\n%s", upper, err);
        else
            set_result(TITLE_DONE, "%s has been installed.", upper);
        break;

    default:
        break;
    }
    refresh_rows();
}

/* Asks for work, and for the overlay that says so. */
static void request(ui_action action, const char *code, const char *busy)
{
    g_pending = action;
    g_pending_drawn = 0;
    snprintf(g_pending_code, sizeof g_pending_code, "%s", code ? code : "");
    snprintf(g_busy_text, sizeof g_busy_text, "%s", busy);

    /* The same line the CLI prints when it starts one of these. It stands in
     * the bar until whatever it announced has something to report. */
    log_line("%s", g_busy_text);
}

/*
 * An install click. When another tab is in place the choice is the user's, so
 * it is put to them; otherwise it just goes ahead.
 */
static void request_install(const npp_tab *tab)
{
    char upper[DIGEST_CODE_BUF], other[DIGEST_CODE_BUF];

    if (g_installed_code[0] == '\0' || str_ieq(g_installed_code, tab->code)) {
        request(ACT_INSTALL, tab->code, "Installing...");
        return;
    }

    digest_code_upper(upper, sizeof upper, tab->code);
    digest_code_upper(other, sizeof other, g_installed_code);
    snprintf(g_confirm_text, sizeof g_confirm_text,
             "%s is installed, and only one custom tab can be installed at a "
             "time.\n\nUninstall %s and install %s in its place?",
             other, other, upper);
    snprintf(g_pending_code, sizeof g_pending_code, "%s", tab->code);
    g_open_popup = TITLE_CONFIRM;
    log_line("%s", g_confirm_text);   /* two paragraphs; the log folds them */
}

/* ---- Updating tabber itself ---------------------------------------------
 *
 * The same three steps the CLI takes, and the same code underneath: look at
 * the manifest the newest release carries, and if it names a version above
 * this one, offer it. Taking it up downloads the build for this platform and
 * this front-end, checks it against the size and the MD5 the manifest
 * declares, moves this binary aside, puts the new one in its place and makes
 * it prove it runs before the old one is let go. See src/update.h.
 *
 * What is different here is what happens at the ends of it. Nothing that goes
 * wrong closes this window: the failure goes in a dialog and the program the
 * user already had carries on running, which is the one thing an update must
 * never take away from them. And nothing that goes right can be reported by
 * the process that did it, because on success it hands over to the binary it
 * just installed and exits â€” so the news is written to the state file and the
 * new process gives it, once.
 *
 * The looking is on a timer rather than a button: at startup and every
 * UPDATE_POLL_SECONDS after, asking GitHub at most once a day. Only a version
 * the user has not already said no to interrupts them.
 */

/*
 * Reads what the state file knows about updates into the two strings the
 * corner draws from. A version at or below this one leaves g_known_version
 * empty, which is what "up to date" means here: the newest release there is,
 * as far as the last look could tell.
 */
static void read_update_status(config *cfg)
{
    const char *latest = config_update_latest(cfg);
    const char *when = config_update_last_check(cfg);

    g_known_version[0] = '\0';
    if (latest && update_version_compare(latest, TABBER_VERSION) > 0)
        snprintf(g_known_version, sizeof g_known_version, "%s", latest);
    time_local_stamp(when ? time_from_iso8601(when) : 0,
                     g_checked_when, sizeof g_checked_when);
}

/* The same, for the places that have no config open already. */
static void refresh_update_status(void)
{
    char err[TB_ERR_LEN];
    config *cfg = config_load(err, sizeof err);

    if (!cfg)
        return;
    read_update_status(cfg);
    config_free(cfg);
}

/* Remembers that this version was turned down, so it is offered once and not
 * every day until it is taken. */
static void decline_update(void)
{
    char err[TB_ERR_LEN];
    config *cfg = config_load(err, sizeof err);

    if (!cfg)
        return;
    config_update_decline(cfg, g_update.version);
    config_save(cfg, err, sizeof err);
    config_free(cfg);
}

/*
 * Reads the newest release's manifest and records that the look happened.
 * Returns 0 whatever the manifest turned out to say, or -1 with the reason in
 * `err`; `declined`, when given, comes back saying whether that exact version
 * has been turned down before.
 *
 * Anything newer than what is running is left in g_update, which is what the
 * corner reads and what applying an update works from â€” the version alone is
 * no use for that, since installing it wants the URL and the two promises the
 * manifest makes about the download.
 */
static int look_for_update(int *declined, char *err, size_t errsz)
{
    char sub[TB_ERR_LEN];
    update_info info;
    config *cfg;
    int rc;

    if (declined)
        *declined = 0;
    rc = update_check(UPDATE_FLAVOUR_GUI, &info, err, errsz);

    /* The look is recorded whether or not it got through, or a machine that
     * cannot reach GitHub would pay for a failed lookup every half hour
     * instead of once a day. */
    cfg = config_load(sub, sizeof sub);
    if (cfg) {
        config_update_checked(cfg, rc == 0 ? info.version : NULL);
        if (rc == 0 && declined)
            *declined = config_update_declined(cfg, info.version);
        config_save(cfg, sub, sizeof sub);
        read_update_status(cfg);
        config_free(cfg);
    }
    if (rc != 0)
        return -1;

    update_info_free(&g_update);   /* whatever was known before is older news */
    if (info.newer)
        g_update = info;
    else
        update_info_free(&info);
    return 0;
}

/*
 * A look, and what is said about one that turns up nothing. A look nobody
 * asked for is silent either way â€” no network is not this window's problem to
 * report, and nothing to report is not worth a dialog. One that was asked for
 * answers both ways, because a button that can be pressed to no visible effect
 * is a button that looks broken.
 */
static void run_check(int asked)
{
    char err[TB_ERR_LEN];
    int declined = 0;

    if (look_for_update(&declined, err, sizeof err) != 0) {
        if (asked)
            set_result(TITLE_FAILED, "The newest release could not be looked "
                                     "up:\n%s", err);
        return;
    }

    if (g_known_version[0] == '\0') {
        if (asked)
            set_result(TITLE_CURRENT, "%s %s is the newest release.",
                       TABBER_NAME, TABBER_VERSION);
        return;
    }

    /* A version turned down before is not put up again on its own; a look the
     * user asked for is an answer owed, so it is put up then. */
    if (asked || !declined) {
        g_open_popup = TITLE_UPDATE;

        /* The release notes are the bulk of that box and are no use on one
         * line, so what is logged is the line the corner shows. */
        log_line(STATUS_WAITING, g_known_version);
    }
}

/*
 * Takes the release the user has just said yes to. Everything up to the last
 * step is the library's; what is left here is where each outcome goes.
 */
static void run_upgrade(void)
{
    char err[TB_ERR_LEN], sub[TB_ERR_LEN];
    update_plan plan;
    config *cfg;

    /*
     * A window opened since the look that found the release knows only what
     * the state file kept â€” which version, not where to get it â€” so the
     * manifest is read again. Which is the honest thing to do in any case:
     * what gets installed is whatever is newest at the moment the button is
     * pressed, not what was newest when the corner last drew.
     */
    if (!g_update.url && look_for_update(NULL, err, sizeof err) != 0) {
        set_result(TITLE_FAILED, "The newest release could not be looked up:\n%s\n\n"
                                 "Nothing has changed; you are still running %s.",
                   err, TABBER_VERSION);
        return;
    }
    if (g_known_version[0] == '\0') {
        set_result(TITLE_CURRENT, "%s %s is the newest release.",
                   TABBER_NAME, TABBER_VERSION);
        return;
    }
    if (!g_update.url) {
        set_result(TITLE_FAILED, "%s %s ships no build this one can replace "
                                 "itself with.\n\nIt can be installed by hand "
                                 "from %s", TABBER_NAME, g_known_version,
                   g_update.page ? g_update.page : UPDATE_RELEASES_URL);
        return;
    }

    /* Downloaded and weighed and hashed; still nothing in place. */
    if (update_plan_build(&g_update, &plan, err, sizeof err) != 0) {
        set_result(TITLE_FAILED, "%s %s could not be downloaded:\n%s\n\n"
                                 "Nothing has changed; you are still running %s.",
                   TABBER_NAME, g_update.version, err, TABBER_VERSION);
        return;
    }

    /* This binary aside, the new one under its name, and the new one made to
     * say what it is before the old one is let go. */
    if (update_plan_apply(&plan, err, sizeof err) != 0) {
        set_result(TITLE_FAILED, "The update was not applied:\n%s\n\n"
                                 "You are still running %s.", err, TABBER_VERSION);
        update_plan_free(&plan);
        return;
    }

    /* Left for the binary that replaces this one to tell the user about. */
    cfg = config_load(sub, sizeof sub);
    if (cfg) {
        config_update_applied(cfg, plan.version);
        config_save(cfg, sub, sizeof sub);
        config_free(cfg);
    }

    /*
     * Started and not waited for: this process is about to go, and on Windows
     * waiting would keep it holding the binary it was replaced from. If it
     * will not start, the update itself still stands â€” the window stays open
     * on the old code, and the news above is given whenever it is next opened.
     */
    if (plat_spawn_detached(plan.exe, NULL, 0) != 0) {
        set_result(TITLE_FAILED, "%s %s is installed, but it could not be "
                                 "started.\n\nClose this window and open "
                                 "%s again.", TABBER_NAME, plan.version, TABBER_NAME);
        update_plan_free(&plan);
        return;
    }
    update_plan_free(&plan);
    glfwSetWindowShouldClose(g_window, GLFW_TRUE);
}

/* Asks for a look, if one is due and there is nothing in the way of it. */
static void poll_for_update(void)
{
    char err[TB_ERR_LEN];
    config *cfg;

    g_update_stamp = (long long)time(NULL);

    /* Never over the top of something else: a dialog waiting on an answer, or
     * work already under way on this thread. */
    if (g_pending != ACT_NONE || g_open_popup || g_dialog_open)
        return;

    cfg = config_load(err, sizeof err);
    if (!cfg)
        return;
    if (config_update_enabled(cfg) && config_update_due(cfg, UPDATE_CHECK_HOURS))
        request(ACT_CHECK, NULL, BUSY_CHECK);
    config_free(cfg);
}

/*
 * The other half of an update that went through, in the process that came out
 * of it: says so, once, and strikes the record so that it is not said again.
 */
static void announce_update(void)
{
    char err[TB_ERR_LEN];
    config *cfg = config_load(err, sizeof err);
    const char *applied;

    if (!cfg)
        return;
    applied = config_update_unannounced(cfg);
    if (applied) {
        /* Only when it is this version that arrived. Anything else is a record
         * left over from a binary that is no longer the one running, and there
         * is nothing to congratulate anyone on. */
        if (strcmp(applied, TABBER_VERSION) == 0)
            set_result(TITLE_UPDATED, "%s has been updated to %s.",
                       TABBER_NAME, TABBER_VERSION);
        config_update_announced(cfg);
        config_save(cfg, err, sizeof err);
    }
    config_free(cfg);
}

/* ---- The theme ----------------------------------------------------------
 *
 * Two of them, both Dear ImGui's own: the dark one the program has always
 * worn, and the light one. Which is in use is kept in the state file, so a
 * window opens the way the last one was left rather than back on the default.
 */

/* Paints the style, and takes the accent from it: the banner and the version
 * lines are drawn in the button's colour, which is not the same in the two. */
static void apply_theme(void)
{
    if (g_light)
        ImGui::StyleColorsLight();
    else
        ImGui::StyleColorsDark();

    /* Only the colours: the sizes were scaled to the monitor once, at startup,
     * and StyleColors* leaves them alone. */
    THEME_COLOR = ImGui::GetStyleColorVec4(ImGuiCol_Button);

    /* That colour is a tint â€” two fifths opaque â€” which carries as text on the
     * dark ground and dissolves into the light one, so there it is taken at
     * full strength. Same hue either way, which is what makes it the accent. */
    if (g_light)
        THEME_COLOR.w = 1.0f;
}

/* Which one the last run was left on. Anything the file does not say is dark. */
static void read_theme(void)
{
    char err[TB_ERR_LEN];
    config *cfg = config_load(err, sizeof err);
    const char *theme;

    if (!cfg)
        return;
    theme = config_gui_theme(cfg);
    g_light = theme && strcmp(theme, CONFIG_THEME_LIGHT) == 0;
    config_free(cfg);
}

/*
 * Switches to the one asked for and writes it down. The switch happens whether
 * or not the writing does: the window has to follow the click either way, and
 * a state file that could not be written simply opens on the old theme next
 * time â€” which is a lesser thing to go wrong with than a button that looks
 * broken. Asking for the one already on does nothing at all, which is what a
 * radio button pressed where it stands is asking for.
 */
static void choose_theme(int light)
{
    char err[TB_ERR_LEN];
    config *cfg;

    if (light == g_light)
        return;
    g_light = light;
    apply_theme();
    cfg = config_load(err, sizeof err);
    if (!cfg)
        return;
    config_set_gui_theme(cfg, g_light ? CONFIG_THEME_LIGHT : CONFIG_THEME_DARK);
    config_save(cfg, err, sizeof err);
    config_free(cfg);
}

/* ---- Drawing ----------------------------------------------------------- */

static void push_button_colors(const ImVec4 &normal, const ImVec4 &hovered,
                               const ImVec4 &active)
{
    ImGui::PushStyleColor(ImGuiCol_Button, normal);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);

    /* The label too: these three are dark whichever theme is on, and the light
     * one would otherwise write on them in near-black. */
    ImGui::PushStyleColor(ImGuiCol_Text, WHITE_TEXT);
}

/*
 * One width for all three buttons: whichever label is longest, plus the usual
 * padding at its sides. A button wide enough for the longest word is wide
 * enough for the other two, and the column then stays still as tabs come and
 * go. The height is left alone, so they are as tall as any other button.
 */
static float button_width(void)
{
    const char *labels[] = { LABEL_DOWNLOAD, LABEL_INSTALL, LABEL_UNINSTALL };
    float widest = 0.0f;
    size_t i;

    for (i = 0; i < sizeof labels / sizeof *labels; i++) {
        float w = ImGui::CalcTextSize(labels[i]).x;
        if (w > widest)
            widest = w;
    }
    return widest + ImGui::GetStyle().FramePadding.x * 2.0f;
}

/* The button that says what can be done to this tab, and does it. */
static void draw_row_button(const npp_tab *tab, const tab_row *row)
{
    ImVec2 size(button_width(), 0.0f);   /* 0 keeps the default height */

    /* While something is running, nothing else may be asked for. */
    ImGui::BeginDisabled(g_pending != ACT_NONE);
    if (row->installed) {
        push_button_colors(RED_BUTTON, RED_HOVER, RED_ACTIVE);
        if (ImGui::Button(LABEL_UNINSTALL, size))
            request(ACT_UNINSTALL, tab->code, "Uninstalling...");
        ImGui::PopStyleColor(4);
    } else if (row->downloaded) {
        push_button_colors(GREEN_BUTTON, GREEN_HOVER, GREEN_ACTIVE);
        if (ImGui::Button(LABEL_INSTALL, size))
            request_install(tab);
        ImGui::PopStyleColor(4);
    } else {
        if (ImGui::Button(LABEL_DOWNLOAD, size))
            request(ACT_DOWNLOAD, tab->code, "Downloading...");
    }
    ImGui::EndDisabled();
}

static float status_bar_height(void);

/*
 * How tall the table has to be for VISIBLE_ROWS rows under the header, or for
 * every row when there are fewer than that. Saying it outright is what stops
 * the table from stretching to the bottom of the window and leaving an empty
 * band below the last row.
 */
static float table_height(size_t rows)
{
    float pad = ImGui::GetStyle().CellPadding.y * 2.0f;
    float shown = (float)(rows < VISIBLE_ROWS ? rows : VISIBLE_ROWS);

    /* The bar along the bottom is not the table's to take: it is drawn after
     * it and outside the flow, so the room it wants comes off here. */
    float avail = ImGui::GetContentRegionAvail().y - status_bar_height();
    float height;

    /* The header is one line of text; a row is as tall as the button in it. */
    height = ImGui::GetFontSize() + pad + shown * (ImGui::GetFrameHeight() + pad);

    /* Never past the bottom of the window, however short it has been dragged. */
    return (avail > 0.0f && height > avail) ? avail : height;
}

static void draw_tab_table(void)
{
    ImGuiTableSortSpecs *specs;
    size_t i;
    float cell, pad, button;

    if (!ImGui::BeginTable(TABLE_ID, COLUMN_COUNT, TABLE_FLAGS,
                           ImVec2(0.0f, table_height(g_row_count))))
        return;

    /*
     * The columns of known size are fixed and start out just wide enough for
     * the widest thing that can land in them; the two carrying free text share
     * whatever is left over. Sortable headers are given a font's width extra
     * for the sort arrow, which would otherwise eat into one as short as CODE.
     */
    cell = ImGui::GetStyle().CellPadding.x * 2.0f;
    pad = cell + ImGui::GetFontSize();
    button = button_width();
    if (button < ImGui::CalcTextSize(COL_INSTALL).x)
        button = ImGui::CalcTextSize(COL_INSTALL).x;
    ImGui::TableSetupColumn(COL_CODE, ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize(COL_CODE).x + pad);
    ImGui::TableSetupColumn(COL_NAME, ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(COL_AUTHORS, ImGuiTableColumnFlags_WidthStretch);
    /* The catalogue opens newest-first, which is the order it is read in. */
    ImGui::TableSetupColumn(COL_DATE, ImGuiTableColumnFlags_WidthFixed |
                                      ImGuiTableColumnFlags_DefaultSort |
                                      ImGuiTableColumnFlags_PreferSortDescending,
                            ImGui::CalcTextSize(WIDEST_DATE).x + pad);
    ImGui::TableSetupColumn(COL_LAST_USED, ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize(WIDEST_LAST_USED).x + pad);
    /* No arrow to make room for here: this column does not sort. */
    ImGui::TableSetupColumn(COL_INSTALL, ImGuiTableColumnFlags_WidthFixed |
                                         ImGuiTableColumnFlags_NoSort,
                            button + cell);
    ImGui::TableSetupScrollFreeze(0, 1);   /* the header stays put */
    ImGui::TableHeadersRow();

    specs = ImGui::TableGetSortSpecs();
    if (specs && specs->SpecsDirty) {
        if (specs->SpecsCount > 0) {
            g_sort_column = specs->Specs[0].ColumnIndex;
            g_sort_ascending =
                specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
        } else {
            g_sort_column = -1;
        }
        apply_sort();
        specs->SpecsDirty = false;
    }

    for (i = 0; i < g_row_count; i++) {
        int index = g_order[i];
        const npp_tab *tab = &g_digest->tabs[index];
        const tab_row *row = &g_rows[index];
        char code[DIGEST_CODE_BUF], date[DIGEST_DATE_BUF];

        digest_code_upper(code, sizeof code, tab->code);
        digest_date_short(date, sizeof date, tab->date);

        ImGui::PushID(index);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(COLUMN_CODE);
        ImGui::TextUnformatted(code);
        ImGui::TableSetColumnIndex(COLUMN_NAME);
        ImGui::TextUnformatted(tab->name ? tab->name : "");
        ImGui::TableSetColumnIndex(COLUMN_AUTHORS);
        ImGui::TextUnformatted(tab->authors ? tab->authors : "");
        ImGui::TableSetColumnIndex(COLUMN_DATE);
        ImGui::TextUnformatted(date);
        ImGui::TableSetColumnIndex(COLUMN_LAST_USED);
        ImGui::TextUnformatted(row->last_used);
        ImGui::TableSetColumnIndex(COLUMN_INSTALL);
        draw_row_button(tab, row);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/* ---- The window's own frame -------------------------------------------- */

/*
 * Where the pointer is, asked of GLFW rather than of Dear ImGui. Both answers
 * are read in the same breath, which matters while the window is being dragged
 * or resized: it is then moving out from under the pointer, and a position
 * left over from the last poll would fight the one being set now.
 */
static ImVec2 cursor_in_window(void)
{
    double x = 0.0, y = 0.0;

    glfwGetCursorPos(g_window, &x, &y);
    return ImVec2((float)x, (float)y);
}

static ImVec2 cursor_on_desktop(void)
{
    ImVec2 c = cursor_in_window();
    int x = 0, y = 0;

    glfwGetWindowPos(g_window, &x, &y);
    return ImVec2(c.x + (float)x, c.y + (float)y);
}

static void toggle_maximised(void)
{
    if (glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED))
        glfwRestoreWindow(g_window);
    else
        glfwMaximizeWindow(g_window);
}

/*
 * The three glyphs, drawn rather than written: the built-in font has no
 * symbols for them, and a line is a line at any size. `c` is the middle of the
 * button and `r` half the width of the mark that goes there.
 */
typedef enum { ICON_MINIMISE, ICON_MAXIMISE, ICON_CLOSE } bar_icon;

static void draw_icon(ImDrawList *dl, bar_icon which, ImVec2 c, float r,
                      ImU32 col, float th, int maximised)
{
    float o = r * 0.4f;   /* how far the two frames of the restore mark part */

    switch (which) {
    case ICON_MINIMISE:
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, th);
        break;
    case ICON_MAXIMISE:
        if (!maximised) {
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                        col, 0.0f, 0, th);
        } else {
            /* The one in front, and two sides of the one behind it. */
            dl->AddRect(ImVec2(c.x - r, c.y - r + o),
                        ImVec2(c.x + r - o, c.y + r), col, 0.0f, 0, th);
            dl->AddLine(ImVec2(c.x - r + o, c.y - r),
                        ImVec2(c.x + r, c.y - r), col, th);
            dl->AddLine(ImVec2(c.x + r, c.y - r),
                        ImVec2(c.x + r, c.y + r - o), col, th);
        }
        break;
    case ICON_CLOSE:
        dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, th);
        dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, th);
        break;
    }
}

/* One of the three, transparent until the pointer is on it. */
static int title_button(const char *id, bar_icon which, ImVec2 size,
                        int maximised)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec4 *style = ImGui::GetStyle().Colors;
    int pressed;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, NO_COLOUR);
    if (which == ICON_CLOSE) {
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CLOSE_HOVER);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, CLOSE_ACTIVE);
    } else {
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style[ImGuiCol_ButtonHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, style[ImGuiCol_ButtonActive]);
    }
    pressed = ImGui::Button(id, size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    draw_icon(dl, which, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f),
              ImGui::GetFontSize() * TITLE_ICON_SIZE,
              ImGui::GetColorU32(ImGuiCol_Text), g_scale, maximised);
    return pressed;
}

/*
 * The bar across the top: the title, the three buttons, and between them the
 * stretch that drags the window. `taken` says an edge has already claimed the
 * pointer this frame, in which case the drag must not also start. Returns the
 * height it used.
 */
static float draw_title_bar(int taken)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImGuiStyle &style = ImGui::GetStyle();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float height = ImGui::GetFrameHeight();
    float button = height * TITLE_BUTTON_ASPECT;
    int maximised = glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED);
    int focused = glfwGetWindowAttrib(g_window, GLFW_FOCUSED);
    ImVec2 corner = vp->WorkPos;
    ImVec2 far_end(corner.x + vp->WorkSize.x, corner.y + height);
    ImVec2 title;
    float drag_width;

    dl->AddRectFilled(corner, far_end, ImGui::GetColorU32(
                          focused ? ImGuiCol_TitleBgActive : ImGuiCol_TitleBg));
    title = ImGui::CalcTextSize(WINDOW_TITLE);
    dl->AddText(ImVec2(corner.x + style.FramePadding.x * 2.0f,
                       corner.y + (height - title.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), WINDOW_TITLE);

    /*
     * The drag area goes down first so that the buttons, submitted after it,
     * win the pointer where the two overlap.
     */
    drag_width = vp->WorkSize.x - button * 3.0f;
    if (drag_width < 1.0f)
        drag_width = 1.0f;
    ImGui::SetCursorScreenPos(corner);
    ImGui::InvisibleButton(DRAG_ID, ImVec2(drag_width, height));
    if (ImGui::IsItemActive() && !maximised && !taken) {
        if (!g_dragging) {
            g_dragging = 1;
            g_drag_grab = cursor_in_window();
        } else {
            /* Keep the point that was grabbed under the pointer. */
            ImVec2 now = cursor_on_desktop();
            glfwSetWindowPos(g_window, (int)(now.x - g_drag_grab.x),
                                       (int)(now.y - g_drag_grab.y));
        }
    } else {
        g_dragging = 0;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        toggle_maximised();

    ImGui::SetCursorScreenPos(ImVec2(far_end.x - button * 3.0f, corner.y));
    if (title_button(MINIMISE_ID, ICON_MINIMISE, ImVec2(button, height), maximised))
        glfwIconifyWindow(g_window);
    ImGui::SameLine(0.0f, 0.0f);
    if (title_button(MAXIMISE_ID, ICON_MAXIMISE, ImVec2(button, height), maximised))
        toggle_maximised();
    ImGui::SameLine(0.0f, 0.0f);
    if (title_button(CLOSE_ID, ICON_CLOSE, ImVec2(button, height), maximised))
        glfwSetWindowShouldClose(g_window, GLFW_TRUE);

    /* Back to where the panel's own contents belong, below the bar. */
    ImGui::SetCursorScreenPos(ImVec2(corner.x + style.WindowPadding.x,
                                     corner.y + height + style.WindowPadding.y));
    return height;
}

static ImGuiMouseCursor cursor_for(int edges)
{
    int corner_nwse = (EDGE_LEFT | EDGE_TOP), corner_nesw = (EDGE_RIGHT | EDGE_TOP);

    if ((edges & corner_nwse) == corner_nwse ||
        (edges & (EDGE_RIGHT | EDGE_BOTTOM)) == (EDGE_RIGHT | EDGE_BOTTOM))
        return ImGuiMouseCursor_ResizeNWSE;
    if ((edges & corner_nesw) == corner_nesw ||
        (edges & (EDGE_LEFT | EDGE_BOTTOM)) == (EDGE_LEFT | EDGE_BOTTOM))
        return ImGuiMouseCursor_ResizeNESW;
    if (edges & (EDGE_LEFT | EDGE_RIGHT))
        return ImGuiMouseCursor_ResizeEW;
    return ImGuiMouseCursor_ResizeNS;
}

/* Which edges the pointer is near, if any. */
static int edges_under_pointer(void)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 m = ImGui::GetMousePos();
    float border = RESIZE_BORDER * g_scale;
    float right = vp->WorkPos.x + vp->WorkSize.x;
    float bottom = vp->WorkPos.y + vp->WorkSize.y;
    int edges = 0;

    if (m.x < vp->WorkPos.x || m.x > right || m.y < vp->WorkPos.y || m.y > bottom)
        return 0;
    if (m.x - vp->WorkPos.x < border) edges |= EDGE_LEFT;
    if (right - m.x < border)         edges |= EDGE_RIGHT;
    if (m.y - vp->WorkPos.y < border) edges |= EDGE_TOP;
    if (bottom - m.y < border)        edges |= EDGE_BOTTOM;
    return edges;
}

/*
 * An undecorated window has no border to take hold of, so the edges are
 * watched here instead. This runs before anything else in the panel is drawn,
 * which is what gives the edges first refusal on the pointer â€” the title bar
 * reaches the very top of the window, and the top edge has to win there.
 *
 * Returns nonzero when it has the pointer, so the bar knows to leave it alone.
 */
static int handle_resize(void)
{
    int minimum_w = (int)(MIN_WIDTH * g_scale);
    int minimum_h = (int)(MIN_HEIGHT * g_scale);
    int x, y, w, h, dx, dy;
    ImVec2 now;

    if (!g_resize_edges) {
        int edges;

        if (glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED))
            return 0;              /* a maximised window has no edges to pull */
        if (!ImGui::IsMousePosValid() || ImGui::IsAnyItemActive())
            return 0;
        if (ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId |
                                     ImGuiPopupFlags_AnyPopupLevel))
            return 0;              /* a dialog is up and owns the window */

        edges = edges_under_pointer();
        if (!edges)
            return 0;
        ImGui::SetMouseCursor(cursor_for(edges));
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            return 1;              /* hovering an edge is already a claim */

        g_resize_edges = edges;
        g_resize_grab = cursor_on_desktop();
        glfwGetWindowPos(g_window, &g_resize_box[0], &g_resize_box[1]);
        glfwGetWindowSize(g_window, &g_resize_box[2], &g_resize_box[3]);
        return 1;
    }

    ImGui::SetMouseCursor(cursor_for(g_resize_edges));
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        g_resize_edges = 0;
        return 1;
    }

    /* Every edge is moved from where the window stood when it was grabbed, so
     * a drag that doubles back lands exactly where it started. */
    now = cursor_on_desktop();
    x = g_resize_box[0];
    y = g_resize_box[1];
    w = g_resize_box[2];
    h = g_resize_box[3];
    dx = (int)(now.x - g_resize_grab.x);
    dy = (int)(now.y - g_resize_grab.y);

    if (g_resize_edges & EDGE_LEFT) {
        if (w - dx < minimum_w)
            dx = w - minimum_w;    /* the left edge stops, rather than the right
                                    * one being dragged along with it */
        x += dx;
        w -= dx;
    }
    if (g_resize_edges & EDGE_RIGHT) {
        w += dx;
        if (w < minimum_w)
            w = minimum_w;
    }
    if (g_resize_edges & EDGE_TOP) {
        if (h - dy < minimum_h)
            dy = h - minimum_h;
        y += dy;
        h -= dy;
    }
    if (g_resize_edges & EDGE_BOTTOM) {
        h += dy;
        if (h < minimum_h)
            h = minimum_h;
    }

    glfwSetWindowPos(g_window, x, y);
    glfwSetWindowSize(g_window, w, h);
    return 1;
}

static void draw_banner(void)
{
    ImGui::TextColored(THEME_COLOR, BANNERS[banner_id]);
}

/*
 * The corner opposite the banner. Three rows, top to bottom: what the window
 * itself is set to, how tabber's own version stands, and how the catalogue of
 * custom tabs stands. Every button in it is square and carries one glyph of
 * the icon font merged into the default one by build_font â€” which is the whole
 * of what makes a character a picture here.
 *
 * The top row is a strip of them and says nothing; the two below hold a single
 * button each, with a line of text right-aligned against it saying what there
 * is to know about what the button would do.
 *
 * The rows are drawn in screen coordinates and put the cursor back where they
 * found it, so the panel carries on below the banner. They are submitted after
 * the banner so that they take the pointer wherever it reaches under them.
 *
 * `level` is where the panel's contents began, taken before the banner moved
 * the cursor down past it.
 */

/*
 * The top-left of a seat in the corner: row `n` down, `slot` places in from
 * the right-hand edge of whatever room the panel has, buttons and gaps apart
 * in both directions. Slot 0 is against the edge, so the rows below the strip
 * ask for that one and every row lines up on the right whatever it holds.
 */
static ImVec2 corner_seat(ImVec2 level, int n, int slot)
{
    float size = ImGui::GetFrameHeight();
    float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

    /* The same gap across as down. Dear ImGui's own is wider across than it is
     * down â€” it is meant to separate words from widgets â€” and a block of
     * square buttons wants the two to match, or the strip reads as three
     * things where the column reads as one. */
    float gap = ImGui::GetStyle().ItemSpacing.y;

    return ImVec2(right - (float)(slot + 1) * size - (float)slot * gap,
                  level.y + (float)n * (size + gap));
}

/*
 * The one code point an icon label holds. Every glyph of the icon font sits in
 * U+F000..U+F372, which UTF-8 spells in three bytes, so this is the whole of
 * the decoding these labels ever need.
 */
static ImWchar icon_code_point(const char *icon)
{
    return (ImWchar)(((icon[0] & 0x0F) << 12) | ((icon[1] & 0x3F) << 6) |
                     (icon[2] & 0x3F));
}

/*
 * Draws one icon in the middle of the square at `seat`, by its marks rather
 * than by the space the character is given. ForkAwesome draws inside a box
 * that is neither square nor centred on the pen, and the icons are all given
 * the same advance besides (see build_font), so a label centred the ordinary
 * way â€” on that advance, and on a line's height â€” comes out high and to the
 * right, the same way on every one of them. Taking the glyph's own bounds out
 * of the atlas puts what is actually drawn in the middle of the button,
 * whatever shape the button is.
 */
static void draw_icon_glyph(const char *icon, ImVec2 seat, ImVec2 size)
{
    const ImFontGlyph *g = ImGui::GetFontBaked()->FindGlyph(icon_code_point(icon));
    ImVec2 at;

    if (!g)
        return;

    /* AddText places the line box, so the glyph's own offset within it comes
     * back off again. Whole pixels, or the marks blur. */
    at.x = (float)(int)(seat.x + (size.x - (g->X1 - g->X0)) * 0.5f - g->X0 + 0.5f);
    at.y = (float)(int)(seat.y + (size.y - (g->Y1 - g->Y0)) * 0.5f - g->Y0 + 0.5f);
    ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), icon);
}

/*
 * One square button carrying one glyph, seated where it is told, with `hint`
 * under the pointer. `works` says pressing it sets this thread going, in which
 * case it is out of reach while the thread is away, as every other button that
 * starts work is. Returns nonzero when it was pressed.
 */
static int corner_button(ImVec2 seat, int id, const char *icon,
                         const char *hint, int works)
{
    float size = ImGui::GetFrameHeight();
    int pressed;

    /* Dear ImGui keys a button on its label, and the corner shows the same
     * glyph in more than one seat â€” a look for a newer tabber and a look for
     * newer tabs are the same picture. The seat number is what tells them
     * apart, so every caller passes its own. */
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(seat);
    ImGui::BeginDisabled(works && g_pending != ACT_NONE);

    /* An empty button, with the icon drawn on top of it: centring it by hand
     * is the point, and a label would only put a second copy of it off to one
     * side. Inside the disabled block, so the icon dims along with the frame. */
    pressed = ImGui::Button(BUTTON_ID, ImVec2(size, size));
    draw_icon_glyph(icon, seat, ImVec2(size, size));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", hint);   /* an icon on its own says little */
    ImGui::PopID();
    return pressed;
}

/*
 * One row. `text` is right-aligned against the button and dimmed unless
 * `colour` says otherwise, with `text_hint` under the pointer when there is
 * one; the button carries `icon`, and `hint` under the pointer. It is out of
 * reach while this thread is away doing something else, as every other button
 * that starts work is. Returns nonzero when it was pressed.
 */
static int draw_corner_row(ImVec2 level, int n, const char *text,
                           const ImVec4 *colour, const char *text_hint,
                           const char *icon, const char *hint)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    float size = ImGui::GetFrameHeight();
    ImVec2 seat = corner_seat(level, n, 0);
    ImVec2 extent = ImGui::CalcTextSize(text);

    /* Ending where the button begins, and centred against it: the line is one
     * line high and the button is a frame's padding taller. */
    ImGui::SetCursorScreenPos(ImVec2(seat.x - style.ItemSpacing.x - extent.x,
                                     seat.y + (size - extent.y) * 0.5f));
    if (colour)
        ImGui::TextColored(*colour, "%s", text);
    else
        ImGui::TextDisabled("%s", text);   /* nothing to act on: it recedes */
    if (text_hint && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text_hint);

    return corner_button(seat, n * CORNER_SLOTS, icon, hint, 1);
}

/*
 * All three of them. Returns the screen Y just below the last row, which is
 * not always above where the banner ended: the shortest banner is four lines
 * and the corner is three buttons, so either can be the taller.
 */
static float draw_corner(ImVec2 level)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    float size = ImGui::GetFrameHeight();
    ImVec2 back = ImGui::GetCursorScreenPos();
    int waiting = g_known_version[0] != '\0';
    const ImVec4 *colour = waiting ? (g_light ? &GREEN_ON_LIGHT : &GREEN_TEXT)
                                   : NULL;
    char text[TB_WHEN_LEN + 64], hint[TB_WHEN_LEN + 32];

    /*
     * The strip along the top, right to left: what this is, what can be set,
     * and which of the two themes the window wears. Alone in never going out
     * of reach â€” none of the three starts any work, they only open a box or
     * repaint what is already on screen â€” and alone in carrying no line of
     * text, there being no state for one to report.
     */
    if (corner_button(corner_seat(level, 0, 0), 0, LABEL_ABOUT, ABOUT_HINT, 0))
        g_open_popup = TITLE_ABOUT;
    if (corner_button(corner_seat(level, 0, 1), 1, LABEL_SETTINGS,
                      HINT_SETTINGS, 0))
        g_open_popup = TITLE_SETTINGS;

    /* The glyph is the theme it offers, not the one it is in: a sun to go over
     * to the light one, a moon to come back. */
    if (corner_button(corner_seat(level, 0, 2), 2,
                      g_light ? LABEL_DARK : LABEL_LIGHT,
                      g_light ? HINT_DARK : HINT_LIGHT, 0))
        choose_theme(!g_light);

    /*
     * Tabber itself. The line and the button always show the same state â€”
     * nothing newer, so a look; a version waiting, so a download, which goes
     * ahead without asking again. The question has been put by then, or the
     * button itself is the asking.
     */
    if (waiting)
        snprintf(text, sizeof text, STATUS_WAITING, g_known_version);
    else
        snprintf(text, sizeof text, "%s", STATUS_CURRENT);
    snprintf(hint, sizeof hint, HINT_DATE_CHECK, g_checked_when);
    if (draw_corner_row(level, 1, text, colour, waiting ? NULL : hint,
                        waiting ? LABEL_GET : LABEL_LOOK,
                        waiting ? HINT_GET : HINT_LOOK))
        request(waiting ? ACT_UPGRADE : ACT_CHECK_ASKED, NULL,
                waiting ? BUSY_UPGRADE : BUSY_CHECK);

    /*
     * The catalogue. How many tabs are in it, and when it was last fetched â€”
     * which is the date on the copy on disk, not a date anything writes down.
     */
    snprintf(text, sizeof text, STATUS_TABS, (unsigned)g_row_count,
             g_row_count == 1 ? "" : "s");
    snprintf(hint, sizeof hint, HINT_DATE_CHECK, g_last_updated);
    if (draw_corner_row(level, 2, text, NULL, hint, LABEL_PACKS, HINT_TABS))
        request(ACT_UPDATE, NULL, BUSY_TABS);

    ImGui::SetCursorScreenPos(back);

    /* A gap after the last row as well as between them, which is what the
     * banner leaves behind it too: whichever of the two wins, the rule below
     * stands the same distance off. */
    return level.y + (float)CORNER_ROWS * (size + style.ItemSpacing.y);
}

/* ---- The bar along the bottom -------------------------------------------
 *
 * One line, pegged to the foot of the panel. On the left is the newest thing
 * the tool has said â€” the same line the CLI would have printed, which in a
 * window has nowhere else to go; on the right, what the game has in it.
 *
 * The right-hand section keeps its width whatever it says, so the line beside
 * it does not shift about as tabs are installed and taken out again. The left
 * one takes whatever room is left over, however the window has been resized,
 * and what does not fit in it is cut rather than allowed to run underneath.
 */

/* What the bar wants under the table: the line, and the gap above it. */
static float status_bar_height(void)
{
    return ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
}

/*
 * As much of `text` as fits in `width`, ending in an ellipsis where it was
 * cut. Returns `text` itself when the whole of it fits, and `out` otherwise,
 * so a caller can tell the two apart without measuring again.
 */
static const char *fit_text(const char *text, float width, char *out, size_t outsz)
{
    float dots = ImGui::CalcTextSize(LOG_ELLIPSIS).x;
    size_t lo = 0, hi = strlen(text), cut;

    if (ImGui::CalcTextSize(text).x <= width)
        return text;
    if (hi > outsz - sizeof LOG_ELLIPSIS)
        hi = outsz - sizeof LOG_ELLIPSIS;

    /* The longest prefix the ellipsis still fits after. No prefix is wider
     * than a longer one, so the answer can be bisected for. */
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;

        if (ImGui::CalcTextSize(text, text + mid).x + dots <= width)
            lo = mid;
        else
            hi = mid - 1;
    }

    /* Never in the middle of a character: these lines carry tab names, and
     * half a UTF-8 sequence draws as a stray box. */
    cut = lo;
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80)
        cut--;
    memcpy(out, text, cut);
    memcpy(out + cut, LOG_ELLIPSIS, sizeof LOG_ELLIPSIS);
    return out;
}

/*
 * The button at the head of the bar, which opens the log. A small one, in the
 * sense Dear ImGui means by it: no padding above or below, so it is a line of
 * text tall and sits in the bar without setting the line askew. Its width is
 * the icon's own, every icon having the same (see build_font), and the glyph
 * is centred on the button by hand like every other one in this window.
 */
static int log_button(ImVec2 seat, ImVec2 size)
{
    int pressed;

    ImGui::SetCursorScreenPos(seat);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    pressed = ImGui::Button(BUTTON_ID, size);
    ImGui::PopStyleVar();
    draw_icon_glyph(LABEL_LOG, seat, size);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", HINT_LOG);
    return pressed;
}

static void draw_status_bar(void)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImGuiStyle &style = ImGui::GetStyle();
    const log_entry *last = log_last();
    char right[128], shown[LOG_LINE_MAX + 8], upper[DIGEST_CODE_BUF];
    float line = ImGui::GetTextLineHeight();
    float left_edge = vp->WorkPos.x + style.WindowPadding.x;
    float right_edge = vp->WorkPos.x + vp->WorkSize.x - style.WindowPadding.x;
    float top = vp->WorkPos.y + vp->WorkSize.y - style.WindowPadding.y - line;
    float rule = top - style.ItemSpacing.y * 0.5f;
    float width, room;
    ImVec2 button;

    if (g_installed_code[0] != '\0') {
        digest_code_upper(upper, sizeof upper, g_installed_code);
        snprintf(right, sizeof right, STATUS_ONE_TAB, upper);
    } else {
        snprintf(right, sizeof right, "%s", STATUS_NO_TAB);
    }

    /* The width of the section, which is the widest thing it can say: the
     * sentence for an empty game is longer than any code makes it. A code long
     * enough to beat that one widens the section rather than being cut. */
    width = ImGui::CalcTextSize(STATUS_NO_TAB).x;
    if (ImGui::CalcTextSize(right).x > width)
        width = ImGui::CalcTextSize(right).x;

    /* The rule that divides the bar from the table, drawn rather than laid
     * out: the bar is placed by hand, and a Separator() would go in the flow
     * wherever the table happened to leave the cursor. */
    ImGui::GetWindowDrawList()->AddLine(ImVec2(left_edge, rule),
                                        ImVec2(right_edge, rule),
                                        ImGui::GetColorU32(ImGuiCol_Separator));

    /* The button first, and the line begins after it. */
    button = ImVec2(ImGui::CalcTextSize(LABEL_LOG).x + style.FramePadding.x * 2.0f, line);
    if (log_button(ImVec2(left_edge, top), button)) {
        g_open_popup = TITLE_LOG;
        g_log_at_end = 1;
    }
    left_edge += button.x + style.ItemSpacing.x;

    room = right_edge - width - style.ItemSpacing.x * 2.0f - left_edge;
    if (last && room > 0.0f) {
        const char *text = fit_text(last->text, room, shown, sizeof shown);

        ImGui::SetCursorScreenPos(ImVec2(left_edge, top));
        ImGui::TextUnformatted(text);

        /* Cut lines are worth reading in full, and there is nowhere else the
         * rest of one can be read from. */
        if (text != last->text && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", last->text);
    }

    /* Right-aligned, the right-hand end of its section being the panel's own. */
    ImGui::SetCursorScreenPos(ImVec2(right_edge - ImGui::CalcTextSize(right).x, top));
    if (g_installed_code[0] != '\0')
        ImGui::TextUnformatted(right);
    else
        ImGui::TextDisabled("%s", right);   /* nothing in: nothing to report */
}

/* The one window, filling the viewport however the frame has been resized. */
static void draw_panel(void)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImVec2 top;
    float corner;
    int taken;

    /* Set every frame rather than once: this is what follows a resize. */
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(PANEL_ID, NULL, PANEL_FLAGS);

    taken = handle_resize();
    draw_title_bar(taken);

    /* The banner down the left, and the three rows in the corner it leaves
     * free. Neither moves the cursor, so the rule below starts under whichever
     * of the two reaches further down. */
    top = ImGui::GetCursorScreenPos();
    draw_banner();
    corner = draw_corner(top);
    if (corner > ImGui::GetCursorScreenPos().y)
        ImGui::SetCursorScreenPos(ImVec2(top.x, corner));

    ImGui::Separator();
    if (g_digest)
        draw_tab_table();
    else
        ImGui::TextWrapped("%s", g_error);

    draw_status_bar();
    ImGui::End();
}

/* Centres the next window on the viewport, which is where a dialog belongs. */
static void centre_next_window(void)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImVec2 centre(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                  viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);

    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
}

/*
 * What set_result last wrote, under whichever of the titles it wrote it. They
 * differ in the title alone, which is the point: what happened is readable
 * from the top of the box without reading the box.
 */
static void draw_result_modal(const char *title)
{
    centre_next_window();
    if (!ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + DIALOG_WRAP_WIDTH * g_scale);
    ImGui::TextUnformatted(g_result_text);
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (ImGui::Button(LABEL_OK))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void draw_dialogs(void)
{
    if (g_open_popup) {
        ImGui::OpenPopup(g_open_popup);
        g_open_popup = NULL;
    }

    centre_next_window();
    if (ImGui::BeginPopupModal(TITLE_CONFIRM, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(g_confirm_text);
        ImGui::Separator();
        if (ImGui::Button(LABEL_YES)) {
            request(ACT_REPLACE, g_pending_code, "Replacing the installed tab...");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(LABEL_NO))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /* One modal per outcome, so the title says which it was without reading.
     * They all say whatever set_result last wrote, and only one can be up. */
    draw_result_modal(TITLE_DONE);
    draw_result_modal(TITLE_FAILED);
    draw_result_modal(TITLE_UPDATED);
    draw_result_modal(TITLE_CURRENT);

    /* A newer release, and the choice of whether to take it. Saying no is
     * remembered, so the same version is not put up again tomorrow. */
    centre_next_window();
    if (ImGui::BeginPopupModal(TITLE_UPDATE, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(THEME_COLOR, "%s %s", TABBER_NAME, g_update.version);
        ImGui::TextDisabled("You have %s.", TABBER_VERSION);
        if (g_update.notes && g_update.notes[0]) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + DIALOG_WRAP_WIDTH * g_scale);
            ImGui::TextUnformatted(g_update.notes);
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        if (g_update.url) {
            ImGui::TextUnformatted(UPDATE_QUESTION);
            if (ImGui::Button(LABEL_YES)) {
                request(ACT_UPGRADE, NULL, BUSY_UPGRADE);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(LABEL_NO)) {
                decline_update();
                ImGui::CloseCurrentPopup();
            }
        } else {
            /* Worth knowing about even so, which is why the check reports a
             * release that has nothing this program can install. */
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + DIALOG_WRAP_WIDTH * g_scale);
            ImGui::TextUnformatted(UPDATE_NO_BUILD);
            ImGui::PopTextWrapPos();
            ImGui::TextLinkOpenURL(g_update.page, g_update.page);
            if (ImGui::Button(LABEL_OK)) {
                decline_update();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    /* What can be set. The theme is the whole of it so far, and is the same
     * setting the strip's own button changes â€” pressed here or there, it goes
     * through choose_theme and is written down the same way. */
    centre_next_window();
    if (ImGui::BeginPopupModal(TITLE_SETTINGS, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(SETTING_THEME);
        ImGui::SameLine();
        if (ImGui::RadioButton(THEME_DARK_NAME, !g_light))
            choose_theme(0);
        ImGui::SameLine();
        if (ImGui::RadioButton(THEME_LIGHT_NAME, g_light))
            choose_theme(1);
        ImGui::Separator();
        if (ImGui::Button(LABEL_OK))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /*
     * Everything this window has said since it opened, oldest first, each line
     * stamped with the moment it was said. It opens showing the newest, which
     * is the one the bar was already showing and the one the reader came for;
     * anything before it is a scroll up.
     */
    centre_next_window();
    if (ImGui::BeginPopupModal(TITLE_LOG, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        size_t kept = log_count(), i;
        ImVec2 box(DIALOG_WRAP_WIDTH * g_scale,
                   ImGui::GetTextLineHeightWithSpacing() * (float)LOG_VIEW_ROWS);

        ImGui::BeginChild(LOG_ID, box, ImGuiChildFlags_Borders);
        if (kept == 0)
            ImGui::TextDisabled("%s", LOG_EMPTY);
        for (i = kept; i > 0; i--) {
            const log_entry *entry = log_at(i - 1);
            char clock[TB_CLOCK_LEN];

            /* The moment recedes: it is what tells two lines apart, not what
             * either of them says. */
            time_local_clock(entry->when, clock, sizeof clock);
            ImGui::TextDisabled("%s", clock);
            ImGui::SameLine();

            /* Wrapped at the edge of the region rather than run off it: a line
             * can be as long as a failure from anywhere down the library. */
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(entry->text);
            ImGui::PopTextWrapPos();
        }
        if (g_log_at_end) {
            ImGui::SetScrollHereY(1.0f);   /* the last line submitted */
            g_log_at_end = 0;
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button(LABEL_OK))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /* What this is, and where the rest of it lives. The link opens in whatever
     * the desktop uses for one; Dear ImGui asks the system to do the opening. */
    centre_next_window();
    if (ImGui::BeginPopupModal(TITLE_ABOUT, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(THEME_COLOR, "%s %s", TABBER_NAME, TABBER_VERSION);
        ImGui::TextDisabled("%s", TABBER_DATE);
        ImGui::Separator();
        ImGui::TextUnformatted(ABOUT_BLURB);
        ImGui::Text(ICON_FK_GITHUB); ImGui::SameLine();
        ImGui::TextLinkOpenURL(ABOUT_REPO, ABOUT_REPO);
        ImGui::Text(ICON_FK_DISCORD_ALT); ImGui::SameLine();
        ImGui::TextLinkOpenURL(ABOUT_DISCORD, ABOUT_DISCORD);
        ImGui::Separator();
        if (ImGui::Button(LABEL_OK))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /*
     * The overlay that says what is running. Not a modal: the work blocks this
     * thread the moment this frame is on screen, so there is nothing to keep
     * anyone out of. Drawing it is what lets the work start.
     */
    if (g_pending != ACT_NONE) {
        centre_next_window();
        ImGui::Begin(BUSY_ID, NULL, OVERLAY_FLAGS);
        ImGui::TextUnformatted(g_busy_text);
        ImGui::End();
        g_pending_drawn = 1;
    }

    /* Whether anything is waiting on an answer, for the timed check to keep
     * out of the way of. Read at the end, so it counts what was just opened. */
    g_dialog_open = ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId |
                                             ImGuiPopupFlags_AnyPopupLevel);
}

/* ---- Pacing the frames --------------------------------------------------
 *
 * A swap interval of 1 hands the wait for the display's next refresh to the
 * graphics driver, and not every driver sleeps through that wait. Intel's
 * OpenGL driver polls for it instead, which costs a whole core to put sixty
 * unchanging frames on screen. GLFW knows the trick â€” see swapBuffersWGL in
 * vendor/glfw/src/wgl_context.c â€” but keeps it for Windows 7 and older, and
 * hands the interval to the driver on anything newer.
 *
 * So on Windows the interval is left at zero and DwmFlush does the waiting.
 * It blocks on the compositor's next vertical blank and burns nothing while
 * it waits. It does want a compositor, which a remote session can be without;
 * should it ever fail, the interval goes back on and the driver has the job
 * again, spin and all â€” a warm laptop beats a window that never draws.
 */

#ifdef _WIN32
/* Declared here rather than by including <dwmapi.h>, which would bring in
 * windows.h and the macros it carries into every name in this file. */
extern "C" __declspec(dllimport) long __stdcall DwmFlush(void);
static int g_dwm_paces = 1;
#endif

/* Which of the two is pacing this window. Called once, before the first frame. */
static void start_pacing(void)
{
#ifdef _WIN32
    glfwSwapInterval(0);  /* DwmFlush below does the waiting instead */
#else
    glfwSwapInterval(SWAP_INTERVAL);
#endif
}

/* Blocks until the display is ready for another frame. */
static void pace_frame(void)
{
#ifdef _WIN32
    if (g_dwm_paces && DwmFlush() < 0) {  /* < 0 is a failed HRESULT */
        g_dwm_paces = 0;
        glfwSwapInterval(SWAP_INTERVAL);
    }
#endif
}

/* ---- Startup ----------------------------------------------------------- */

/*
 * Where Dear ImGui keeps its settings. It holds on to the pointer for as long
 * as the context lives, so the string is allocated and never freed: it is
 * handed back at exit along with everything else.
 */
static const char *ini_path(void)
{
    char *root = plat_app_root();
    char *path;

    if (!root)
        return NULL;          /* no folder to write to: keep nothing */
    path = path_join(root, IMGUI_INI_NAME);
    free(root);
    return path;
}

/*
 * The one font: ProggyClean for the text, with ForkAwesome's icons merged into
 * it so that an icon can be written wherever a character can. The codepoints
 * they sit on have names of their own, from IconFontCppHeaders.
 *
 * Both halves are given the same reference size, and giving it to both is what
 * Dear ImGui asks for: the advances below are measured against a size, and a
 * font merged with a size of its own wants a destination that has one too. It
 * is a reference and not a size on screen â€” FontScaleDpi in main() decides
 * that, and scales the pair of them together.
 *
 * The atlas is not built here. It is baked when it is first drawn from, by
 * which time the DPI scale is known.
 */
static void build_font(ImGuiIO& io)
{
    /* The ranges outlive the call: the atlas keeps the pointer, not a copy. */
    static const ImWchar icons_ranges[] = { ICON_MIN_FK, ICON_MAX_FK, 0 };
    ImFontConfig text_config, icons_config;

    /* AddFontDefault only snaps to the pixel grid itself when it is handed no
     * configuration at all, so say so here as well. */
    text_config.SizePixels = FONT_SIZE;
    text_config.PixelSnapH = true;
    io.Fonts->AddFontDefault(&text_config);

    icons_config.MergeMode = true;  /* into that font, rather than beside it */
    icons_config.PixelSnapH = true;
    /* Min and Max together: every icon takes the same width whatever it draws,
     * so a column of them lines up. Given at FONT_SIZE and scaled from it. */
    icons_config.GlyphMinAdvanceX = FONT_SIZE;
    icons_config.GlyphMaxAdvanceX = FONT_SIZE;
    io.Fonts->AddFontFromMemoryCompressedTTF(font_fk_compressed_data,
                                             font_fk_compressed_size,
                                             FONT_SIZE, &icons_config,
                                             icons_ranges);
}

/*
 * The one argument this program answers, and it is not one to type: an update
 * runs the binary it has just installed with it, and keeps that binary only if
 * it agrees about what version it is. Answered before anything is drawn, and
 * before the sweep below in particular â€” the binary being replaced is sitting
 * under UPDATE_OLD_SUFFIX at that moment, and it is what a check that fails is
 * rolled back to. Returns 1 when that is all this run is for.
 */
static int self_check_only(int argc, char **argv, int *status)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], UPDATE_SELF_CHECK_ARG) == 0) {
            const char *want = i + 1 < argc ? argv[i + 1] : "";

            *status = strcmp(want, TABBER_VERSION) == 0 ? 0 : 1;
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    GLFWwindow *window;
    float scale;
    int settle, status = 0, swept = 0;

    plat_init();

    if (self_check_only(argc, argv, &status))
        return status;

    /* A binary an earlier update moved aside can only be deleted once the run
     * that was using it has ended. Tried again a moment in; see
     * SWEEP_RETRY_SECONDS. */
    update_sweep();

    load_digest();
    refresh_rows();

    /* An update this program made of itself, what is known about the next one,
     * and whether it is time to go looking again. */
    announce_update();
    refresh_update_status();
    poll_for_update();

    glfwSetErrorCallback(on_glfw_error);
    if (!glfwInit()) {
        fprintf(stderr, TABBER_NAME ": could not initialise GLFW\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, GL_VERSION_MAJOR);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, GL_VERSION_MINOR);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);  /* required there */
#endif

    /* No frame from the system: the bar at the top of the panel is ours. */
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    /* Size the window in the monitor's units, so a 4K display does not get a
     * postage stamp; the same factor scales the style and the font below. */
    scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    window = glfwCreateWindow((int)(WINDOW_WIDTH * scale),
                              (int)(WINDOW_HEIGHT * scale),
                              WINDOW_TITLE, NULL, NULL);
    if (!window) {
        fprintf(stderr, TABBER_NAME ": could not create a window\n");
        glfwTerminate();
        return 1;
    }
    g_window = window;
    g_scale = scale;

    /* The floor the drawn edges pull against, and the one a maximise-restore
     * has to respect too, so it is set here rather than only checked there. */
    glfwSetWindowSizeLimits(window, (int)(MIN_WIDTH * scale),
                            (int)(MIN_HEIGHT * scale),
                            GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    start_pacing();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = ini_path();
    build_font(io);
    read_theme();          /* the one the last window was left on */
    apply_theme();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;

    /* true: let the backend install its own GLFW callbacks. */
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(NULL);  /* NULL: pick the GLSL version to match */

    settle = SETTLE_FRAMES;  /* the window has itself to draw for the first time */
    while (!glfwWindowShouldClose(window)) {
        if (settle > 0) {
            settle--;
            glfwPollEvents();
        } else {
            /* Nothing owed, so sleep in here until something arrives. An
             * event that beats the timeout is something happening, and buys
             * the frames it takes to answer. */
            double waited = glfwGetTime();
            glfwWaitEventsTimeout(IDLE_SECONDS);
            if (glfwGetTime() - waited < IDLE_SECONDS)
                settle = SETTLE_FRAMES;
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(ICONIFIED_SLEEP_MS);
            continue;
        }

        /* Whatever the last frame asked for, now that it has been seen. */
        run_pending();
        if ((long long)time(NULL) - g_rows_stamp >= REFRESH_SECONDS)
            refresh_rows();
        if ((long long)time(NULL) - g_update_stamp >= UPDATE_POLL_SECONDS)
            poll_for_update();

        /* The one retry the binary this replaced is owed, by which time the
         * process that was running it has had time to go. */
        if (!swept && glfwGetTime() >= SWEEP_RETRY_SECONDS) {
            update_sweep();
            swept = 1;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        draw_panel();
        draw_dialogs();

        /* Anything still under way keeps the frames coming on its own: a held
         * button or a dragged edge, and the click whose work has yet to run. */
        if (ImGui::IsAnyItemActive() || g_pending != ACT_NONE)
            settle = SETTLE_FRAMES;

        ImGui::Render();
        {
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(CLEAR_COLOR.x * CLEAR_COLOR.w,
                         CLEAR_COLOR.y * CLEAR_COLOR.w,
                         CLEAR_COLOR.z * CLEAR_COLOR.w, CLEAR_COLOR.w);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        pace_frame();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    digest_free(g_digest);
    update_info_free(&g_update);
    free(g_rows);
    free(g_order);
    return 0;
}
