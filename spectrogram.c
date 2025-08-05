/*
    Stereo Spectrogram plugin for the DeaDBeeF audio player

    Copyright (C) 2014 Christian Boxdörfer <christian.boxdoerfer@posteo.de>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <fftw3.h>

#include <deadbeef/deadbeef.h>
#include <deadbeef/gtkui_api.h>

#include "fastftoi.h"

#define GRADIENT_TABLE_SIZE 2048
#define FFT_SIZE 8192
#define MAX_HEIGHT 4096

#define     CONFSTR_SP_LOG_SCALE              "spectrogram.log_scale"
#define     CONFSTR_SP_REFRESH_INTERVAL       "spectrogram.refresh_interval"
#define     CONFSTR_SP_DB_RANGE               "spectrogram.db_range"
#define     CONFSTR_SP_NUM_COLORS             "spectrogram.num_colors"
#define     CONFSTR_SP_COLOR_GRADIENT_00      "spectrogram.color.gradient_00"
#define     CONFSTR_SP_COLOR_GRADIENT_01      "spectrogram.color.gradient_01"
#define     CONFSTR_SP_COLOR_GRADIENT_02      "spectrogram.color.gradient_02"
#define     CONFSTR_SP_COLOR_GRADIENT_03      "spectrogram.color.gradient_03"
#define     CONFSTR_SP_COLOR_GRADIENT_04      "spectrogram.color.gradient_04"
#define     CONFSTR_SP_COLOR_GRADIENT_05      "spectrogram.color.gradient_05"
#define     CONFSTR_SP_COLOR_GRADIENT_06      "spectrogram.color.gradient_06"

/* Global variables */
static DB_misc_t            plugin;
static DB_functions_t *     deadbeef = NULL;
static ddb_gtkui_t *        gtkui_plugin = NULL;

typedef struct {
    ddb_gtkui_widget_t base;
    GtkWidget *drawarea;
    GtkWidget *popup;
    GtkWidget *popup_item;
    guint drawtimer;
    // Stereo channel data
    double *data_left;
    double *data_right;
    double window[FFT_SIZE];
    // Stereo FFT inputs
    double *in_left;
    double *in_right;
    // Stereo FFT outputs
    fftw_complex *out_complex_left;
    fftw_complex *out_complex_right;
    // Stereo FFT plans
    fftw_plan p_r2c_left;
    fftw_plan p_r2c_right;
    uint32_t colors[GRADIENT_TABLE_SIZE];
    // Stereo sample buffers
    double *samples_left;
    double *samples_right;
    int *log_index;
    float samplerate;
    int height;
    int low_res_end;
    int resized;
    int buffered;
    intptr_t mutex;
    cairo_surface_t *surf;
} w_spectrogram_t;


static int CONFIG_LOG_SCALE = 1;
static int CONFIG_DB_RANGE = 70;
static int CONFIG_NUM_COLORS = 7;
static int CONFIG_REFRESH_INTERVAL = 25;
static GdkColor CONFIG_GRADIENT_COLORS[7];

static void
save_config (void)
{
    deadbeef->conf_set_int (CONFSTR_SP_LOG_SCALE, CONFIG_LOG_SCALE);
    deadbeef->conf_set_int (CONFSTR_SP_DB_RANGE, CONFIG_DB_RANGE);
    deadbeef->conf_set_int (CONFSTR_SP_NUM_COLORS, CONFIG_NUM_COLORS);
    deadbeef->conf_set_int (CONFSTR_SP_REFRESH_INTERVAL, CONFIG_REFRESH_INTERVAL);
    char color[100];
    snprintf (color, sizeof (color), "%d %d %d", CONFIG_GRADIENT_COLORS[0].red, CONFIG_GRADIENT_COLORS[0].green, CONFIG_GRADIENT_COLORS[0].blue);
    deadbeef->conf_set_str (CONFSTR_SP_COLOR_GRADIENT_00, color);
    snprintf (color, sizeof (color), "%d %d %d", CONFIG_GRADIENT_COLORS[1].red, CONFIG_GRADIENT_COLORS[1].green, CONFIG_GRADIENT_COLORS[1].blue);
    deadbeef->conf_set_str (CONFSTR_SP_COLOR_GRADIENT_01, color);
    snprintf (color, sizeof (color), "%d %d %d", CONFIG_GRADIENT_COLORS[2].red, CONFIG_GRADIENT_COLORS[2].green, CONFIG_GRADIENT_COLORS[2].blue);
    deadbeef->conf_set_str (CONFSTR_SP_COLOR_GRADIENT_02, color);
    snprintf (color, sizeof (color), "%d %d %d", CONFIG_GRADIENT_COLORS[3].red, CONFIG_GRADIENT_COLORS[3].green, CONFIG_GRADIENT_COLORS[3].blue);
    deadbeef->conf_set_str (CONFSTR_SP_COLOR_GRADIENT_03, color);
    snprintf (color, sizeof (color), "%d %d %d", CONFIG_GRADIENT_COLORS[4].red, CONFIG_GRADIENT_COLORS[4].green, CONFIG_GRADIENT_COLORS[4].blue);
    deadbeef->conf_set_str (CONFSTR_SP_COLOR_GRADIENT_04, color);
    snprintf (color, sizeof (color), "%d %d %d", CONFIG_GRADIENT_COLORS[5].red, CONFIG_GRADIENT_COLORS[5].green, CONFIG_GRADIENT_COLORS[5].blue);
    deadbeef->conf_set_str (CONFSTR_SP_COLOR_GRADIENT_05, color);
    snprintf (color, sizeof (color), "%d %d %d", CONFIG_GRADIENT_COLORS[6].red, CONFIG_GRADIENT_COLORS[6].green, CONFIG_GRADIENT_COLORS[6].blue);
    deadbeef->conf_set_str (CONFSTR_SP_COLOR_GRADIENT_06, color);
}

