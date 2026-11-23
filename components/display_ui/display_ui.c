/*
 * Hardware V2 display UI
 * ST7796S 3.5" / 320x480 native -> 480x320 landscape.
 *
 * Full logical framebuffer lives in PSRAM.
 * A small internal DMA buffer streams it to the panel in strips.
 */

#include "display_ui.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "board_pins.h"
#include "nanocore_map.h"
#include "tempo_engine.h"

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LCD_HOST          SPI2_HOST
#define LCD_W             BOARD_LCD_H_RES
#define LCD_H             BOARD_LCD_V_RES
#define DMA_LINES         24

#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))

#define C_BG        RGB565(8,11,16)
#define C_PANEL     RGB565(22,27,36)
#define C_PANEL2    RGB565(31,38,49)
#define C_WHITE     RGB565(245,247,250)
#define C_MUTED     RGB565(125,137,153)
#define C_GREEN     RGB565(45,220,120)
#define C_RED       RGB565(235,70,88)
#define C_CYAN      RGB565(54,202,225)
#define C_BLUE      RGB565(70,120,245)
#define C_ORANGE    RGB565(250,163,55)
#define C_YELLOW    RGB565(250,214,65)
#define C_PURPLE    RGB565(180,100,245)
#define C_BLACK     RGB565(0,0,0)

static const char *TAG = "NC_UI";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static uint16_t *s_dma;
static SemaphoreHandle_t s_done;
static SemaphoreHandle_t s_mutex;
static bool s_ready;

typedef struct { char c; uint8_t row[7]; } glyph_t;

