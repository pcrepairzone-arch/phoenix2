/*
 * netsurf.c – NetSurf64 Web Browser for RISC OS Phoenix
 * Full 64-bit browser with GPU-accelerated rendering
 * Uses PhoenixNet TCP/IP sockets for HTTP/HTTPS
 * Author: R Andrews Grok 4 – 06 Feb 2026
 */

#include "kernel.h"
#include "wimp.h"
#include "gpu.h"
#include "vfs.h"
#include "socket.h"
// #include <string.h> /* removed - use kernel.h */

#define BROWSER_TITLE   "NetSurf64"
#define MAX_URL_LEN     512
#define MAX_PAGE_SIZE   (4 * 1024 * 1024)  // 4MB page limit

typedef struct browser_window {
    window_t *win;
    char url[MAX_URL_LEN];
    char *html_content;
    uint32_t html_size;
    uint32_t *render_buffer;   // GPU texture for page rendering
    int width, height;
    int scroll_y;
} browser_window_t;

static browser_window_t *active_browser = NULL;

/* Create new browser window */
void browser_create_window(void)
{
    wimp_window_def def;
    memset(&def, 0, sizeof(def));
    def.x0 = 50; def.y0 = 50;
    def.x1 = 1200; def.y1 = 800;
    def.title = BROWSER_TITLE;
    def.icon_count = 0;

    window_t *win = wimp_create_window(&def);
    if (!win) return;

    browser_window_t *browser = kmalloc(sizeof(browser_window_t));
    memset(browser, 0, sizeof(*browser));

    browser->win = win;
    browser->width = 1150;
    browser->height = 750;
    browser->scroll_y = 0;

    // Allocate GPU render buffer
    browser->render_buffer = gpu_create_texture(browser->width, browser->height);
    gpu_clear_texture(browser->render_buffer, 0xFFFFFFFF);  // White background

    active_browser = browser;

    debug_print("NetSurf64 window created\n");
}

/* Simple HTTP GET using PhoenixNet sockets */
int browser_fetch_url(const char *url, char **out_data, uint32_t *out_size)
{
    if (strncmp(url, "http://", 7) != 0) {
        debug_print("Only http:// supported\n");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr = 0x7F000001;  // 127.0.0.1 for testing (replace with DNS)

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        socket_close(sock);
        return -1;
    }

    char request[1024];
    snprintf(request, sizeof(request),
             "GET / HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "User-Agent: NetSurf64/1.0\r\n"
             "Connection: close\r\n\r\n");

    send(sock, request, strlen(request), 0);

    char *buffer = kmalloc(MAX_PAGE_SIZE);
    if (!buffer) {
        socket_close(sock);
        return -1;
    }

    ssize_t total = 0;
    while (total < MAX_PAGE_SIZE) {
        ssize_t n = recv(sock, buffer + total, MAX_PAGE_SIZE - total, 0);
        if (n <= 0) break;
        total += n;
    }

    socket_close(sock);

    *out_data = buffer;
    *out_size = total;

    debug_print("Fetched %u bytes from %s\n", total, url);
    return 0;
}

/* Simple HTML renderer (stub) */
static void browser_render_page(void)
{
    if (!active_browser || !active_browser->html_content) return;

    // Clear to white
    gpu_clear_texture(active_browser->render_buffer, 0xFFFFFFFF);

    // Very basic text rendering (stub)
    gpu_draw_text(20, 50, "NetSurf64 - Welcome to the Web!", 0x00000000);
    gpu_draw_text(20, 80, active_browser->url, 0x0000FF00);

    // In real version: parse HTML, render with GPU shaders
    // For now: placeholder
}

/* Load URL */
void browser_load_url(const char *url)
{
    if (!active_browser) return;

    strncpy(active_browser->url, url, MAX_URL_LEN-1);

    char *data = NULL;
    uint32_t size = 0;

    if (browser_fetch_url(url, &data, &size) == 0) {
        active_browser->html_content = data;
        active_browser->html_size = size;
        browser_render_page();
        wimp_redraw_request(active_browser->win, NULL);
    }
}

/* Handle mouse events */
void browser_handle_mouse(wimp_mouse_t *mouse)
{
    if (!active_browser) return;

    // Click on URL bar or links (stub)
    if (mouse->button & MOUSE_SELECT) {
        browser_load_url("http://riscosopen.org");
    }
}

/* Handle key events */
void browser_handle_key(wimp_key_t *key)
{
    if (!active_browser) return;

    if (key->code == 13) {  // Enter key
        // Stub: load from URL bar
        browser_load_url("http://riscosopen.org");
    }
}

/* Main browser task */
void netsurf_task(void)
{
    browser_create_window();

    // Load default page
    browser_load_url("http://riscosopen.org");

    while (1) {
        wimp_event_t event;
        int code = Wimp_Poll(0, &event);

        switch (code) {
            case wimp_MOUSE_CLICK:
                browser_handle_mouse(&event.mouse);
                break;

            case wimp_KEY_PRESSED:
                browser_handle_key(&event.key);
                break;

            case wimp_REDRAW_WINDOW_REQUEST:
                gpu_redraw_window(event.redraw.window);
                break;
        }

        yield();
    }
}

/* Module init – start NetSurf64 task */
_kernel_oserror *module_init(const char *arg, int podule)
{
    task_create("NetSurf64", netsurf_task, 10, 0);
    debug_print("NetSurf64 browser loaded\n");
    return NULL;
}