static void
load_config (void)
{
    deadbeef->conf_lock ();
    CONFIG_LOG_SCALE = deadbeef->conf_get_int (CONFSTR_SP_LOG_SCALE,                1);
    CONFIG_DB_RANGE = deadbeef->conf_get_int (CONFSTR_SP_DB_RANGE,                 70);
    CONFIG_NUM_COLORS = deadbeef->conf_get_int (CONFSTR_SP_NUM_COLORS,              7);
    CONFIG_REFRESH_INTERVAL = deadbeef->conf_get_int (CONFSTR_SP_REFRESH_INTERVAL, 25);
    const char *color;
    color = deadbeef->conf_get_str_fast (CONFSTR_SP_COLOR_GRADIENT_00,        "65535 0 0");
    sscanf (color, "%hd %hd %hd", &(CONFIG_GRADIENT_COLORS[0].red), &(CONFIG_GRADIENT_COLORS[0].green), &(CONFIG_GRADIENT_COLORS[0].blue));
    color = deadbeef->conf_get_str_fast (CONFSTR_SP_COLOR_GRADIENT_01,      "65535 32896 0");
    sscanf (color, "%hd %hd %hd", &(CONFIG_GRADIENT_COLORS[1].red), &(CONFIG_GRADIENT_COLORS[1].green), &(CONFIG_GRADIENT_COLORS[1].blue));
    color = deadbeef->conf_get_str_fast (CONFSTR_SP_COLOR_GRADIENT_02,      "65535 65535 0");
    sscanf (color, "%hd %hd %hd", &(CONFIG_GRADIENT_COLORS[2].red), &(CONFIG_GRADIENT_COLORS[2].green), &(CONFIG_GRADIENT_COLORS[2].blue));
    color = deadbeef->conf_get_str_fast (CONFSTR_SP_COLOR_GRADIENT_03,    "32896 65535 30840");
    sscanf (color, "%hd %hd %hd", &(CONFIG_GRADIENT_COLORS[3].red), &(CONFIG_GRADIENT_COLORS[3].green), &(CONFIG_GRADIENT_COLORS[3].blue));
    color = deadbeef->conf_get_str_fast (CONFSTR_SP_COLOR_GRADIENT_04,      "0 38036 41120");
    sscanf (color, "%hd %hd %hd", &(CONFIG_GRADIENT_COLORS[4].red), &(CONFIG_GRADIENT_COLORS[4].green), &(CONFIG_GRADIENT_COLORS[4].blue));
    color = deadbeef->conf_get_str_fast (CONFSTR_SP_COLOR_GRADIENT_05,       "0 8224 25700");
    sscanf (color, "%hd %hd %hd", &(CONFIG_GRADIENT_COLORS[5].red), &(CONFIG_GRADIENT_COLORS[5].green), &(CONFIG_GRADIENT_COLORS[5].blue));
    color = deadbeef->conf_get_str_fast (CONFSTR_SP_COLOR_GRADIENT_06,       "0 0 0");
    sscanf (color, "%hd %hd %hd", &(CONFIG_GRADIENT_COLORS[6].red), &(CONFIG_GRADIENT_COLORS[6].green), &(CONFIG_GRADIENT_COLORS[6].blue));
    deadbeef->conf_unlock ();
}

// Helper function to process FFT for a single channel
static void
process_channel_fft(w_spectrogram_t *w, double *samples, double *in, fftw_complex *out_complex, fftw_plan plan, double *data)
{
    double real, imag;

    for (int i = 0; i < FFT_SIZE; i++) {
        in[i] = samples[i] * w->window[i];
    }
    
    fftw_execute (plan);
    
    for (int i = 0; i < FFT_SIZE/2; i++) {
        real = out_complex[i][0];
        imag = out_complex[i][1];
        data[i] = (real*real + imag*imag);
    }
}

void
do_fft (w_spectrogram_t *w)
{
    if ((!w->samples_left || !w->samples_right) || w->buffered < FFT_SIZE/2) {
        return;
    }
    
    deadbeef->mutex_lock (w->mutex);
    
    // Process left channel
    process_channel_fft(w, w->samples_left, w->in_left, w->out_complex_left, w->p_r2c_left, w->data_left);
    
    // Process right channel
    process_channel_fft(w, w->samples_right, w->in_right, w->out_complex_right, w->p_r2c_right, w->data_right);
    
    deadbeef->mutex_unlock (w->mutex);
}

static inline void
_draw_point (uint8_t *data, int stride, int x0, int y0, uint32_t color) {
    uint32_t *ptr = (uint32_t*)&data[y0*stride+x0*4];
    *ptr = color;
}

/* based on Delphi function by Witold J.Janik */
void
create_gradient_table (gpointer user_data, GdkColor *colors, int num_colors)
{
    w_spectrogram_t *w = user_data;

    num_colors -= 1;

    for (int i = 0; i < GRADIENT_TABLE_SIZE; i++) {
        double position = (double)i/GRADIENT_TABLE_SIZE;
        /* if position > 1 then we have repetition of colors it maybe useful    */
        if (position > 1.0) {
            if (position - ftoi (position) == 0.0) {
                position = 1.0;
            }
            else {
                position = position - ftoi (position);
            }
        }

        double m= num_colors * position;
        int n=(int)m; // integer of m
        double f=m-n;  // fraction of m

        w->colors[i] = 0xFF000000;
        float scale = 255/65535.f;
        if (num_colors == 0) {
            w->colors[i] = ((uint32_t)(colors[0].red*scale) & 0xFF) << 16 |
                ((uint32_t)(colors[0].green*scale) & 0xFF) << 8 |
                ((uint32_t)(colors[0].blue*scale) & 0xFF) << 0;
        }
        else if (n < num_colors) {
            w->colors[i] = ((uint32_t)((colors[n].red*scale) + f * ((colors[n+1].red*scale)-(colors[n].red*scale))) & 0xFF) << 16 |
                ((uint32_t)((colors[n].green*scale) + f * ((colors[n+1].green*scale)-(colors[n].green*scale))) & 0xFF) << 8 |
                ((uint32_t)((colors[n].blue*scale) + f * ((colors[n+1].blue*scale)-(colors[n].blue*scale))) & 0xFF) << 0;
        }
        else if (n == num_colors) {
            w->colors[i] = ((uint32_t)(colors[n].red*scale) & 0xFF) << 16 |
                ((uint32_t)(colors[n].green*scale) & 0xFF) << 8 |
                ((uint32_t)(colors[n].blue*scale) & 0xFF) << 0;
        }
        else {
            w->colors[i] = 0xFFFFFFFF;
        }
    }
}

