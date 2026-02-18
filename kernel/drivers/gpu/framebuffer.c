/*
 * framebuffer.c - Pi 4 legacy mailbox framebuffer + drawing
 */
#include "kernel.h"
#include "framebuffer.h"
#include "mailbox.h"
#include "font8x8.h"
#include <stdarg.h>

framebuffer_t fb = {0};

/* 16-byte aligned message buffer in .data (not .bss) */
static volatile uint32_t __attribute__((aligned(16))) mbox[36] = {0};

int fb_init(uint32_t w, uint32_t h)
{
    debug_print("fb_init: %ux%u mbox@0x%x\n", w, h, (uint32_t)(uint64_t)mbox);
    for (int i = 0; i < 36; i++) mbox[i] = 0;

    int i = 0;
    mbox[i++] = 0;          /* [0]  size (fill below)  */
    mbox[i++] = 0;          /* [1]  0=request           */
    mbox[i++] = 0x00048003; /* SET_PHYS_WH              */
    mbox[i++] = 8;  mbox[i++] = 0;
    mbox[i++] = w;          /* [5]  */
    mbox[i++] = h;          /* [6]  */
    mbox[i++] = 0x00048004; /* SET_VIRT_WH              */
    mbox[i++] = 8;  mbox[i++] = 0;
    mbox[i++] = w;          /* [10] */
    mbox[i++] = h;          /* [11] */
    mbox[i++] = 0x00048009; /* SET_VIRT_OFF             */
    mbox[i++] = 8;  mbox[i++] = 0;
    mbox[i++] = 0;          /* [15] x */
    mbox[i++] = 0;          /* [16] y */
    mbox[i++] = 0x00048005; /* SET_DEPTH                */
    mbox[i++] = 4;  mbox[i++] = 0;
    mbox[i++] = 32;         /* [20] bpp */
    mbox[i++] = 0x00048006; /* SET_PIXEL_ORDER          */
    mbox[i++] = 4;  mbox[i++] = 0;
    mbox[i++] = 1;          /* [24] 1=RGB */
    mbox[i++] = 0x00040001; /* ALLOCATE_BUFFER          */
    mbox[i++] = 8;  mbox[i++] = 0;
    mbox[i++] = 4096;       /* [28] IN:align OUT:gpu_addr */
    mbox[i++] = 0;          /* [29]          OUT:size    */
    mbox[i++] = 0x00040008; /* GET_PITCH                */
    mbox[i++] = 4;  mbox[i++] = 0;
    mbox[i++] = 0;          /* [33]          OUT:pitch   */
    mbox[i++] = 0;          /* [34] END                 */
    mbox[0]   = (uint32_t)(i * 4);

    if (mbox_call(mbox) != 0) {
        debug_print("fb_init: FAILED code=0x%x addr=0x%x sz=%u pitch=%u\n",
                    mbox[1], mbox[28], mbox[29], mbox[33]);
        return -1;
    }

    uint32_t gpu_addr = mbox[28];
    uint32_t pitch    = mbox[33] ? mbox[33] : w * 4;

    if (!gpu_addr) { debug_print("fb_init: null gpu_addr\n"); return -1; }

    fb.base   = (uint32_t *)(uint64_t)(gpu_addr & 0x3FFFFFFFU);
    fb.width  = w;  fb.height = h;
    fb.pitch  = pitch;
    fb.bpp    = 32; fb.size = mbox[29]; fb.valid = 1;

    debug_print("fb_init: OK base=0x%x %ux%u pitch=%u\n",
                (uint32_t)(uint64_t)fb.base, w, h, pitch);

    /* Corner markers — proves pixel writes work */
    fb_fill_rect(0,      0,      32, 32, RGB(255,0,0));
    fb_fill_rect(w-32,   0,      32, 32, RGB(0,255,0));
    fb_fill_rect(0,      h-32,   32, 32, RGB(0,0,255));
    fb_fill_rect(w-32,   h-32,   32, 32, RGB(255,255,255));
    return 0;
}

