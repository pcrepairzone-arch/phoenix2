#ifndef WIMP_H
#define WIMP_H

typedef struct window window_t;
typedef struct wimp_window_def wimp_window_def;
typedef struct bbox bbox_t;
typedef struct wimp_event wimp_event_t;

struct wimp_window_def {
    int x0, y0, x1, y1;
    char *title;
    int icon_count;
};

struct bbox {
    int x0, y0, x1, y1;
};

struct wimp_event {
    int type;
    union {
        struct {
            window_t *window;
            bbox_t clip;
        } redraw;
    } u;
};

#endif