static int
on_config_changed (gpointer user_data, uintptr_t ctx)
{
    create_gradient_table (user_data, CONFIG_GRADIENT_COLORS, CONFIG_NUM_COLORS);
    load_config ();
    return 0;
}

#if !GTK_CHECK_VERSION(2,12,0)
#define gtk_widget_get_window(widget) ((widget)->window)
#define gtk_dialog_get_content_area(dialog) (dialog->vbox)
#define gtk_dialog_get_action_area(dialog) (dialog->action_area)
#endif

#if !GTK_CHECK_VERSION(2,18,0)
void
gtk_widget_get_allocation (GtkWidget *widget, GtkAllocation *allocation) {
    (allocation)->x = widget->allocation.x;
    (allocation)->y = widget->allocation.y;
    (allocation)->width = widget->allocation.width;
    (allocation)->height = widget->allocation.height;
}
#define gtk_widget_set_can_default(widget, candefault) {if (candefault) GTK_WIDGET_SET_FLAGS (widget, GTK_CAN_DEFAULT); else GTK_WIDGET_UNSET_FLAGS(widget, GTK_CAN_DEFAULT);}
#endif

static void
on_button_config (GtkMenuItem *menuitem, gpointer user_data)
{
    GtkWidget *spectrogram_properties;
    GtkWidget *config_dialog;
    GtkWidget *vbox01;
    GtkWidget *vbox02;
    GtkWidget *hbox01;
    //GtkWidget *hbox02;
    GtkWidget *hbox03;
    GtkWidget *color_label;
    GtkWidget *color_frame;
    GtkWidget *color_gradient_00;
    GtkWidget *color_gradient_01;
    GtkWidget *color_gradient_02;
    GtkWidget *color_gradient_03;
    GtkWidget *color_gradient_04;
    GtkWidget *color_gradient_05;
    GtkWidget *color_gradient_06;
    GtkWidget *num_colors_label;
    GtkWidget *num_colors;
    GtkWidget *log_scale;
    GtkWidget *db_range_label0;
    GtkWidget *db_range;
    GtkWidget *dialog_action_area13;
    GtkWidget *applybutton1;
    GtkWidget *cancelbutton1;
    GtkWidget *okbutton1;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    spectrogram_properties = gtk_dialog_new ();
    gtk_window_set_title (GTK_WINDOW (spectrogram_properties), "Spectrogram Properties");
    gtk_window_set_type_hint (GTK_WINDOW (spectrogram_properties), GDK_WINDOW_TYPE_HINT_DIALOG);

    config_dialog = gtk_dialog_get_content_area (GTK_DIALOG (spectrogram_properties));
    gtk_widget_show (config_dialog);

    hbox01 = gtk_hbox_new (FALSE, 8);
    gtk_widget_show (hbox01);
    gtk_box_pack_start (GTK_BOX (config_dialog), hbox01, FALSE, FALSE, 0);
    gtk_container_set_border_width (GTK_CONTAINER (hbox01), 12);

    color_label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (color_label),"<b>Colors</b>");
    gtk_widget_show (color_label);

    color_frame = gtk_frame_new ("Colors");
    gtk_frame_set_label_widget ((GtkFrame *)color_frame, color_label);
    gtk_frame_set_shadow_type ((GtkFrame *)color_frame, GTK_SHADOW_IN);
    gtk_widget_show (color_frame);
    gtk_box_pack_start (GTK_BOX (hbox01), color_frame, TRUE, FALSE, 0);

    vbox02 = gtk_vbox_new (FALSE, 8);
    gtk_widget_show (vbox02);
    gtk_container_add (GTK_CONTAINER (color_frame), vbox02);
    gtk_container_set_border_width (GTK_CONTAINER (vbox02), 12);

    num_colors_label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (num_colors_label),"Number of colors:");
    gtk_widget_show (num_colors_label);
    gtk_box_pack_start (GTK_BOX (vbox02), num_colors_label, FALSE, FALSE, 0);

    num_colors = gtk_spin_button_new_with_range (1,7,1);
    gtk_widget_show (num_colors);
    gtk_box_pack_start (GTK_BOX (vbox02), num_colors, FALSE, FALSE, 0);

    color_gradient_00 = gtk_color_button_new ();
    gtk_color_button_set_use_alpha ((GtkColorButton *)color_gradient_00, TRUE);
    gtk_widget_show (color_gradient_00);
    gtk_box_pack_start (GTK_BOX (vbox02), color_gradient_00, TRUE, FALSE, 0);
    gtk_widget_set_size_request (color_gradient_00, -1, 30);

    color_gradient_01 = gtk_color_button_new ();
    gtk_color_button_set_use_alpha ((GtkColorButton *)color_gradient_01, TRUE);
    gtk_widget_show (color_gradient_01);
    gtk_box_pack_start (GTK_BOX (vbox02), color_gradient_01, TRUE, FALSE, 0);
    gtk_widget_set_size_request (color_gradient_01, -1, 30);

    color_gradient_02 = gtk_color_button_new ();
    gtk_color_button_set_use_alpha ((GtkColorButton *)color_gradient_02, TRUE);
    gtk_widget_show (color_gradient_02);
    gtk_box_pack_start (GTK_BOX (vbox02), color_gradient_02, TRUE, FALSE, 0);
    gtk_widget_set_size_request (color_gradient_02, -1, 30);

    color_gradient_03 = gtk_color_button_new ();
    gtk_color_button_set_use_alpha ((GtkColorButton *)color_gradient_03, TRUE);
    gtk_widget_show (color_gradient_03);
    gtk_box_pack_start (GTK_BOX (vbox02), color_gradient_03, TRUE, FALSE, 0);
    gtk_widget_set_size_request (color_gradient_03, -1, 30);

    color_gradient_04 = gtk_color_button_new ();
    gtk_color_button_set_use_alpha ((GtkColorButton *)color_gradient_04, TRUE);
    gtk_widget_show (color_gradient_04);
    gtk_box_pack_start (GTK_BOX (vbox02), color_gradient_04, TRUE, FALSE, 0);
    gtk_widget_set_size_request (color_gradient_04, -1, 30);

    color_gradient_05 = gtk_color_button_new ();
    gtk_color_button_set_use_alpha ((GtkColorButton *)color_gradient_05, TRUE);
    gtk_widget_show (color_gradient_05);
    gtk_box_pack_start (GTK_BOX (vbox02), color_gradient_05, TRUE, FALSE, 0);
    gtk_widget_set_size_request (color_gradient_05, -1, 30);

    color_gradient_06 = gtk_color_button_new ();
    gtk_color_button_set_use_alpha ((GtkColorButton *)color_gradient_06, TRUE);
    gtk_widget_show (color_gradient_06);
    gtk_box_pack_start (GTK_BOX (vbox02), color_gradient_06, TRUE, FALSE, 0);
    gtk_widget_set_size_request (color_gradient_06, -1, 30);

    vbox01 = gtk_vbox_new (FALSE, 8);
    gtk_widget_show (vbox01);
    gtk_box_pack_start (GTK_BOX (hbox01), vbox01, FALSE, FALSE, 0);
    gtk_container_set_border_width (GTK_CONTAINER (vbox01), 12);

    //hbox02 = gtk_hbox_new (FALSE, 8);
    //gtk_widget_show (hbox02);
    //gtk_box_pack_start (GTK_BOX (vbox01), hbox02, FALSE, FALSE, 0);
    //gtk_container_set_border_width (GTK_CONTAINER (hbox01), 12);

    hbox03 = gtk_hbox_new (FALSE, 8);
    gtk_widget_show (hbox03);
    gtk_box_pack_start (GTK_BOX (vbox01), hbox03, FALSE, FALSE, 0);

    db_range_label0 = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (db_range_label0),"dB range:");
    gtk_widget_show (db_range_label0);
    gtk_box_pack_start (GTK_BOX (hbox03), db_range_label0, FALSE, TRUE, 0);

    db_range = gtk_spin_button_new_with_range (50,120,10);
    gtk_widget_show (db_range);
    gtk_box_pack_start (GTK_BOX (hbox03), db_range, TRUE, TRUE, 0);

    log_scale = gtk_check_button_new_with_label ("Log scale");
    gtk_widget_show (log_scale);
    gtk_box_pack_start (GTK_BOX (vbox01), log_scale, FALSE, FALSE, 0);

    dialog_action_area13 = gtk_dialog_get_action_area (GTK_DIALOG (spectrogram_properties));
    gtk_widget_show (dialog_action_area13);
    gtk_button_box_set_layout (GTK_BUTTON_BOX (dialog_action_area13), GTK_BUTTONBOX_END);

    applybutton1 = gtk_button_new_from_stock ("gtk-apply");
    gtk_widget_show (applybutton1);
    gtk_dialog_add_action_widget (GTK_DIALOG (spectrogram_properties), applybutton1, GTK_RESPONSE_APPLY);
    gtk_widget_set_can_default (applybutton1, TRUE);

    cancelbutton1 = gtk_button_new_from_stock ("gtk-cancel");
    gtk_widget_show (cancelbutton1);
    gtk_dialog_add_action_widget (GTK_DIALOG (spectrogram_properties), cancelbutton1, GTK_RESPONSE_CANCEL);
    gtk_widget_set_can_default (cancelbutton1, TRUE);

    okbutton1 = gtk_button_new_from_stock ("gtk-ok");
    gtk_widget_show (okbutton1);
    gtk_dialog_add_action_widget (GTK_DIALOG (spectrogram_properties), okbutton1, GTK_RESPONSE_OK);
    gtk_widget_set_can_default (okbutton1, TRUE);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (log_scale), CONFIG_LOG_SCALE);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (num_colors), CONFIG_NUM_COLORS);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (db_range), CONFIG_DB_RANGE);
    gtk_color_button_set_color (GTK_COLOR_BUTTON (color_gradient_00), &(CONFIG_GRADIENT_COLORS[0]));
    gtk_color_button_set_color (GTK_COLOR_BUTTON (color_gradient_01), &(CONFIG_GRADIENT_COLORS[1]));
    gtk_color_button_set_color (GTK_COLOR_BUTTON (color_gradient_02), &(CONFIG_GRADIENT_COLORS[2]));
    gtk_color_button_set_color (GTK_COLOR_BUTTON (color_gradient_03), &(CONFIG_GRADIENT_COLORS[3]));
    gtk_color_button_set_color (GTK_COLOR_BUTTON (color_gradient_04), &(CONFIG_GRADIENT_COLORS[4]));
    gtk_color_button_set_color (GTK_COLOR_BUTTON (color_gradient_05), &(CONFIG_GRADIENT_COLORS[5]));
    gtk_color_button_set_color (GTK_COLOR_BUTTON (color_gradient_06), &(CONFIG_GRADIENT_COLORS[6]));

    switch (CONFIG_NUM_COLORS) {
        case 1:
            gtk_widget_hide (color_gradient_01);
            gtk_widget_hide (color_gradient_02);
            gtk_widget_hide (color_gradient_03);
            gtk_widget_hide (color_gradient_04);
            gtk_widget_hide (color_gradient_05);
            gtk_widget_hide (color_gradient_06);
            break;
        case 2:
            gtk_widget_show (color_gradient_01);
            gtk_widget_hide (color_gradient_02);
            gtk_widget_hide (color_gradient_03);
            gtk_widget_hide (color_gradient_04);
            gtk_widget_hide (color_gradient_05);
            gtk_widget_hide (color_gradient_06);
            break;
        case 3:
            gtk_widget_show (color_gradient_01);
            gtk_widget_show (color_gradient_02);
            gtk_widget_hide (color_gradient_03);
            gtk_widget_hide (color_gradient_04);
            gtk_widget_hide (color_gradient_05);
            gtk_widget_hide (color_gradient_06);
            break;
        case 4:
            gtk_widget_show (color_gradient_01);
            gtk_widget_show (color_gradient_02);
            gtk_widget_show (color_gradient_03);
            gtk_widget_hide (color_gradient_04);
            gtk_widget_hide (color_gradient_05);
            gtk_widget_hide (color_gradient_06);
            break;
        case 5:
            gtk_widget_show (color_gradient_01);
            gtk_widget_show (color_gradient_02);
            gtk_widget_show (color_gradient_03);
            gtk_widget_show (color_gradient_04);
            gtk_widget_hide (color_gradient_05);
            gtk_widget_hide (color_gradient_06);
            break;
        case 6:
            gtk_widget_show (color_gradient_01);
            gtk_widget_show (color_gradient_02);
            gtk_widget_show (color_gradient_03);
            gtk_widget_show (color_gradient_04);
            gtk_widget_show (color_gradient_05);
            gtk_widget_hide (color_gradient_06);
            break;
        case 7:
            gtk_widget_show (color_gradient_01);
            gtk_widget_show (color_gradient_02);
            gtk_widget_show (color_gradient_03);
            gtk_widget_show (color_gradient_04);
            gtk_widget_show (color_gradient_05);
            gtk_widget_show (color_gradient_06);
            break;
    }

    for (;;) {
        int response = gtk_dialog_run (GTK_DIALOG (spectrogram_properties));
        if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY) {
            gtk_color_button_get_color (GTK_COLOR_BUTTON (color_gradient_00), &CONFIG_GRADIENT_COLORS[0]);
            gtk_color_button_get_color (GTK_COLOR_BUTTON (color_gradient_01), &CONFIG_GRADIENT_COLORS[1]);
            gtk_color_button_get_color (GTK_COLOR_BUTTON (color_gradient_02), &CONFIG_GRADIENT_COLORS[2]);
            gtk_color_button_get_color (GTK_COLOR_BUTTON (color_gradient_03), &CONFIG_GRADIENT_COLORS[3]);
            gtk_color_button_get_color (GTK_COLOR_BUTTON (color_gradient_04), &CONFIG_GRADIENT_COLORS[4]);
            gtk_color_button_get_color (GTK_COLOR_BUTTON (color_gradient_05), &CONFIG_GRADIENT_COLORS[5]);
            gtk_color_button_get_color (GTK_COLOR_BUTTON (color_gradient_06), &CONFIG_GRADIENT_COLORS[6]);

            CONFIG_LOG_SCALE = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (log_scale));
            CONFIG_DB_RANGE = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (db_range));
            CONFIG_NUM_COLORS = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (num_colors));
            switch (CONFIG_NUM_COLORS) {
                case 1:
                    gtk_widget_hide (color_gradient_01);
                    gtk_widget_hide (color_gradient_02);
                    gtk_widget_hide (color_gradient_03);
                    gtk_widget_hide (color_gradient_04);
                    gtk_widget_hide (color_gradient_05);
                    gtk_widget_hide (color_gradient_06);
                    break;
                case 2:
                    gtk_widget_show (color_gradient_01);
                    gtk_widget_hide (color_gradient_02);
                    gtk_widget_hide (color_gradient_03);
                    gtk_widget_hide (color_gradient_04);
                    gtk_widget_hide (color_gradient_05);
                    gtk_widget_hide (color_gradient_06);
                    break;
                case 3:
                    gtk_widget_show (color_gradient_01);
                    gtk_widget_show (color_gradient_02);
                    gtk_widget_hide (color_gradient_03);
                    gtk_widget_hide (color_gradient_04);
                    gtk_widget_hide (color_gradient_05);
                    gtk_widget_hide (color_gradient_06);
                    break;
                case 4:
                    gtk_widget_show (color_gradient_01);
                    gtk_widget_show (color_gradient_02);
                    gtk_widget_show (color_gradient_03);
                    gtk_widget_hide (color_gradient_04);
                    gtk_widget_hide (color_gradient_05);
                    gtk_widget_hide (color_gradient_06);
                    break;
                case 5:
                    gtk_widget_show (color_gradient_01);
                    gtk_widget_show (color_gradient_02);
                    gtk_widget_show (color_gradient_03);
                    gtk_widget_show (color_gradient_04);
                    gtk_widget_hide (color_gradient_05);
                    gtk_widget_hide (color_gradient_06);
                    break;
                case 6:
                    gtk_widget_show (color_gradient_01);
                    gtk_widget_show (color_gradient_02);
                    gtk_widget_show (color_gradient_03);
                    gtk_widget_show (color_gradient_04);
                    gtk_widget_show (color_gradient_05);
                    gtk_widget_hide (color_gradient_06);
                    break;
                case 7:
                    gtk_widget_show (color_gradient_01);
                    gtk_widget_show (color_gradient_02);
                    gtk_widget_show (color_gradient_03);
                    gtk_widget_show (color_gradient_04);
                    gtk_widget_show (color_gradient_05);
                    gtk_widget_show (color_gradient_06);
                    break;
            }
            save_config ();
            deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);
        }
        if (response == GTK_RESPONSE_APPLY) {
            continue;
        }
        break;
    }
    gtk_widget_destroy (spectrogram_properties);
