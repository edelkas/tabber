/*
 * main.cpp - Entry point of the graphical front-end.
 *
 * Dear ImGui on GLFW + OpenGL 3, both built from source under vendor/. The
 * window is a single full-viewport panel holding the catalogue of custom tabs,
 * the same four columns the CLI's `list` prints.
 *
 * This is a second front-end onto the library in src/, not a second copy of
 * it: everything below the presentation is the code the CLI runs.
 */

#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

/* Brings in the system's OpenGL header, so it goes after the backends. */
#include <GLFW/glfw3.h>

/* The tool's own library. Its headers carry their own extern "C". */
#include "digest.h"
#include "platform.h"
#include "util.h"
#include "version.h"

/* Window and context. GL 3.2 is the floor: it is what the OpenGL 3 backend
 * asks for and the oldest core profile macOS will hand out. */
static const char *WINDOW_TITLE      = TABBER_NAME " " TABBER_VERSION;
static const int   WINDOW_WIDTH      = 1000;
static const int   WINDOW_HEIGHT     = 700;
static const int   GL_VERSION_MAJOR  = 3;
static const int   GL_VERSION_MINOR  = 2;
static const int   SWAP_INTERVAL     = 1;  /* vsync: an idle GUI must not spin */

/* Milliseconds to idle for per frame while the window is minimised, when there
 * is nothing to draw and polling would otherwise burn a core. */
static const int   ICONIFIED_SLEEP_MS = 10;

/* Background behind the panel, straight from the Dear ImGui example. */
static const ImVec4 CLEAR_COLOR(0.45f, 0.55f, 0.60f, 1.00f);

/*
 * Dear ImGui's own state (column widths and the like) goes in the tool's
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
    ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

/* Identifiers Dear ImGui keys its state on. They are not shown to anyone. */
static const char *PANEL_ID = "tabber-panel";
static const char *TABLE_ID = "tabber-tabs";

/* What is on screen: the catalogue, or why there is none. */
static digest *g_digest = NULL;
static char    g_error[TB_ERR_LEN] = "";

static void on_glfw_error(int error, const char *description)
{
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

/*
 * Loads the catalogue, refreshing it from the network first. A refresh that
 * fails is not fatal — the cached copy is what the CLI falls back on too — so
 * only a failure to load at all leaves g_error set.
 */
static void load_digest(void)
{
    char err[TB_ERR_LEN];

    if (digest_ensure_fresh(0, err, sizeof err) != 0)
        fprintf(stderr, TABBER_NAME ": could not refresh the digest (%s), using the cached copy\n", err);

    g_digest = digest_load(err, sizeof err);
    if (!g_digest)
        snprintf(g_error, sizeof g_error, "%s", err);
}

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

/* The catalogue as a table, one row per tab, the columns `list` prints. */
static void draw_tab_table(void)
{
    size_t i;

    if (!ImGui::BeginTable(TABLE_ID, 4, TABLE_FLAGS, ImGui::GetContentRegionAvail()))
        return;

    /* The two fixed-width columns hold values of a known size; the two that
     * carry free text share whatever is left. */
    ImGui::TableSetupColumn(COL_CODE, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(COL_NAME, ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(COL_AUTHORS, ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(COL_DATE, ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupScrollFreeze(0, 1);   /* the header stays put */
    ImGui::TableHeadersRow();

    for (i = 0; i < g_digest->tab_count; i++) {
        const npp_tab *tab = &g_digest->tabs[i];
        char code[DIGEST_CODE_BUF], date[DIGEST_DATE_BUF];

        digest_code_upper(code, sizeof code, tab->code);
        digest_date_short(date, sizeof date, tab->date);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(code);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(tab->name ? tab->name : "");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(tab->authors ? tab->authors : "");
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(date);
    }
    ImGui::EndTable();
}

/* The one window, filling the viewport however the frame has been resized. */
static void draw_panel(void)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();

    /* Set every frame rather than once: this is what follows a resize. */
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(PANEL_ID, NULL, PANEL_FLAGS);

    if (g_digest) {
        ImGui::Text("%u custom tab(s) available.", (unsigned)g_digest->tab_count);
        ImGui::Separator();
        draw_tab_table();
    } else {
        ImGui::TextWrapped("%s", g_error);
    }

    ImGui::End();
}

int main(int argc, char **argv)
{
    GLFWwindow *window;
    float scale;

    (void)argc;
    (void)argv;

    plat_init();
    load_digest();

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
    glfwMakeContextCurrent(window);
    glfwSwapInterval(SWAP_INTERVAL);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().IniFilename = ini_path();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;

    /* true: let the backend install its own GLFW callbacks. */
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(NULL);  /* NULL: pick the GLSL version to match */

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(ICONIFIED_SLEEP_MS);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        draw_panel();

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
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    digest_free(g_digest);
    return 0;
}