void fb_set_pixel(int x, int y, pixel_t c) {
    if (!fb.valid||(unsigned)x>=fb.width||(unsigned)y>=fb.height) return;
    ((uint32_t*)((uint8_t*)fb.base + y*fb.pitch))[x] = c;
}
void fb_clear(pixel_t c) {
    if (!fb.valid) return;
    for (uint32_t y=0;y<fb.height;y++) {
        uint32_t *r=(uint32_t*)((uint8_t*)fb.base+y*fb.pitch);
        for (uint32_t x=0;x<fb.width;x++) r[x]=c;
    }
}
void fb_fill_rect(int x,int y,int w,int h,pixel_t c) {
    for (int r=y;r<y+h;r++) for (int col=x;col<x+w;col++) fb_set_pixel(col,r,c);
}
void fb_draw_rect(int x,int y,int w,int h,pixel_t c) {
    fb_fill_rect(x,y,w,1,c); fb_fill_rect(x,y+h-1,w,1,c);
    fb_fill_rect(x,y,1,h,c); fb_fill_rect(x+w-1,y,1,h,c);
}
void fb_draw_line(int x0,int y0,int x1,int y1,pixel_t c) {
    int dx=x1>x0?x1-x0:x0-x1, dy=y1>y0?y1-y0:y0-y1;
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, e=dx-dy;
    for(;;){
        fb_set_pixel(x0,y0,c); if(x0==x1&&y0==y1) break;
        int e2=2*e;
        if(e2>-dy){e-=dy;x0+=sx;} if(e2<dx){e+=dx;y0+=sy;}
    }
}
void fb_draw_circle(int cx,int cy,int r,pixel_t c) {
    int x=0,y=r,d=1-r;
    while(x<=y){
        fb_set_pixel(cx+x,cy+y,c);fb_set_pixel(cx-x,cy+y,c);
        fb_set_pixel(cx+x,cy-y,c);fb_set_pixel(cx-x,cy-y,c);
        fb_set_pixel(cx+y,cy+x,c);fb_set_pixel(cx-y,cy+x,c);
        fb_set_pixel(cx+y,cy-x,c);fb_set_pixel(cx-y,cy-x,c);
        if(d<0)d+=2*x+3;else{d+=2*(x-y)+5;y--;}x++;
    }
}
void fb_draw_char(int x,int y,char ch,pixel_t fg,pixel_t bg) {
    const uint8_t *g=font8x8_basic[(uint8_t)ch<128?(uint8_t)ch:'?'];
    for(int r=0;r<8;r++){uint8_t b=g[r];
        for(int col=0;col<8;col++) fb_set_pixel(x+col,y+r,(b&(0x80>>col))?fg:bg);}
}
void fb_draw_string(int x,int y,const char *s,pixel_t fg,pixel_t bg) {
    for(;*s;s++){if(*s=='\n'){x=0;y+=8;continue;} fb_draw_char(x,y,*s,fg,bg);x+=8;}
}
void fb_draw_string_scaled(int x,int y,const char *s,pixel_t fg,pixel_t bg,int sc) {
    for(;*s;s++){
        if(*s=='\n'){x=0;y+=8*sc;continue;}
        const uint8_t *g=font8x8_basic[(uint8_t)*s<128?(uint8_t)*s:'?'];
        for(int r=0;r<8;r++) for(int col=0;col<8;col++)
            fb_fill_rect(x+col*sc,y+r*sc,sc,sc,(g[r]&(0x80>>col))?fg:bg);
        x+=8*sc;
    }
}

/* ---- Console ---- */
#define CON_MX  4
#define CON_MY  36
#define CON_CW  8
#define CON_CH  10
#define CMAX_C  120
#define CMAX_R  64

static int     cc,cr,ccols,crows;
static pixel_t cfg=COL_WHITE, cbg=COL_BLACK;
static char    cbuf[CMAX_R][CMAX_C];

void con_init(void) {
    if(!fb.valid) return;
    ccols=(fb.width-CON_MX*2)/CON_CW; crows=(fb.height-CON_MY)/CON_CH;
    if(ccols>CMAX_C) ccols=CMAX_C;
    if(crows>CMAX_R) crows=CMAX_R;
    cc=cr=0;
    for(int r=0;r<CMAX_R;r++) for(int c=0;c<CMAX_C;c++) cbuf[r][c]=' ';
}
void con_set_colours(pixel_t fg,pixel_t bg){cfg=fg;cbg=bg;}
void con_clear(void){
    if(!fb.valid)return;
    fb_fill_rect(0,CON_MY,fb.width,fb.height-CON_MY,cbg);
    for(int r=0;r<CMAX_R;r++) for(int c=0;c<CMAX_C;c++) cbuf[r][c]=' ';
    cc=cr=0;
}
static void con_render(int row){
    int y=CON_MY+row*CON_CH;
    for(int c=0;c<ccols;c++) fb_draw_char(CON_MX+c*CON_CW,y,cbuf[row][c],cfg,cbg);
}
static void con_scroll(void){
    for(int r=0;r<crows-1;r++){for(int c=0;c<ccols;c++)cbuf[r][c]=cbuf[r+1][c];con_render(r);}
    for(int c=0;c<ccols;c++) cbuf[crows-1][c]=' ';
    con_render(crows-1);
    cr=crows-1;
}
void con_putc(char ch){
    if(!fb.valid)return;
    if(ch=='\r'){cc=0;return;}
    if(ch=='\n'){cc=0;cr++;if(cr>=crows)con_scroll();return;}
    if(ch=='\t'){int n=(cc+4)&~3;while(cc<n)con_putc(' ');return;}
    cbuf[cr][cc]=ch;
    fb_draw_char(CON_MX+cc*CON_CW,CON_MY+cr*CON_CH,ch,cfg,cbg);
    if(++cc>=ccols){cc=0;cr++;if(cr>=crows)con_scroll();}
}
void con_puts(const char *s){while(*s)con_putc(*s++);}
void con_printf(const char *fmt,...){
    va_list ap; va_start(ap,fmt);
    while(*fmt){
        if(*fmt!='%'){con_putc(*fmt++);continue;}
        fmt++;
        switch(*fmt){
        case 's':{const char*s=va_arg(ap,const char*);con_puts(s?s:"(null)");break;}
        case 'd':{int v=va_arg(ap,int);if(v<0){con_putc('-');v=-v;}
                  if(v==0){con_putc('0');break;}
                  char b[16];int n=0;while(v>0){b[n++]='0'+v%10;v/=10;}
                  for(int k=n-1;k>=0;k--) con_putc(b[k]);
                  break;}
        case 'x':{unsigned v=va_arg(ap,unsigned);const char*h="0123456789abcdef";
                  con_puts("0x");for(int s=28;s>=0;s-=4)con_putc(h[(v>>s)&0xF]);break;}
        case 'c':con_putc((char)va_arg(ap,int));break;
        case '%':con_putc('%');break;
        default:con_putc('%');con_putc(*fmt);break;}
        fmt++;
    }
    va_end(ap);
}

pixel_t fb_get_pixel(int x, int y) {
    if (!fb.valid||(unsigned)x>=fb.width||(unsigned)y>=fb.height) return 0;
    return ((uint32_t*)((uint8_t*)fb.base+y*fb.pitch))[x];
}