#pragma GCC diagnostic pop
    return;
}

void
w_spectrogram_destroy (ddb_gtkui_widget_t *w) {
    w_spectrogram_t *s = (w_spectrogram_t *)w;
    deadbeef->vis_waveform_unlisten (w);
    
    // Free stereo data arrays
    if (s->data_left) {
        free (s->data_left);
        s->data_left = NULL;
    }
    if (s->data_right) {
        free (s->data_right);
        s->data_right = NULL;
    }
    
    // Free stereo sample arrays
    if (s->samples_left) {
        free (s->samples_left);
        s->samples_left = NULL;
    }
    if (s->samples_right) {
        free (s->samples_right);
        s->samples_right = NULL;
    }
    
    if (s->log_index) {
        free (s->log_index);
        s->log_index = NULL;
    }
    
    // Destroy stereo FFT plans
    if (s->p_r2c_left) {
        fftw_destroy_plan (s->p_r2c_left);
    }
    if (s->p_r2c_right) {
        fftw_destroy_plan (s->p_r2c_right);
    }
    
    // Free stereo FFT input arrays
    if (s->in_left) {
        fftw_free (s->in_left);
        s->in_left = NULL;
    }
    if (s->in_right) {
        fftw_free (s->in_right);
        s->in_right = NULL;
    }
    
    // Free stereo FFT output arrays
    if (s->out_complex_left) {
        fftw_free (s->out_complex_left);
        s->out_complex_left = NULL;
    }
    if (s->out_complex_right) {
        fftw_free (s->out_complex_right);
        s->out_complex_right = NULL;
    }
    
    if (s->drawtimer) {
        g_source_remove (s->drawtimer);
        s->drawtimer = 0;
    }
    if (s->surf) {
        cairo_surface_destroy (s->surf);
        s->surf = NULL;
    }
    if (s->mutex) {
        deadbeef->mutex_free (s->mutex);
        s->mutex = 0;
    }
}