static const glyph_t FONT[] = {
 {' ',{0,0,0,0,0,0,0}}, {'-',{0,0,0,31,0,0,0}},
 {'.',{0,0,0,0,0,12,12}}, {':',{0,4,4,0,4,4,0}},
 {'/',{1,2,4,8,16,0,0}}, {'?',{14,17,1,2,4,0,4}},
 {'!',{4,4,4,4,4,0,4}}, {'_',{0,0,0,0,0,0,31}},
 {'+',{0,4,4,31,4,4,0}}, {'=',{0,31,0,31,0,0,0}},
 {'@',{14,17,23,21,23,16,14}}, {'#',{10,31,10,10,31,10,0}},
 {'&',{12,18,20,8,21,18,13}}, {'(',{2,4,8,8,8,4,2}},
 {')',{8,4,2,2,2,4,8}}, {'[',{14,8,8,8,8,8,14}},
 {']',{14,2,2,2,2,2,14}}, {'*',{0,21,14,31,14,21,0}},
 {',',{0,0,0,0,0,4,8}},
 {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
 {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
 {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
 {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
 {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
 {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
 {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
 {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
 {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
 {'I',{14,4,4,4,4,4,14}}, {'J',{7,2,2,2,18,18,12}},
 {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
 {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
 {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
 {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
 {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
 {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
 {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
 {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
 {'a',{0,0,14,1,15,17,15}}, {'b',{16,16,22,25,17,17,30}},
 {'c',{0,0,14,17,16,17,14}}, {'d',{1,1,13,19,17,17,15}},
 {'e',{0,0,14,17,31,16,14}}, {'f',{6,9,8,28,8,8,8}},
 {'g',{0,0,15,17,15,1,14}}, {'h',{16,16,22,25,17,17,17}},
 {'i',{4,0,12,4,4,4,14}}, {'j',{2,0,6,2,2,18,12}},
 {'k',{16,16,18,20,24,20,18}}, {'l',{12,4,4,4,4,4,14}},
 {'m',{0,0,26,21,21,17,17}}, {'n',{0,0,22,25,17,17,17}},
 {'o',{0,0,14,17,17,17,14}}, {'p',{0,0,30,17,30,16,16}},
 {'q',{0,0,15,17,15,1,1}}, {'r',{0,0,22,25,16,16,16}},
 {'s',{0,0,15,16,14,1,30}}, {'t',{8,8,28,8,8,9,6}},
 {'u',{0,0,17,17,17,19,13}}, {'v',{0,0,17,17,17,10,4}},
 {'w',{0,0,17,17,21,21,10}}, {'x',{0,0,17,10,4,10,17}},
 {'y',{0,0,17,17,15,1,14}}, {'z',{0,0,31,2,4,8,31}},
};

static bool tx_done(esp_lcd_panel_io_handle_t io,
                    esp_lcd_panel_io_event_data_t *edata,
                    void *ctx)
{
    (void)io; (void)edata; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

static const uint8_t *glyph(char c)
{
    for (size_t i=0;i<sizeof(FONT)/sizeof(FONT[0]);++i)
        if (FONT[i].c == c) return FONT[i].row;
    for (size_t i=0;i<sizeof(FONT)/sizeof(FONT[0]);++i)
        if (FONT[i].c == '?') return FONT[i].row;
    return NULL;
}

static void fill_rect(int x,int y,int w,int h,uint16_t c)
{
    if (!s_fb || w<=0 || h<=0) return;
    if (x<0) { w+=x; x=0; }
    if (y<0) { h+=y; y=0; }
    if (x+w>LCD_W) w=LCD_W-x;
    if (y+h>LCD_H) h=LCD_H-y;
    if (w<=0 || h<=0) return;

    for (int yy=y;yy<y+h;++yy) {
        uint16_t *row=&s_fb[yy*LCD_W+x];
        for (int xx=0;xx<w;++xx) row[xx]=c;
    }
}

static void rect(int x,int y,int w,int h,uint16_t c,int t)
{
    fill_rect(x,y,w,t,c);
    fill_rect(x,y+h-t,w,t,c);
    fill_rect(x,y,t,h,c);
    fill_rect(x+w-t,y,t,h,c);
}

static void draw_char(int x,int y,char ch,uint16_t c,int s)
{
    const uint8_t *r=glyph(ch);
    if (!r) return;
    for(int yy=0;yy<7;++yy)
        for(int xx=0;xx<5;++xx)
            if(r[yy]&(1u<<(4-xx)))
                fill_rect(x+xx*s,y+yy*s,s,s,c);
}

static int text_w(const char *s,int scale)
{
    return (!s||!*s)?0:(int)strlen(s)*(6*scale)-scale;
}

static void text(int x,int y,const char *s,uint16_t c,int scale)
{
    if(!s)return;
    while(*s){draw_char(x,y,*s++,c,scale);x+=6*scale;}
}

static void text_center(int cx,int y,const char *s,uint16_t c,int scale)
{
    text(cx-text_w(s,scale)/2,y,s,c,scale);
}

static const char *mode_name(controller_mode_t m)
{
    switch(m){
    case CTRL_MODE_STOMP:return "STOMP";
    case CTRL_MODE_SCENE:return "SCENE";
    default:return "PRESET";
    }
}

static uint16_t swap16(uint16_t v){return (uint16_t)((v<<8)|(v>>8));}

static esp_err_t flush_screen(void)
{
    while(xSemaphoreTake(s_done,0)==pdTRUE){}

    for(int y=0;y<LCD_H;y+=DMA_LINES){
        int lines=(y+DMA_LINES<=LCD_H)?DMA_LINES:(LCD_H-y);
        size_t n=(size_t)LCD_W*lines;

        for(size_t i=0;i<n;++i)
            s_dma[i]=swap16(s_fb[(size_t)y*LCD_W+i]);

        esp_err_t rc=esp_lcd_panel_draw_bitmap(
            s_panel,0,y,LCD_W,y+lines,s_dma);
        if(rc!=ESP_OK)return rc;

        if(xSemaphoreTake(s_done,pdMS_TO_TICKS(300))!=pdTRUE)
            return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void header(const display_ui_state_t *s)
{
    fill_rect(0,0,LCD_W,42,C_PANEL2);
    text(14,12,"NANOCORE MIDI",C_WHITE,2);

    char buf[64];
    snprintf(buf,sizeof(buf),"P%02u  %s",
             (unsigned)(s->preset_index+1),mode_name(s->mode));
    text(225,14,buf,C_CYAN,1);

    uint16_t link=s->midi_ready?C_GREEN:(s->ble_connected?C_YELLOW:C_RED);
    fill_rect(430,13,12,12,link);
    text(448,14,s->midi_ready?"M":"-",link,1);

    if(s->dirty) text(460,14,"*",C_ORANGE,1);
}

static void footer(const display_ui_state_t *s)
{
    fill_rect(0,285,LCD_W,35,C_PANEL2);
    char buf[96];
    snprintf(buf,sizeof(buf),"BPM %u  %s%s",
             s->bpm,
             tempo_engine_subdivision_name(s->subdivision),
             s->tap_capture?"  TAP":"");
    text(12,296,buf,s->tap_capture?C_BLUE:C_MUTED,1);
    text(300,296,"ENC1 SELECT  ENC2 BACK",C_MUTED,1);
}

static void performance(const display_ui_state_t *s)
{
    const char *pname=(s->preset&&s->preset->name[0])?s->preset->name:"PRESET";
    text_center(LCD_W/2,54,pname,C_WHITE,2);

    if(s->mode==CTRL_MODE_SCENE && s->preset){
        const char *sn=s->preset->scene[s->scene_index].name;
        char sb[48];
        snprintf(sb,sizeof(sb),"%c: %s",(char)('A'+s->scene_index),sn);
        text_center(LCD_W/2,78,sb,C_ORANGE,1);
    }

    const int x0=18,y0=108,w=102,h=64,gx=12,gy=12;
    for(int i=0;i<BLOCK_COUNT;++i){
        int row=i/4,col=i%4;
        int x=x0+col*(w+gx), y=y0+row*(h+gy);
        bool sel=(i==(int)s->selected_block);
        fill_rect(x,y,w,h,sel?C_PANEL2:C_PANEL);
        rect(x,y,w,h,sel?C_CYAN:C_MUTED,sel?3:1);
        text_center(x+w/2,y+20,nanocore_block_name((controller_block_t)i),
                    sel?C_WHITE:C_MUTED,2);
    }
}

static void block_editor(const display_ui_state_t *s)
{
    char title[64];
    snprintf(title,sizeof(title),"%s EDIT",
             nanocore_block_name(s->selected_block));
    text(20,58,title,C_WHITE,2);

    size_t count=nanocore_parameter_count(s->selected_block);
    const size_t visible=8;
    size_t first=0;
    if(s->selected_parameter>=visible)
        first=s->selected_parameter-(visible-1);
    if(first+visible>count && count>visible)
        first=count-visible;

    const int start_y=98;
    for(size_t row=0;row<visible && first+row<count;++row){
        size_t i=first+row;
        const nc_parameter_t *p=nanocore_parameter(s->selected_block,i);
        int y=start_y+(int)row*22;
        bool sel=i==s->selected_parameter;
        if(sel) fill_rect(16,y-3,260,20,C_PANEL2);
        text(28,y,p?p->label:"?",sel?C_CYAN:C_MUTED,1);
    }

    const nc_parameter_t *p=nanocore_parameter(
        s->selected_block,s->selected_parameter);
    if(p){
        char val[72];
        if(p->kind==NC_PARAM_TEMPO_BPM){
            snprintf(val,sizeof(val),"%u BPM",s->bpm);
        }else if(p->kind==NC_PARAM_TEMPO_SUBDIV){
            snprintf(val,sizeof(val),"%s",
                     tempo_engine_subdivision_name(s->subdivision));
        }else if(s->parameter_known){
            if(p->cc>=20 && p->cc<=28)
                snprintf(val,sizeof(val),"%s",s->parameter_value>=64?"ON":"OFF");
            else
                snprintf(val,sizeof(val),"%u",(unsigned)s->parameter_value);
        }else{
            snprintf(val,sizeof(val),"--");
        }
        fill_rect(300,92,150,70,C_PANEL);
        rect(300,92,150,70,s->parameter_editing?C_YELLOW:C_PURPLE,2);
        text_center(375,105,p->label,C_WHITE,1);
        text_center(375,130,val,s->parameter_editing?C_YELLOW:C_WHITE,2);
    }
    text(300,180,s->parameter_editing?"ENC2 CHANGE":"ENC1 PARAM",C_MUTED,1);
    text(300,200,"ENC1 CLICK EDIT",C_MUTED,1);
    text(300,220,"ENC2 CLICK BACK",C_MUTED,1);
}

static void name_editor_screen(const display_ui_state_t *s)
{
    const name_editor_t *ed=s->name_editor;
    if(!ed)return;

    text_center(LCD_W/2,52,"SAVE NAME",C_WHITE,2);

    fill_rect(18,82,444,48,C_PANEL);
    rect(18,82,444,48,C_CYAN,2);
    text(28,98,ed->text,C_WHITE,2);

    // Cursor approximation in 12 px cells at scale 2.
    int cx=28+(int)ed->cursor*12;
    fill_rect(cx,121,10,3,C_YELLOW);

    for(uint8_t row=0;row<name_editor_row_count();++row){
        const char *chars=name_editor_row_text(ed,row);
        int y=155+row*38;
        if(row==ed->row) fill_rect(10,y-8,460,31,C_PANEL2);

        int x=18;
        for(size_t i=0;i<strlen(chars);++i){
            char one[2]={chars[i],0};
            bool sel=(row==ed->row && i==ed->col);
            if(sel){
                fill_rect(x-3,y-4,17,23,C_CYAN);
                text(x,y,one,C_BLACK,1);
            }else{
                text(x,y,one,C_WHITE,1);
            }
            x+=17;
        }
    }

    text(18,274,ed->uppercase?"UPPER":"lower",C_ORANGE,1);
    text(88,274,"ENC2 CLICK CASE",C_MUTED,1);
    text(250,274,"B1 DELETE  B2 CANCEL",C_MUTED,1);
}

static void tuner_screen(const display_ui_state_t *s)
{
    (void)s;
    text_center(LCD_W/2,85,"TUNER",C_GREEN,4);
    text_center(LCD_W/2,145,"NANOCORE TUNER ON",C_WHITE,2);
    text_center(LCD_W/2,195,"MODE + TOP RIGHT TO EXIT",C_MUTED,1);
}

esp_err_t display_ui_init(void)
{
    if(s_ready)return ESP_OK;

    s_done=xSemaphoreCreateBinary();
    s_mutex=xSemaphoreCreateMutex();
    if(!s_done||!s_mutex)return ESP_ERR_NO_MEM;

    size_t fb_bytes=(size_t)LCD_W*LCD_H*sizeof(uint16_t);
    s_fb=heap_caps_malloc(fb_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_dma=heap_caps_malloc((size_t)LCD_W*DMA_LINES*sizeof(uint16_t),
                           MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!s_fb||!s_dma){
        ESP_LOGE(TAG,"Framebuffer allocation failed fb=%p dma=%p",s_fb,s_dma);
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus={
        .sclk_io_num=BOARD_LCD_SCK,
        .mosi_io_num=BOARD_LCD_MOSI,
        .miso_io_num=BOARD_LCD_MISO,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz=LCD_W*DMA_LINES*sizeof(uint16_t),
    };
    esp_err_t rc=spi_bus_initialize(LCD_HOST,&bus,SPI_DMA_CH_AUTO);
    if(rc!=ESP_OK&&rc!=ESP_ERR_INVALID_STATE)return rc;

    esp_lcd_panel_io_handle_t io=NULL;
    esp_lcd_panel_io_spi_config_t io_cfg={
        .cs_gpio_num=BOARD_LCD_CS,
        .dc_gpio_num=BOARD_LCD_DC,
        .spi_mode=0,
        .pclk_hz=BOARD_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth=1,
        .on_color_trans_done=tx_done,
        .lcd_cmd_bits=8,
        .lcd_param_bits=8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST,&io_cfg,&io));

    esp_lcd_panel_dev_config_t panel_cfg={
        .reset_gpio_num=BOARD_LCD_RST,
        .rgb_ele_order=BOARD_LCD_BGR
            ? LCD_RGB_ELEMENT_ORDER_BGR
            : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel=16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io,&panel_cfg,&s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    if(BOARD_LCD_SWAP_XY)
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(
        s_panel,BOARD_LCD_MIRROR_X,BOARD_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(
        s_panel,BOARD_LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel,true));

    memset(s_fb,0,fb_bytes);
    board_set_backlight(true);

    s_ready=true;
    ESP_LOGI(TAG,"ST7796S ready 480x320 landscape, framebuffer in PSRAM");
    return ESP_OK;
}

bool display_ui_is_ready(void){return s_ready;}

void display_ui_render(const display_ui_state_t *s)
{
    if(!s_ready||!s)return;
    if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(500))!=pdTRUE)return;

    fill_rect(0,0,LCD_W,LCD_H,C_BG);
    header(s);

    switch(s->screen){
    case UI_SCREEN_BLOCK_EDITOR:block_editor(s);break;
    case UI_SCREEN_NAME_EDITOR:name_editor_screen(s);break;
    case UI_SCREEN_TUNER:tuner_screen(s);break;
    default:performance(s);break;
    }

    if(s->screen!=UI_SCREEN_NAME_EDITOR && s->screen!=UI_SCREEN_TUNER)
        footer(s);

    esp_err_t rc=flush_screen();
    if(rc!=ESP_OK)ESP_LOGW(TAG,"LCD flush: %s",esp_err_to_name(rc));

    xSemaphoreGive(s_mutex);
}