gboolean
w_spectrogram_draw_cb (void *data) {
    w_spectrogram_t *s = data;
    gtk_widget_queue_draw (s->drawarea);
    return TRUE;
}

// Forward declaration
static void process_channel_samples(double *samples, const ddb_audio_data_t *data, int channel, int sz, int n, int nsamples);

static void
spectrogram_wavedata_listener (void *ctx, const ddb_audio_data_t *data) {
    w_spectrogram_t *w = ctx;
    if (!w->samples_left || !w->samples_right) {
        return;
    }
    
    deadbeef->mutex_lock (w->mutex);
    w->samplerate = (float)data->fmt->samplerate;
    int nsamples = data->nframes;
    int sz = MIN (FFT_SIZE, nsamples);
    int n = FFT_SIZE - sz;
    
    // Shift existing samples for both channels
    memmove (w->samples_left, w->samples_left + sz, (FFT_SIZE - sz)*sizeof (double));
    memmove (w->samples_right, w->samples_right + sz, (FFT_SIZE - sz)*sizeof (double));

    // Process left channel (channel 0)
    process_channel_samples(w->samples_left, data, 0, sz, n, nsamples);
    
    // Process right channel (channel 1)
    process_channel_samples(w->samples_right, data, 1, sz, n, nsamples);
    
    deadbeef->mutex_unlock (w->mutex);
    if (w->buffered < FFT_SIZE) {
        w->buffered += sz;
    }
}

// Helper function to get value from specific channel data
static inline float
spectrogram_get_value_from_data (double *data, int start, int end)
{
    if (start >= end) {
        return data[end];
    }
    float value = 0.0;
    for (int i = start; i < end; i++) {
        value = MAX (data[i], value);
    }
    return value;
}

// Helper function to process samples for a single channel
static void
process_channel_samples(double *samples, const ddb_audio_data_t *data, int channel, int sz, int n, int nsamples)
{
    float pos = 0;
    for (int i = 0; i < sz && pos < nsamples; i++, pos++) {
        int sample_idx = ftoi(pos * data->fmt->channels);
        if (channel < data->fmt->channels) {
            samples[n+i] = data->data[sample_idx + channel];
        } else {
            samples[n+i] = 0.0; // Fallback for mono input to stereo
        }
    }
}

static inline float
linear_interpolate (float y1, float y2, float mu)
{
       return (y1 * (1 - mu) + y2 * mu);
}

// Helper function to render a single channel in specified vertical range
static void
render_channel_spectrogram(w_spectrogram_t *w, double *channel_data, uint8_t *data, int stride, 
                          int width, int y_start, int y_end, int ratio)
{
    int channel_height = y_end - y_start;
    
    for (int i = 0; i < channel_height; i++) {
        float f = 1.0;
        int index0, index1;
        int bin0, bin1, bin2;
        
        if (CONFIG_LOG_SCALE) {
            // Scale log_index to channel height
            int scaled_i = (i * w->height) / channel_height;
            bin0 = w->log_index[CLAMP (scaled_i-1, 0, w->height-1)];
            bin1 = w->log_index[CLAMP (scaled_i, 0, w->height-1)];
            bin2 = w->log_index[CLAMP (scaled_i+1, 0, w->height-1)];
        } else {
            bin0 = (i-1) * ratio;
            bin1 = i * ratio;
            bin2 = (i+1) * ratio;
        }

        index0 = bin0 + ftoi ((bin1 - bin0)/2.f);
        if (index0 == bin0) index0 = bin1;
        index1 = bin1 + ftoi ((bin2 - bin1)/2.f);
        if (index1 == bin2) index1 = bin1;

        index0 = CLAMP (index0, 0, FFT_SIZE/2-1);
        index1 = CLAMP (index1, 0, FFT_SIZE/2-1);

        f = spectrogram_get_value_from_data (channel_data, index0, index1);
        float x = 10 * log10f (f);

        // Interpolation for log scale low resolution
        if (CONFIG_LOG_SCALE && i <= (w->low_res_end * channel_height) / w->height) {
            int j = 0;
            int scaled_i = (i * w->height) / channel_height;
            // Find index of next value
            while (scaled_i+j < w->height && w->log_index[scaled_i+j] == w->log_index[scaled_i]) {
                j++;
            }
            float v0 = x;
            float v1 = 0;
            if (scaled_i+j < w->height) {
                v1 = channel_data[w->log_index[scaled_i+j]];
                if (v1 != 0) {
                    v1 = 10 * log10f (v1);
                }
            }

            int k = 0;
            while ((k+scaled_i) >= 0 && w->log_index[k+scaled_i] == w->log_index[scaled_i]) {
                j++;
                k--;
            }
            if (j > 1) {
                x = linear_interpolate (v0, v1, (1.0/(j-1)) * ((-1 * k) - 1));
            }
        }

        // Apply dB range and color mapping
        x += CONFIG_DB_RANGE - 63;
        x = CLAMP (x, 0, CONFIG_DB_RANGE);
        int color_index = GRADIENT_TABLE_SIZE - ftoi (GRADIENT_TABLE_SIZE/(float)CONFIG_DB_RANGE * x);
        color_index = CLAMP (color_index, 0, GRADIENT_TABLE_SIZE-1);
        
        // Draw pixel at proper position (invert y for bottom-to-top frequency display)
        _draw_point (data, stride, width-1, y_end-1-i, w->colors[color_index]);
    }
}

static gboolean
spectrogram_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    w_spectrogram_t *w = user_data;
    GtkAllocation a;
    gtk_widget_get_allocation (widget, &a);
    if (!w->samples_left || !w->samples_right || a.height < 2) {
        return FALSE;
    }

    int width, height;
    width = a.width;
    height = a.height;
    int half_height = height / 2;
    int ratio = ftoi (FFT_SIZE/(half_height*2));
    ratio = CLAMP (ratio,0,1023);

    if (deadbeef->get_output ()->state () == OUTPUT_STATE_PLAYING) {
        do_fft (w);
        float log_scale = (log2f(w->samplerate/2)-log2f(25.))/(half_height);
        float freq_res = w->samplerate / FFT_SIZE;

        if (half_height != w->height) {
            w->height = MIN (half_height, MAX_HEIGHT);
            for (int i = 0; i < w->height; i++) {
                w->log_index[i] = ftoi (powf(2.,((float)i) * log_scale + log2f(25.)) / freq_res);
                if (i > 0 && w->log_index[i-1] == w->log_index [i]) {
                    w->low_res_end = i;
                }
            }
        }
    }

    // start drawing
    if (!w->surf || cairo_image_surface_get_width (w->surf) != a.width || cairo_image_surface_get_height (w->surf) != a.height) {
        if (w->surf) {
            cairo_surface_destroy (w->surf);
            w->surf = NULL;
        }
        w->surf = cairo_image_surface_create (CAIRO_FORMAT_RGB24, a.width, a.height);
    }

    cairo_surface_flush (w->surf);

    unsigned char *data = cairo_image_surface_get_data (w->surf);
    if (!data) {
        return FALSE;
    }
    int stride = cairo_image_surface_get_stride (w->surf);

    if (deadbeef->get_output ()->state () == OUTPUT_STATE_PLAYING) {
        for (int i = 0; i < a.height; i++) {
            // scrolling: move line i 1px to the left
            memmove (data + (i*stride), data + sizeof (uint32_t) + (i*stride), stride - sizeof (uint32_t));
        }

        // Render left channel in top half
        render_channel_spectrogram(w, w->data_left, data, stride, width, 0, half_height, ratio);
        
        // Render right channel in bottom half
        render_channel_spectrogram(w, w->data_right, data, stride, width, half_height, height, ratio);
    }
    cairo_surface_mark_dirty (w->surf);

    cairo_save (cr);
    cairo_set_source_surface (cr, w->surf, 0, 0);
    cairo_rectangle (cr, 0, 0, a.width, a.height);
    cairo_fill (cr);
    cairo_restore (cr);

    return FALSE;
}


gboolean
spectrogram_expose_event (GtkWidget *widget, GdkEventExpose *event, gpointer user_data) {
    cairo_t *cr = gdk_cairo_create (gtk_widget_get_window (widget));
    gboolean res = spectrogram_draw (widget, cr, user_data);
    cairo_destroy (cr);
    return res;
}


gboolean
spectrogram_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    //w_spectrogram_t *w = user_data;
    if (event->button == 3) {
      return TRUE;
    }
    return TRUE;
}

gboolean
spectrogram_button_release_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    w_spectrogram_t *w = user_data;
    if (event->button == 3) {
      gtk_menu_popup (GTK_MENU (w->popup), NULL, NULL, NULL, w->drawarea, 0, gtk_get_current_event_time ());
      return TRUE;
    }
    return TRUE;
}

static gboolean
spectrogram_set_refresh_interval (gpointer user_data, int interval)
{
    w_spectrogram_t *w = user_data;
    if (!w || interval <= 0) {
        return FALSE;
    }
    if (w->drawtimer) {
        g_source_remove (w->drawtimer);
        w->drawtimer = 0;
    }
    w->drawtimer = g_timeout_add (interval, w_spectrogram_draw_cb, w);
    return TRUE;
}

static int
spectrogram_message (ddb_gtkui_widget_t *widget, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2)
{
    w_spectrogram_t *w = (w_spectrogram_t *)widget;

    switch (id) {
        case DB_EV_CONFIGCHANGED:
            on_config_changed (w, ctx);
            spectrogram_set_refresh_interval (w, CONFIG_REFRESH_INTERVAL);
            break;
        case DB_EV_SONGSTARTED:
            spectrogram_set_refresh_interval (w, CONFIG_REFRESH_INTERVAL);
            break;
        case DB_EV_PAUSED:
            if (deadbeef->get_output ()->state () == OUTPUT_STATE_PLAYING) {
                spectrogram_set_refresh_interval (w, CONFIG_REFRESH_INTERVAL);
            }
            else {
                if (w->drawtimer) {
                    g_source_remove (w->drawtimer);
                    w->drawtimer = 0;
                }
            }
            break;
        case DB_EV_STOP:
            if (w->drawtimer) {
                g_source_remove (w->drawtimer);
                w->drawtimer = 0;
            }
            break;
    }
    return 0;
}

void
w_spectrogram_init (ddb_gtkui_widget_t *w) {
    w_spectrogram_t *s = (w_spectrogram_t *)w;
    load_config ();
    deadbeef->mutex_lock (s->mutex);
    
    // Allocate stereo sample buffers
    s->samples_left = malloc (sizeof (double) * FFT_SIZE);
    s->samples_right = malloc (sizeof (double) * FFT_SIZE);
    memset (s->samples_left, 0, sizeof (double) * FFT_SIZE);
    memset (s->samples_right, 0, sizeof (double) * FFT_SIZE);
    
    // Allocate stereo data buffers
    s->data_left = malloc (sizeof (double) * FFT_SIZE);
    s->data_right = malloc (sizeof (double) * FFT_SIZE);
    memset (s->data_left, 0, sizeof (double) * FFT_SIZE);
    memset (s->data_right, 0, sizeof (double) * FFT_SIZE);
    
    if (s->drawtimer) {
        g_source_remove (s->drawtimer);
        s->drawtimer = 0;
    }
    s->samplerate = 44100.0;
    s->height = 0;
    s->low_res_end = 0;
    s->log_index = (int *)malloc (sizeof (int) * MAX_HEIGHT);
    memset (s->log_index, 0, sizeof (int) * MAX_HEIGHT);

    for (int i = 0; i < FFT_SIZE; i++) {
        // Blackman-Harris window
        s->window[i] = 0.35875 - 0.48829 * cos(2 * M_PI * i /(FFT_SIZE)) + 0.14128 * cos(4 * M_PI * i/(FFT_SIZE)) - 0.01168 * cos(6 * M_PI * i/(FFT_SIZE));;
    }
    create_gradient_table (s, CONFIG_GRADIENT_COLORS, CONFIG_NUM_COLORS);
    
    // Allocate stereo FFT input buffers
    s->in_left = fftw_malloc (sizeof (double) * FFT_SIZE);
    s->in_right = fftw_malloc (sizeof (double) * FFT_SIZE);
    memset (s->in_left, 0, sizeof (double) * FFT_SIZE);
    memset (s->in_right, 0, sizeof (double) * FFT_SIZE);
    
    // Allocate stereo FFT output buffers
    s->out_complex_left = fftw_malloc (sizeof (fftw_complex) * FFT_SIZE);
    s->out_complex_right = fftw_malloc (sizeof (fftw_complex) * FFT_SIZE);
    
    // Create stereo FFT plans
    s->p_r2c_left = fftw_plan_dft_r2c_1d (FFT_SIZE, s->in_left, s->out_complex_left, FFTW_ESTIMATE);
    s->p_r2c_right = fftw_plan_dft_r2c_1d (FFT_SIZE, s->in_right, s->out_complex_right, FFTW_ESTIMATE);
    
    spectrogram_set_refresh_interval (s, CONFIG_REFRESH_INTERVAL);
    deadbeef->mutex_unlock (s->mutex);
}

ddb_gtkui_widget_t *
w_spectrogram_create (void) {
    w_spectrogram_t *w = malloc (sizeof (w_spectrogram_t));
    memset (w, 0, sizeof (w_spectrogram_t));

    w->base.widget = gtk_event_box_new ();
    w->base.init = w_spectrogram_init;
    w->base.destroy  = w_spectrogram_destroy;
    w->base.message = spectrogram_message;
    w->drawarea = gtk_drawing_area_new ();
    w->popup = gtk_menu_new ();
    w->popup_item = gtk_menu_item_new_with_mnemonic ("Configure");
    w->mutex = deadbeef->mutex_create ();
    gtk_widget_show (w->drawarea);
    gtk_container_add (GTK_CONTAINER (w->base.widget), w->drawarea);
    gtk_widget_show (w->popup);
    //gtk_container_add (GTK_CONTAINER (w->drawarea), w->popup);
    gtk_widget_show (w->popup_item);
    gtk_container_add (GTK_CONTAINER (w->popup), w->popup_item);
#if !GTK_CHECK_VERSION(3,0,0)
    g_signal_connect_after ((gpointer) w->drawarea, "expose_event", G_CALLBACK (spectrogram_expose_event), w);
#else
    g_signal_connect_after ((gpointer) w->drawarea, "draw", G_CALLBACK (spectrogram_draw), w);
#endif
    g_signal_connect_after ((gpointer) w->base.widget, "button_press_event", G_CALLBACK (spectrogram_button_press_event), w);
    g_signal_connect_after ((gpointer) w->base.widget, "button_release_event", G_CALLBACK (spectrogram_button_release_event), w);
    g_signal_connect_after ((gpointer) w->popup_item, "activate", G_CALLBACK (on_button_config), w);
    gtkui_plugin->w_override_signals (w->base.widget, w);
    deadbeef->vis_waveform_listen (w, spectrogram_wavedata_listener);
    return (ddb_gtkui_widget_t *)w;
}

int
spectrogram_connect (void)
{
    gtkui_plugin = (ddb_gtkui_t *) deadbeef->plug_get_for_id (DDB_GTKUI_PLUGIN_ID);
    if (gtkui_plugin) {
        //trace("using '%s' plugin %d.%d\n", DDB_GTKUI_PLUGIN_ID, gtkui_plugin->gui.plugin.version_major, gtkui_plugin->gui.plugin.version_minor );
        if (gtkui_plugin->gui.plugin.version_major == 2) {
            //printf ("fb api2\n");
            // 0.6+, use the new widget API
            gtkui_plugin->w_reg_widget ("Stereo Spectrogram", 0, w_spectrogram_create, "stereo_spectrogram", NULL);
            return 0;
        }
    }
    return -1;
}

int
spectrogram_start (void)
{
    load_config ();
    return 0;
}

int
spectrogram_stop (void)
{
    save_config ();
    return 0;
}

int
spectrogram_startup (GtkWidget *cont)
{
    return 0;
}

int
spectrogram_shutdown (GtkWidget *cont)
{
    return 0;
}
int
spectrogram_disconnect (void)
{
    gtkui_plugin = NULL;
    return 0;
}

static const char settings_dlg[] =
    "property \"Refresh interval (ms): \"          spinbtn[10,1000,1] "      CONFSTR_SP_REFRESH_INTERVAL        " 25 ;\n"
;

static DB_misc_t plugin = {
    //DB_PLUGIN_SET_API_VERSION
    .plugin.type            = DB_PLUGIN_MISC,
    .plugin.api_vmajor      = 1,
    .plugin.api_vminor      = 5,
    .plugin.version_major   = 0,
    .plugin.version_minor   = 1,
#if GTK_CHECK_VERSION(3,0,0)
    .plugin.id              = "stereo_spectrogram-gtk3",
#else
    .plugin.id              = "stereo_spectrogram",
#endif
    .plugin.name            = "Stereo Spectrogram",
    .plugin.descr           = "Stereo Spectrogram",
    .plugin.copyright       =
        "Copyright (C) 2013 Christian Boxdörfer <christian.boxdoerfer@posteo.de>\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
    ,
    .plugin.website         = "https://github.com/cboxdoerfer/ddb_spectrogram",
    .plugin.start           = spectrogram_start,
    .plugin.stop            = spectrogram_stop,
    .plugin.connect         = spectrogram_connect,
    .plugin.disconnect      = spectrogram_disconnect,
    .plugin.configdialog    = settings_dlg,
};

#if !GTK_CHECK_VERSION(3,0,0)
DB_plugin_t *
ddb_vis_stereo_spectrogram_GTK2_load (DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
#else
DB_plugin_t *
ddb_vis_stereo_spectrogram_GTK3_load (DB_functions_t *ddb) {
    deadbeef = ddb;
    return &plugin.plugin;
}
#endif
