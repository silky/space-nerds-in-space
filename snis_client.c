/*
        Copyright (C) 2010 Stephen M. Cameron
       Author: Stephen M. Cameron

        This file is part of Spacenerds In Space.

        Spacenerds in Space is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.

        Spacenerds in Space is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with Spacenerds in Space; if not, write to the Free Software
        Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdint.h>
#include <malloc.h>
#include <gtk/gtk.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gdk/gdkevents.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netdb.h>
#include <assert.h>
#include <linux/tcp.h>

#include "snis.h"
#include "mathutils.h"
#include "snis_alloc.h"
#include "my_point.h"
#include "snis_font.h"
#include "snis_typeface.h"
#include "snis_graph.h"
#include "snis_ui_element.h"
#include "snis_gauge.h"
#include "snis_button.h"
#include "snis_sliders.h"
#include "snis_text_window.h"
#include "snis_socket_io.h"
#include "ssgl/ssgl.h"
#include "snis_marshal.h"
#include "snis_packet.h"
#include "wwviaudio.h"
#include "sounds.h"
#include "bline.h"
#include "shield_strength.h"

#define SCREEN_WIDTH 800        /* window width, in pixels */
#define SCREEN_HEIGHT 600       /* window height, in pixels */

__attribute__((unused)) static double max_speed[];

typedef void explosion_function(int x, int y, int ivx, int ivy, int v, int nsparks, int time);

explosion_function *explosion = NULL;

/* I can switch out the line drawing function with these macros */
/* in case I come across something faster than gdk_draw_line */
#define DEFAULT_LINE_STYLE sng_current_draw_line
#define DEFAULT_RECTANGLE_STYLE sng_current_draw_rectangle
#define DEFAULT_BRIGHT_LINE_STYLE sng_current_bright_line
#define DEFAULT_DRAW_ARC sng_current_draw_arc

#define snis_draw_line DEFAULT_LINE_STYLE
#define snis_draw_rectangle DEFAULT_RECTANGLE_STYLE
#define snis_bright_line DEFAULT_BRIGHT_LINE_STYLE
#define snis_draw_arc DEFAULT_DRAW_ARC
int thicklines = 0;
int frame_rate_hz = 30;

GtkWidget *window;
GdkGC *gc = NULL;               /* our graphics context. */
GtkWidget *main_da;             /* main drawing area. */
gint timer_tag;  
int fullscreen = 0;
int in_the_process_of_quitting = 0;

uint32_t role = ROLE_ALL;
char *password;
char *shipname;
#define UNKNOWN_ID 0xffffffff
uint32_t my_ship_id = UNKNOWN_ID;
uint32_t my_ship_oid = UNKNOWN_ID;

float xscale_screen = 1.0;
float yscale_screen= 1.0;
int real_screen_width;
int real_screen_height;

int displaymode = DISPLAYMODE_LOBBYSCREEN;

struct client_network_stats {
	uint64_t bytes_sent;
	uint64_t bytes_recd;
	uint32_t elapsed_seconds;
} netstats;

int nframes = 0;
int timer = 0;
struct timeval start_time, end_time;

volatile int done_with_lobby = 0;
pthread_t lobby_thread; pthread_attr_t lobby_attr;
pthread_t gameserver_connect_thread; pthread_attr_t gameserver_connect_attr;
pthread_t read_from_gameserver_thread; pthread_attr_t gameserver_reader_attr;

pthread_t write_to_gameserver_thread; pthread_attr_t gameserver_writer_attr;
struct packed_buffer_queue to_server_queue;
pthread_mutex_t to_server_queue_mutex;
pthread_mutex_t to_server_queue_event_mutex;
pthread_cond_t server_write_cond = PTHREAD_COND_INITIALIZER;
int have_packets_for_server = 0;

int lobby_socket = -1;
int gameserver_sock = -1;
int lobby_count = 0;
char lobbyerror[200];
char *lobbyhost = "localhost";
struct ssgl_game_server lobby_game_server[100];
int ngameservers = 0;

struct ui_element_list *uiobjs = NULL;
ui_element_drawing_function ui_slider_draw = (ui_element_drawing_function) snis_slider_draw;
ui_element_button_press_function ui_slider_button_press = (ui_element_button_press_function) snis_slider_button_press;

double sine[361];
double cosine[361];

void init_trig_arrays(void)
{
	int i;

	for (i = 0; i <= 360; i++) {
		sine[i] = sin((double)i * M_PI * 2.0 / 360.0);
		cosine[i] = cos((double)i * M_PI * 2.0 / 360.0);
	}
}

static void *connect_to_lobby_thread(__attribute__((unused)) void *arg)
{
	int i, sock, rc, game_server_count;
	struct ssgl_game_server *game_server = NULL;
	struct ssgl_client_filter filter;

try_again:

	/* Loop, trying to connect to the lobby server... */
	strcpy(lobbyerror, "");
	while (1) {
		sock = ssgl_gameclient_connect_to_lobby(lobbyhost);
		lobby_count++;
		if (sock >= 0) {
			lobby_socket = sock;
			break;
		}
		if (errno)
			sprintf(lobbyerror, "%s (%d)", strerror(errno), errno);
		else
			sprintf(lobbyerror, "%s (%d)", 
				gai_strerror(sock), sock);
		ssgl_sleep(5);
	}
	strcpy(lobbyerror, "");

	/* Ok, we've connected to the lobby server... */

	strcpy(filter.game_type, "SNIS");
	do {
		rc = ssgl_recv_game_servers(sock, &game_server, &game_server_count, &filter);
		if (rc) {
			sprintf(lobbyerror, "ssgl_recv_game_servers failed: %s\n", strerror(errno));
			goto handle_error;
		}
	
		/* copy the game server data to where the display thread will see it. */
		if (game_server_count > 100)
			game_server_count = 100;
		for (i = 0; i < game_server_count; i++)
			lobby_game_server[i] = game_server[i];
		ngameservers = game_server_count;
		if (game_server_count > 0) {
			free(game_server);
			game_server_count = 0;
		}
		ssgl_sleep(5);  /* just a thread safe sleep. */
	} while (!done_with_lobby);

	/* close connection to the lobby */
	shutdown(sock, SHUT_RDWR);
	close(sock);
	lobby_socket = -1;
	return NULL;

handle_error:
	if (lobby_socket >= 0)
		close(lobby_socket);
	lobby_socket = -1;
	ssgl_sleep(5);
	if (game_server_count > 0) {
		free(game_server);
		game_server_count = 0;
		game_server = NULL;
	}
	goto try_again;
}

static void connect_to_lobby()
{
        pthread_attr_init(&lobby_attr);
        pthread_attr_setdetachstate(&lobby_attr, PTHREAD_CREATE_JOINABLE);
	(void) pthread_create(&lobby_thread, &lobby_attr, connect_to_lobby_thread, NULL);
	return;
}

static struct snis_object_pool *pool;
static struct snis_entity go[MAXGAMEOBJS];
static struct snis_object_pool *sparkpool;
static struct snis_entity spark[MAXSPARKS];
static pthread_mutex_t universe_mutex = PTHREAD_MUTEX_INITIALIZER;

static int add_generic_object(uint32_t id, double x, double y, double vx, double vy, double heading, int type, uint32_t alive)
{
	int i;

	i = snis_object_pool_alloc_obj(pool); 	 
	if (i < 0) {
		printf("snis_object_pool_alloc_obj failed\n");
		return -1;
	}
	memset(&go[i], 0, sizeof(go[i]));
	go[i].index = i;
	go[i].id = id;
	go[i].x = x;
	go[i].y = y;
	go[i].vx = vx;
	go[i].vy = vy;
	go[i].heading = heading;
	go[i].type = type;
	go[i].alive = alive;
	return i;
}

static void update_generic_object(int index, double x, double y, double vx, double vy, double heading, uint32_t alive)
{
	go[index].x = x;
	go[index].y = y;
	go[index].vx = vx;
	go[index].vy = vy;
	go[index].heading = heading;
	go[index].alive = alive;
}

static int lookup_object_by_id(uint32_t id)
{
	int i;

	for (i = 0; i <= snis_object_pool_highest_object(pool); i++)
		if (go[i].id == id)
			return i;
	return -1;
}

static int update_econ_ship(uint32_t id, double x, double y, double vx,
			double vy, double heading, uint32_t alive)
{
	int i;
	i = lookup_object_by_id(id);
	if (i < 0) {
		i = add_generic_object(id, x, y, vx, vy, heading, OBJTYPE_SHIP2, alive);
		if (i < 0)
			return i;
	} else {
		update_generic_object(i, x, y, vx, vy, heading, alive); 
	}
	return 0;
}

static int update_ship(uint32_t id, double x, double y, double vx, double vy, double heading, uint32_t alive,
			uint32_t torpedoes, uint32_t power, 
			double gun_heading, double sci_heading, double sci_beam_width, int shiptype,
			uint8_t tloading, uint8_t tloaded, uint8_t throttle, uint8_t rpm, uint32_t
			fuel, uint8_t temp, struct power_dist *pd, uint8_t scizoom, uint8_t warpdrive,
			uint8_t requested_warpdrive, uint8_t requested_shield, uint8_t phaser_charge, uint8_t phaser_wavelength)
{
	int i;
	i = lookup_object_by_id(id);
	if (i < 0) {
		i = add_generic_object(id, x, y, vx, vy, heading, shiptype, alive);
		if (i < 0)
			return i;
	} else {
		update_generic_object(i, x, y, vx, vy, heading, alive); 
	}
	go[i].tsd.ship.torpedoes = torpedoes;
	go[i].tsd.ship.power = power;
	go[i].tsd.ship.gun_heading = gun_heading;
	go[i].tsd.ship.sci_heading = sci_heading;
	go[i].tsd.ship.sci_beam_width = sci_beam_width;
	go[i].tsd.ship.torpedoes_loaded = tloaded;
	go[i].tsd.ship.torpedoes_loading = tloading;
	go[i].tsd.ship.throttle = throttle;
	go[i].tsd.ship.rpm = rpm;
	go[i].tsd.ship.fuel = fuel;
	go[i].tsd.ship.temp = temp;
	go[i].tsd.ship.pwrdist = *pd;
	go[i].tsd.ship.scizoom = scizoom;
	go[i].tsd.ship.requested_warpdrive = requested_warpdrive;
	go[i].tsd.ship.requested_shield = requested_shield;
	go[i].tsd.ship.warpdrive = warpdrive;
	go[i].tsd.ship.phaser_charge = phaser_charge;
	go[i].tsd.ship.phaser_wavelength = phaser_wavelength;
	return 0;
}

static int update_ship_sdata(uint32_t id, uint8_t subclass, char *name,
				uint8_t shield_strength, uint8_t shield_wavelength,
				uint8_t shield_width, uint8_t shield_depth)
{
	int i;
	i = lookup_object_by_id(id);
	if (i < 0)
		return i;
	go[i].sdata.subclass = subclass;
	go[i].sdata.shield_strength = shield_strength;
	go[i].sdata.shield_wavelength = shield_wavelength;
	go[i].sdata.shield_width = shield_width;
	go[i].sdata.shield_depth = shield_depth;
	strcpy(go[i].sdata.name, name);
	go[i].sdata.science_data_known = 30 * 10; /* only remember for ten secs. */
	go[i].sdata.science_data_requested = 0; /* request is fullfilled */
	return 0;
}

static int update_torpedo(uint32_t id, double x, double y, double vx, double vy, uint32_t ship_id)
{
	int i;
	i = lookup_object_by_id(id);
	if (i < 0) {
		i = add_generic_object(id, x, y, vx, vy, 0.0, OBJTYPE_TORPEDO, 1);
		if (i < 0)
			return i;
		go[i].tsd.torpedo.ship_id = ship_id;
	} else {
		update_generic_object(i, x, y, vx, vy, 0.0, 1); 
	}
	return 0;
}

static int update_laser(uint32_t id, double x, double y, double vx, double vy, uint32_t ship_id)
{
	int i;
	i = lookup_object_by_id(id);
	if (i < 0) {
		i = add_generic_object(id, x, y, vx, vy, 0.0, OBJTYPE_LASER, 1);
		if (i < 0)
			return i;
		go[i].tsd.laser.ship_id = ship_id;
	} else {
		update_generic_object(i, x, y, vx, vy, 0.0, 1); 
	}
	return 0;
}

static int update_planet(uint32_t id, double x, double y)
{
	int i;
	i = lookup_object_by_id(id);
	if (i < 0) {
		i = add_generic_object(id, x, y, 0.0, 0.0, 0.0, OBJTYPE_PLANET, 1);
		if (i < 0)
			return i;
	} else {
		update_generic_object(i, x, y, 0.0, 0.0, 0.0, 1);
	}
	return 0;
}

static int update_starbase(uint32_t id, double x, double y)
{
	int i;
	i = lookup_object_by_id(id);
	if (i < 0) {
		i = add_generic_object(id, x, y, 0.0, 0.0, 0.0, OBJTYPE_STARBASE, 1);
		if (i < 0)
			return i;
	} else {
		update_generic_object(i, x, y, 0.0, 0.0, 0.0, 1);
	}
	return 0;
}


static void spark_move(struct snis_entity *o)
{
	o->x += o->vx;
	o->y += o->vy;
	o->alive--;
	if (o->alive <= 0)
		snis_object_pool_free_object(sparkpool, o->index);
}

static void move_sparks(void)
{
	int i;

	for (i = 0; i <= snis_object_pool_highest_object(sparkpool); i++)
		if (spark[i].alive)
			spark[i].move(&spark[i]);
}

void add_spark(double x, double y, double vx, double vy, int time)
{
	int i;

	i = snis_object_pool_alloc_obj(sparkpool);
	if (i < 0)
		return;
	memset(&spark[i], 0, sizeof(spark[i]));
	spark[i].index = i;
	spark[i].x = x;
	spark[i].y = y;
	spark[i].vx = vx;
	spark[i].vy = vy;
	spark[i].type = OBJTYPE_SPARK;
	spark[i].alive = time + snis_randn(time);
	spark[i].move = spark_move;
	return;
}

#if 0
void sphere_explode(int x, int y, int ivx, int ivy, int v,
	int nsparks, int time)
{
	int vx, vy;
	int angle;
	int shell;
	int velocity;
	int delta_angle;
	float point_dist;
	int angle_offset;

	angle_offset = snis_randn(360);

	if (nsparks < 30)
		nsparks = 40;
	point_dist = v * 2 * M_PI / (nsparks / 4);
	delta_angle = (int) ((point_dist / (2.0 * M_PI * v)) * 360.0);

	v = v / (1.0 + snis_randn(100) / 100.0);
	for (shell = 0; shell < 90; shell += 7) {
		for (angle = 0; angle < 360; angle += delta_angle) {
			velocity = (int) (cosine[shell] * (double) v);
			vx = cosine[(angle + angle_offset) % 360] * velocity + ivx;
			vy = sine[(angle + angle_offset) % 360] * velocity + ivy;
			add_spark(x, y, vx, vy, time);
		}
		delta_angle = (int) ((point_dist / (2.0 * velocity * M_PI)) * 360.0);
	}
}
#endif

static void do_explosion(double x, double y, uint16_t nsparks, uint16_t velocity, int time)
{
	double angle, v, vx, vy;
	int i;

	for (i = 0; i < nsparks; i++) {
		angle = ((double) snis_randn(360) * M_PI / 180.0);
		v = snis_randn(velocity * 2) - velocity;
		vx = v * cos(angle);
		vy = v * sin(angle);
		add_spark(x, y, vx, vy, time);
	}
}

static int update_explosion(uint32_t id, double x, double y,
		uint16_t nsparks, uint16_t velocity, uint16_t time)
{
	int i;
	i = lookup_object_by_id(id);
	if (i < 0) {
		i = add_generic_object(id, x, y, 0.0, 0.0, 0.0, OBJTYPE_EXPLOSION, 1);
		if (i < 0)
			return i;
		go[i].tsd.explosion.nsparks = nsparks;
		go[i].tsd.explosion.velocity = velocity;
		do_explosion(x, y, nsparks, velocity, (int) time);
	}
	return 0;
}

static int __attribute__((unused)) add_planet(uint32_t id, double x, double y, double vx, double vy, double heading, uint32_t alive)
{
	return add_generic_object(id, x, y, vx, vy, heading, OBJTYPE_PLANET, alive);
}

static int __attribute__((unused)) add_starbase(uint32_t id, double x, double y, double vx, double vy, double heading, uint32_t alive)
{
	return add_generic_object(id, x, y, vx, vy, heading, OBJTYPE_STARBASE, alive);
}

static int __attribute__((unused)) add_torpedo(uint32_t id, double x, double y, double vx, double vy, double heading, uint32_t alive)
{
	return add_generic_object(id, x, y, vx, vy, heading, OBJTYPE_TORPEDO, alive);
}

void spin_points(struct my_point_t *points, int npoints, 
	struct my_point_t **spun_points, int nangles,
	int originx, int originy)
{
	int i, j;
	double angle;
	double angle_inc;
	double startx, starty, start_angle, new_angle, newx, newy;
	double magnitude, diff;

	*spun_points = (struct my_point_t *) 
		malloc(sizeof(*spun_points) * npoints * nangles);
	if (*spun_points == NULL)
		return;

	angle_inc = (2.0*3.1415927) / (nangles);

	for (i = 0; i < nangles; i++) {
		angle = angle_inc * (double) i;
		printf("Rotation angle = %f\n", angle * 180.0/3.1415927);
		for (j=0;j<npoints;j++) {
			startx = (double) (points[j].x - originx);
			starty = (double) (points[j].y - originy);
			magnitude = sqrt(startx*startx + starty*starty);
			diff = (startx - 0);
			if (diff < 0 && diff > -0.00001)
				start_angle = 0.0;
			else if (diff > 00 && diff < 0.00001)
				start_angle = 3.1415927;
			else
				start_angle = atan2(starty, startx);
			new_angle = start_angle + angle;
			newx = cos(new_angle) * magnitude + originx;
			newy = sin(new_angle) * magnitude + originy;
			(*spun_points)[i*npoints + j].x = newx;
			(*spun_points)[i*npoints + j].y = newy;
			printf("s=%f,%f, e=%f,%f, angle = %f/%f\n", 
				startx, starty, newx, newy, 
				start_angle * 180/3.1415927, new_angle * 360.0 / (2.0*3.1415927)); 
		}
	} 
}

/**********************************/
/* keyboard handling stuff begins */

struct keyname_value_entry {
	char *name;
	unsigned int value;
} keyname_value_map[] = {
	{ "a", GDK_a }, 
	{ "b", GDK_b }, 
	{ "c", GDK_c }, 
	{ "d", GDK_d }, 
	{ "e", GDK_e }, 
	{ "f", GDK_f }, 
	{ "g", GDK_g }, 
	{ "h", GDK_h }, 
	{ "i", GDK_i }, 
	{ "j", GDK_j }, 
	{ "k", GDK_k }, 
	{ "l", GDK_l }, 
	{ "m", GDK_m }, 
	{ "n", GDK_n }, 
	{ "o", GDK_o }, 
	{ "p", GDK_p }, 
	{ "q", GDK_q }, 
	{ "r", GDK_r }, 
	{ "s", GDK_s }, 
	{ "t", GDK_t }, 
	{ "u", GDK_u }, 
	{ "v", GDK_v }, 
	{ "w", GDK_w }, 
	{ "x", GDK_x }, 
	{ "y", GDK_y }, 
	{ "z", GDK_z }, 
	{ "A", GDK_A }, 
	{ "B", GDK_B }, 
	{ "C", GDK_C }, 
	{ "D", GDK_D }, 
	{ "E", GDK_E }, 
	{ "F", GDK_F }, 
	{ "G", GDK_G }, 
	{ "H", GDK_H }, 
	{ "I", GDK_I }, 
	{ "J", GDK_J }, 
	{ "K", GDK_K }, 
	{ "L", GDK_L }, 
	{ "M", GDK_M }, 
	{ "N", GDK_N }, 
	{ "O", GDK_O }, 
	{ "P", GDK_P }, 
	{ "Q", GDK_Q }, 
	{ "R", GDK_R }, 
	{ "S", GDK_S }, 
	{ "T", GDK_T }, 
	{ "U", GDK_U }, 
	{ "V", GDK_V }, 
	{ "W", GDK_W }, 
	{ "X", GDK_X }, 
	{ "Y", GDK_Y }, 
	{ "Z", GDK_Z }, 
	{ "0", GDK_0 }, 
	{ "1", GDK_1 }, 
	{ "2", GDK_2 }, 
	{ "3", GDK_3 }, 
	{ "4", GDK_4 }, 
	{ "5", GDK_5 }, 
	{ "6", GDK_6 }, 
	{ "7", GDK_7 }, 
	{ "8", GDK_8 }, 
	{ "9", GDK_9 }, 
	{ "-", GDK_minus }, 
	{ "+", GDK_plus }, 
	{ "=", GDK_equal }, 
	{ "?", GDK_question }, 
	{ ".", GDK_period }, 
	{ ",", GDK_comma }, 
	{ "<", GDK_less }, 
	{ ">", GDK_greater }, 
	{ ":", GDK_colon }, 
	{ ";", GDK_semicolon }, 
	{ "@", GDK_at }, 
	{ "*", GDK_asterisk }, 
	{ "$", GDK_dollar }, 
	{ "%", GDK_percent }, 
	{ "&", GDK_ampersand }, 
	{ "'", GDK_apostrophe }, 
	{ "(", GDK_parenleft }, 
	{ ")", GDK_parenright }, 
	{ "space", GDK_space }, 
	{ "enter", GDK_Return }, 
	{ "return", GDK_Return }, 
	{ "backspace", GDK_BackSpace }, 
	{ "delete", GDK_Delete }, 
	{ "pause", GDK_Pause }, 
	{ "scrolllock", GDK_Scroll_Lock }, 
	{ "escape", GDK_Escape }, 
	{ "sysreq", GDK_Sys_Req }, 
	{ "left", GDK_Left }, 
	{ "right", GDK_Right }, 
	{ "up", GDK_Up }, 
	{ "down", GDK_Down }, 
	{ "kp_home", GDK_KP_Home }, 
	{ "kp_down", GDK_KP_Down }, 
	{ "kp_up", GDK_KP_Up }, 
	{ "kp_left", GDK_KP_Left }, 
	{ "kp_right", GDK_KP_Right }, 
	{ "kp_end", GDK_KP_End }, 
	{ "kp_delete", GDK_KP_Delete }, 
	{ "kp_insert", GDK_KP_Insert }, 
	{ "home", GDK_Home }, 
	{ "down", GDK_Down }, 
	{ "up", GDK_Up }, 
	{ "left", GDK_Left }, 
	{ "right", GDK_Right }, 
	{ "end", GDK_End }, 
	{ "delete", GDK_Delete }, 
	{ "insert", GDK_Insert }, 
	{ "kp_0", GDK_KP_0 }, 
	{ "kp_1", GDK_KP_1 }, 
	{ "kp_2", GDK_KP_2 }, 
	{ "kp_3", GDK_KP_3 }, 
	{ "kp_4", GDK_KP_4 }, 
	{ "kp_5", GDK_KP_5 }, 
	{ "kp_6", GDK_KP_6 }, 
	{ "kp_7", GDK_KP_7 }, 
	{ "kp_8", GDK_KP_8 }, 
	{ "kp_9", GDK_KP_9 }, 
	{ "f1", GDK_F1 }, 
	{ "f2", GDK_F2 }, 
	{ "f3", GDK_F3 }, 
	{ "f4", GDK_F4 }, 
	{ "f5", GDK_F5 }, 
	{ "f6", GDK_F6 }, 
	{ "f7", GDK_F7 }, 
	{ "f9", GDK_F9 }, 
	{ "f9", GDK_F9 }, 
	{ "f10", GDK_F10 }, 
	{ "f11", GDK_F11 }, 
	{ "f12", GDK_F12 }, 
};

enum keyaction { keynone, keydown, keyup, keyleft, keyright, 
		keytorpedo, keytransform, 
		keyquarter, keypause, key2, key3, key4, key5, key6,
		key7, key8, keysuicide, keyfullscreen, keythrust, 
		keysoundeffects, keymusic, keyquit, keytogglemissilealarm,
		keypausehelp, keyreverse, keyf1, keyf2, keyf3, keyf4, keyf5,
		keyf6, keyf7, keyonscreen
};

enum keyaction keymap[256];
enum keyaction ffkeymap[256];
unsigned char *keycharmap[256];

char *keyactionstring[] = {
	"none", "down", "up", "left", "right", 
	"laser", "bomb", "chaff", "gravitybomb",
	"quarter", "pause", "2x", "3x", "4x", "5x", "6x",
	"7x", "8x", "suicide", "fullscreen", "thrust", 
	"soundeffect", "music", "quit", "missilealarm", "help", "reverse"
};
void init_keymap()
{
	memset(keymap, 0, sizeof(keymap));
	memset(ffkeymap, 0, sizeof(ffkeymap));
	memset(keycharmap, 0, sizeof(keycharmap));

	keymap[GDK_j] = keydown;
	ffkeymap[GDK_Down & 0x00ff] = keydown;

	keymap[GDK_k] = keyup;
	ffkeymap[GDK_Up & 0x00ff] = keyup;

	keymap[GDK_l] = keyright;
	ffkeymap[GDK_Right & 0x00ff] = keyright;
	keymap[GDK_period] = keyright;
	keymap[GDK_greater] = keyright;

	keymap[GDK_h] = keyleft;
	ffkeymap[GDK_Left & 0x00ff] = keyleft;
	keymap[GDK_comma] = keyleft;
	keymap[GDK_less] = keyleft;

	keymap[GDK_space] = keytorpedo;
	keymap[GDK_z] = keytorpedo;

	keymap[GDK_b] = keytransform;
	keymap[GDK_x] = keythrust;
	keymap[GDK_p] = keypause;
	ffkeymap[GDK_F1 & 0x00ff] = keypausehelp;
	keymap[GDK_q] = keyquarter;
	keymap[GDK_m] = keymusic;
	keymap[GDK_s] = keysoundeffects;
	ffkeymap[GDK_Escape & 0x00ff] = keyquit;
	keymap[GDK_1] = keytogglemissilealarm;

	keymap[GDK_2] = key2;
	keymap[GDK_3] = key3;
	keymap[GDK_4] = key4;
	keymap[GDK_5] = key5;
	keymap[GDK_6] = key6;
	keymap[GDK_7] = key7;
	keymap[GDK_8] = key8;
	keymap[GDK_9] = keysuicide;
	keymap[GDK_O] = keyonscreen;
	keymap[GDK_o] = keyonscreen;

	ffkeymap[GDK_F1 & 0x00ff] = keyf1;
	ffkeymap[GDK_F2 & 0x00ff] = keyf2;
	ffkeymap[GDK_F3 & 0x00ff] = keyf3;
	ffkeymap[GDK_F4 & 0x00ff] = keyf4;
	ffkeymap[GDK_F5 & 0x00ff] = keyf5;
	ffkeymap[GDK_F6 & 0x00ff] = keyf6;
	ffkeymap[GDK_F7 & 0x00ff] = keyf7;

	ffkeymap[GDK_F11 & 0x00ff] = keyfullscreen;
}

static void wakeup_gameserver_writer(void);

static void request_ship_sdata(struct snis_entity *o)
{
	struct packed_buffer *pb;

	pb = packed_buffer_allocate(sizeof(struct request_ship_sdata_packet));
	packed_buffer_append(pb, "hw", OPCODE_REQUEST_SHIP_SDATA, o->id);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	o->sdata.science_data_requested = 1;
	wakeup_gameserver_writer();
}

static void request_sci_select_target(uint32_t id)
{
	struct packed_buffer *pb;

	pb = packed_buffer_allocate(sizeof(struct snis_sci_select_target_packet));
	packed_buffer_append(pb, "hw", OPCODE_SCI_SELECT_TARGET, id);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	wakeup_gameserver_writer();
}

static void request_sci_select_coords(double ux, double uy)
{
	struct packed_buffer *pb;

	pb = packed_buffer_allocate(sizeof(struct snis_sci_select_coords_packet));
	packed_buffer_append(pb, "hSS", OPCODE_SCI_SELECT_COORDS,
		ux, (int32_t) UNIVERSE_DIM, uy, (int32_t) UNIVERSE_DIM);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	wakeup_gameserver_writer();
}

static void navigation_dirkey(int h, int v)
{
	struct packed_buffer *pb;
	uint8_t yaw, thrust;

	if (!h && !v)
		return;

	if (h) {
		yaw = h < 0 ? YAW_LEFT : YAW_RIGHT;
		pb = packed_buffer_allocate(sizeof(struct request_yaw_packet));
		packed_buffer_append(pb, "hb", OPCODE_REQUEST_YAW, yaw);
		packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	}
	if (v) {
		thrust = v < 0 ? THRUST_BACKWARDS : THRUST_FORWARDS;
		pb = packed_buffer_allocate(sizeof(struct request_thrust_packet));
		packed_buffer_append(pb, "hb", OPCODE_REQUEST_THRUST, thrust);
		packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	}
	wakeup_gameserver_writer();
}

static void weapons_dirkey(int h, int v)
{
	struct packed_buffer *pb;
	uint8_t yaw;

	if (!h && !v)
		return;

	if (h) {
		yaw = h < 0 ? YAW_LEFT : YAW_RIGHT;
		pb = packed_buffer_allocate(sizeof(struct request_yaw_packet));
		packed_buffer_append(pb, "hb", OPCODE_REQUEST_GUNYAW, yaw);
		packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
		wakeup_gameserver_writer();
	}
}

static void science_dirkey(int h, int v)
{
	struct packed_buffer *pb;
	uint8_t yaw;

	if (!h && !v)
		return;
	if (v) {
		yaw = v < 0 ? YAW_LEFT : YAW_RIGHT;
		pb = packed_buffer_allocate(sizeof(struct request_yaw_packet));
		packed_buffer_append(pb, "hb", OPCODE_REQUEST_SCIBEAMWIDTH, yaw);
		packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
		wakeup_gameserver_writer();
	}
	if (h) {
		yaw = h < 0 ? YAW_LEFT : YAW_RIGHT;
		pb = packed_buffer_allocate(sizeof(struct request_yaw_packet));
		packed_buffer_append(pb, "hb", OPCODE_REQUEST_SCIYAW, yaw);
		packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
		wakeup_gameserver_writer();
	}
}

static void do_onscreen(uint8_t mode)
{
	struct packed_buffer *pb;

	pb = packed_buffer_allocate(sizeof(struct role_onscreen_packet));
	packed_buffer_append(pb, "hb", OPCODE_ROLE_ONSCREEN, mode);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	wakeup_gameserver_writer();
}

static void do_dirkey(int h, int v)
{
	switch (displaymode) {
		case DISPLAYMODE_NAVIGATION:
			navigation_dirkey(h, v); 
			break;
		case DISPLAYMODE_WEAPONS:
			weapons_dirkey(h, v); 
			break;
		case DISPLAYMODE_SCIENCE:
			science_dirkey(h, v); 
			break;
		default:
			break;
	}
	return;
}

static void do_torpedo(void)
{
	struct packed_buffer *pb;
	struct snis_entity *o;

	if (displaymode != DISPLAYMODE_WEAPONS)
		return;

	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return;

	o = &go[my_ship_oid];

	if (o->tsd.ship.torpedoes_loaded <= 0)
		return;
	pb = packed_buffer_allocate(sizeof(struct request_yaw_packet));
	packed_buffer_append_u16(pb, OPCODE_REQUEST_TORPEDO);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	wakeup_gameserver_writer();
}

static void do_laser(void)
{
	struct packed_buffer *pb;
#if 0
	struct snis_entity *o;
#endif
	if (displaymode != DISPLAYMODE_WEAPONS)
		return;
#if 0
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return;

	o = &go[my_ship_oid];

	if (o->tsd.ship.phaser_bank_charge < 128.0)
		return;
#endif
	pb = packed_buffer_allocate(sizeof(struct request_yaw_packet));
	packed_buffer_append_u16(pb, OPCODE_REQUEST_LASER);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	wakeup_gameserver_writer();
}

static int control_key_pressed = 0;

static gint key_press_cb(GtkWidget* widget, GdkEventKey* event, gpointer data)
{
	enum keyaction ka;
        if ((event->keyval & 0xff00) == 0) 
                ka = keymap[event->keyval];
        else
                ka = ffkeymap[event->keyval & 0x00ff];

	if (event->keyval == GDK_Control_R ||
		event->keyval == GDK_Control_L) {
		control_key_pressed = 1;
		return TRUE;
	}

#if 0
	printf("event->keyval = 0x%08x\n", event->keyval);
#endif

        switch (ka) {
        case keyfullscreen: {
			if (fullscreen) {
				gtk_window_unfullscreen(GTK_WINDOW(window));
				fullscreen = 0;
				/* configure_event takes care of resizing drawing area, etc. */
			} else {
				gtk_window_fullscreen(GTK_WINDOW(window));
				fullscreen = 1;
				/* configure_event takes care of resizing drawing area, etc. */
			}
			return TRUE;
		}
	case keyquit:	in_the_process_of_quitting = !in_the_process_of_quitting;
			break;
	case keyleft:
		do_dirkey(-1, 0);
		break;
	case keyright:
		do_dirkey(1, 0);
		break;
	case keyup:
		do_dirkey(0, -1);
		break;
	case keydown:
		do_dirkey(0, 1);
		break;
	case keytorpedo:
		do_torpedo();
		break;
	case keyf1:
		if (displaymode >= DISPLAYMODE_FONTTEST)
			break;
		if (role & ROLE_MAIN) {
			displaymode = DISPLAYMODE_MAINSCREEN;
			wwviaudio_add_sound(CHANGESCREEN_SOUND);
		}
		break;
	case keyf2:
		if (displaymode >= DISPLAYMODE_FONTTEST)
			break;
		if (role & ROLE_NAVIGATION) {
			displaymode = DISPLAYMODE_NAVIGATION;
			wwviaudio_add_sound(CHANGESCREEN_SOUND);
		}
		break;
	case keyf3:
		if (displaymode >= DISPLAYMODE_FONTTEST)
			break;
		if (role & ROLE_WEAPONS) {
			displaymode = DISPLAYMODE_WEAPONS;
			wwviaudio_add_sound(CHANGESCREEN_SOUND);
		}
		break;
	case keyf4:
		if (displaymode >= DISPLAYMODE_FONTTEST)
			break;
		if (role & ROLE_ENGINEERING) {
			displaymode = DISPLAYMODE_ENGINEERING;
			wwviaudio_add_sound(CHANGESCREEN_SOUND);
		}
		break;
	case keyf5:
		if (displaymode >= DISPLAYMODE_FONTTEST)
			break;
		if (role & ROLE_SCIENCE) {
			displaymode = DISPLAYMODE_SCIENCE;
			wwviaudio_add_sound(CHANGESCREEN_SOUND);
		}
		break;
	case keyf6:
		if (displaymode >= DISPLAYMODE_FONTTEST)
			break;
		if (role & ROLE_COMMS) {
			displaymode = DISPLAYMODE_COMMS;
			wwviaudio_add_sound(CHANGESCREEN_SOUND);
		}
		break;
	case keyf7:
		if (displaymode >= DISPLAYMODE_FONTTEST)
			break;
		if (role & ROLE_DEBUG) {
			displaymode = DISPLAYMODE_DEBUG;
			wwviaudio_add_sound(CHANGESCREEN_SOUND);
		}
		break;
	case keyonscreen:
		if (control_key_pressed)
			do_onscreen((uint8_t) displaymode & 0xff);
		break;
	default:
		break;
	}
	return FALSE;
}

static gint key_release_cb(GtkWidget* widget, GdkEventKey* event, gpointer data)
{
	if (event->keyval == GDK_Control_R ||
		event->keyval == GDK_Control_L) {
		control_key_pressed = 0;
		return TRUE;
	}
	return FALSE;
}

/* keyboard handling stuff ends */
/**********************************/


static void show_fonttest(GtkWidget *w)
{
	sng_abs_xy_draw_string(w, gc, "A B C D E F G H I J K L M", SMALL_FONT, 30, 30); 
	sng_abs_xy_draw_string(w, gc, "N O P Q R S T U V W X Y Z", SMALL_FONT, 30, 60); 
	sng_abs_xy_draw_string(w, gc, "a b c d e f g h i j k l m", SMALL_FONT, 30, 90); 
	sng_abs_xy_draw_string(w, gc, "n o p q r s t u v w x y z", SMALL_FONT, 30, 120); 
	sng_abs_xy_draw_string(w, gc, "0 1 2 3 4 5 6 7 8 9 ! , .", SMALL_FONT, 30, 150); 
	sng_abs_xy_draw_string(w, gc, "? | - = * / \\ + ( ) \" ' _", SMALL_FONT, 30, 180); 

	sng_abs_xy_draw_string(w, gc, "The Quick Fox Jumps Over The Lazy Brown Dog.", SMALL_FONT, 30, 210); 
	sng_abs_xy_draw_string(w, gc, "The Quick Fox Jumps Over The Lazy Brown Dog.", BIG_FONT, 30, 280); 
	sng_abs_xy_draw_string(w, gc, "The Quick Fox Jumps Over The Lazy Brown Dog.", TINY_FONT, 30, 350); 
	sng_abs_xy_draw_string(w, gc, "The quick fox jumps over the lazy brown dog.", NANO_FONT, 30, 380); 
	sng_abs_xy_draw_string(w, gc, "THE QUICK FOX JUMPS OVER THE LAZY BROWN DOG.", TINY_FONT, 30, 410); 
	sng_abs_xy_draw_string(w, gc, "Well now, what have we here?  James Bond!", NANO_FONT, 30, 425); 
	sng_abs_xy_draw_string(w, gc, "The quick fox jumps over the lazy brown dog.", SMALL_FONT, 30, 450); 
	sng_abs_xy_draw_string(w, gc, "Copyright (C) 2010 Stephen M. Cameron 0123456789", TINY_FONT, 30, 480); 
}

static void show_introscreen(GtkWidget *w)
{
	sng_abs_xy_draw_string(w, gc, "Space Nerds", BIG_FONT, 80, 200); 
	sng_abs_xy_draw_string(w, gc, "In Space", BIG_FONT, 180, 320); 
	sng_abs_xy_draw_string(w, gc, "Copyright (C) 2010 Stephen M. Cameron", NANO_FONT, 255, 550); 
}

int lobbylast1clickx = -1;
int lobbylast1clicky = -1;
int lobby_selected_server = -1;
static void show_lobbyscreen(GtkWidget *w)
{
	char msg[100];
	int i;
#define STARTLINE 100
#define LINEHEIGHT 30

	sng_set_foreground(WHITE);
	if (lobby_socket == -1) {
		sng_abs_xy_draw_string(w, gc, "Space Nerds", BIG_FONT, 80, 200); 
		sng_abs_xy_draw_string(w, gc, "In Space", BIG_FONT, 180, 320); 
		sng_abs_xy_draw_string(w, gc, "Copyright (C) 2010 Stephen M. Cameron", NANO_FONT, 255, 550); 
		sprintf(msg, "Connecting to lobby... tried %d times.",
			lobby_count);
		sng_abs_xy_draw_string(w, gc, msg, SMALL_FONT, 100, 400);
		sng_abs_xy_draw_string(w, gc, lobbyerror, NANO_FONT, 100, 430);
	} else {
		if (lobby_selected_server != -1 &&
			lobbylast1clickx > 200 && lobbylast1clickx < 620 &&
			lobbylast1clicky > 520 && lobbylast1clicky < 520 + LINEHEIGHT * 2) {
			displaymode = DISPLAYMODE_CONNECTING;
			return;
		}
		lobby_selected_server = -1;
		sprintf(msg, "Connected to lobby on socket %d\n", lobby_socket);
		sng_abs_xy_draw_string(w, gc, msg, TINY_FONT, 30, LINEHEIGHT);
		sprintf(msg, "Total game servers: %d\n", ngameservers);
		sng_abs_xy_draw_string(w, gc, msg, TINY_FONT, 30, LINEHEIGHT + 20);
		for (i = 0; i < ngameservers; i++) {
			unsigned char *x = (unsigned char *) 
				&lobby_game_server[i].ipaddr;
			if (lobbylast1clickx > 30 && lobbylast1clickx < 700 &&
				lobbylast1clicky > 100 + (-0.5 + i) * LINEHEIGHT &&
				lobbylast1clicky < 100 + (0.5 + i) * LINEHEIGHT) {
				lobby_selected_server = i;
				sng_set_foreground(GREEN);
				snis_draw_rectangle(w->window, gc, 0, 25, 100 + (-0.5 + i) * LINEHEIGHT,
					725, LINEHEIGHT);
			} else
				sng_set_foreground(WHITE);
			 
			sprintf(msg, "%hu.%hu.%hu.%hu/%hu", x[0], x[1], x[2], x[3], lobby_game_server[i].port);
			sng_abs_xy_draw_string(w, gc, msg, TINY_FONT, 30, 100 + i * LINEHEIGHT);
			sprintf(msg, "%s", lobby_game_server[i].game_instance);
			sng_abs_xy_draw_string(w, gc, msg, TINY_FONT, 350, 100 + i * LINEHEIGHT);
			sprintf(msg, "%s", lobby_game_server[i].server_nickname);
			sng_abs_xy_draw_string(w, gc, msg, TINY_FONT, 450, 100 + i * LINEHEIGHT);
			sprintf(msg, "%s", lobby_game_server[i].location);
			sng_abs_xy_draw_string(w, gc, msg, TINY_FONT, 650, 100 + i * LINEHEIGHT);
		}
		if (lobby_selected_server != -1) {
			sng_set_foreground(WHITE);
			snis_draw_rectangle(w->window, gc, 0, 200, 520, 400, LINEHEIGHT * 2);
			sng_abs_xy_draw_string(w, gc, "CONNECT TO SERVER", TINY_FONT, 280, 520 + LINEHEIGHT);
		}
	}
}

static int process_update_ship_packet(uint16_t opcode)
{
	unsigned char buffer[100];
	struct packed_buffer pb;
	uint32_t id, alive, torpedoes, power;
	uint32_t fuel;
	double dx, dy, dheading, dgheading, dsheading, dbeamwidth, dvx, dvy;
	int rc;
	int shiptype = opcode == OPCODE_UPDATE_SHIP ? OBJTYPE_SHIP1 : OBJTYPE_SHIP2;
	uint8_t tloading, tloaded, throttle, rpm, temp, scizoom, warpdrive, requested_warpdrive,
		requested_shield, phaser_charge, phaser_wavelength;
	struct power_dist pd;

	assert(sizeof(buffer) > sizeof(struct update_ship_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct update_ship_packet) - sizeof(uint16_t));
	/* printf("process_update_ship_packet, snis_readsocket returned %d\n", rc); */
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "wwSSSS", &id, &alive,
				&dx, (int32_t) UNIVERSE_DIM, &dy, (int32_t) UNIVERSE_DIM,
				&dvx, (int32_t) UNIVERSE_DIM, &dvy, (int32_t) UNIVERSE_DIM);
	packed_buffer_extract(&pb, "UwwUUU", &dheading, (uint32_t) 360,
				&torpedoes, &power, &dgheading, (uint32_t) 360,
				&dsheading, (uint32_t) 360, &dbeamwidth, (uint32_t) 360);
	packed_buffer_extract(&pb, "bbbwbrbbbbbb", &tloading, &throttle, &rpm, &fuel, &temp,
			&pd, (unsigned short) sizeof(pd), &scizoom, &warpdrive, &requested_warpdrive,
			&requested_shield, &phaser_charge, &phaser_wavelength);
	tloaded = (tloading >> 4) & 0x0f;
	tloading = tloading & 0x0f;
	pthread_mutex_lock(&universe_mutex);
	rc = update_ship(id, dx, dy, dvx, dvy, dheading, alive, torpedoes, power,
				dgheading, dsheading, dbeamwidth, shiptype,
				tloading, tloaded, throttle, rpm, fuel, temp, &pd, scizoom,
				warpdrive, requested_warpdrive, requested_shield,
				phaser_charge, phaser_wavelength);
	pthread_mutex_unlock(&universe_mutex);
	return (rc < 0);
} 

static int process_update_econ_ship_packet(uint16_t opcode)
{
	unsigned char buffer[100];
	struct packed_buffer pb;
	uint32_t id, alive;
	double dx, dy, dheading, dv, dvx, dvy;
	int rc;

	assert(sizeof(buffer) > sizeof(struct update_econ_ship_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct update_econ_ship_packet) - sizeof(uint16_t));
	/* printf("process_update_econ_ship_packet, snis_readsocket returned %d\n", rc); */
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "wwSSUU", &id, &alive,
				&dx, (int32_t) UNIVERSE_DIM, &dy, (int32_t) UNIVERSE_DIM, 
				&dv, (uint32_t) UNIVERSE_DIM, &dheading, (uint32_t) 360);
	dvx = sin(dheading) * dv;
	dvy = -cos(dheading) * dv;
	pthread_mutex_lock(&universe_mutex);
	rc = update_econ_ship(id, dx, dy, dvx, dvy, dheading, alive);
	pthread_mutex_unlock(&universe_mutex);
	return (rc < 0);
} 

static int process_update_torpedo_packet(void)
{
	unsigned char buffer[100];
	struct packed_buffer pb;
	uint32_t id, ship_id;
	double dx, dy, dvx, dvy;
	int rc;

	assert(sizeof(buffer) > sizeof(struct update_torpedo_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct update_torpedo_packet) -
					sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "wwSSSS", &id, &ship_id,
				&dx, (int32_t) UNIVERSE_DIM, &dy, (int32_t) UNIVERSE_DIM,
				&dvx, (int32_t) UNIVERSE_DIM, &dvy, (int32_t) UNIVERSE_DIM);
	pthread_mutex_lock(&universe_mutex);
	rc = update_torpedo(id, dx, dy, dvx, dvy, ship_id);
	pthread_mutex_unlock(&universe_mutex);
	return (rc < 0);
} 

static int process_update_laser_packet(void)
{
	unsigned char buffer[100];
	struct packed_buffer pb;
	uint32_t id, ship_id;
	double dx, dy, dvx, dvy;
	int rc;

	assert(sizeof(buffer) > sizeof(struct update_laser_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct update_laser_packet) -
					sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "wwSSSS", &id, &ship_id,
				&dx, (int32_t) UNIVERSE_DIM, &dy, (int32_t) UNIVERSE_DIM,
				&dvx, (int32_t) UNIVERSE_DIM, &dvy, (int32_t) UNIVERSE_DIM);
	pthread_mutex_lock(&universe_mutex);
	rc = update_laser(id, dx, dy, dvx, dvy, ship_id);
	pthread_mutex_unlock(&universe_mutex);
	return (rc < 0);
} 

static void delete_object(uint32_t id)
{
	int i;

	i = lookup_object_by_id(id);
	/* perhaps we just joined and so don't know about this object. */
	if (i < 0)
		return;
	go[i].alive = 0;
	snis_object_pool_free_object(pool, i);
}

static int process_delete_object_packet(void)
{
	unsigned char buffer[10];
	struct packed_buffer pb;
	uint32_t id;
	int rc;

	assert(sizeof(buffer) > sizeof(struct delete_object_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct delete_object_packet) -
					sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	id = packed_buffer_extract_u32(&pb);
	pthread_mutex_lock(&universe_mutex);
	delete_object(id);
	pthread_mutex_unlock(&universe_mutex);
	return 0;
}

static int process_play_sound_packet(void)
{
	unsigned char buffer[10];
	struct packed_buffer pb;
	uint16_t sound_number;
	int rc;

	assert(sizeof(buffer) > sizeof(struct play_sound_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct play_sound_packet) -
					sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	sound_number = packed_buffer_extract_u16(&pb);
	wwviaudio_add_sound(sound_number);
	return 0;
}

static int process_ship_sdata_packet(void)
{
	unsigned char buffer[50];
	struct packed_buffer pb;
	uint32_t id;
	uint8_t subclass, shstrength, shwavelength, shwidth, shdepth;
	int rc;
	char name[NAMESIZE];

	assert(sizeof(buffer) > sizeof(struct ship_sdata_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct ship_sdata_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "wbbbbbr",&id, &subclass, &shstrength, &shwavelength,
			&shwidth, &shdepth, name, (unsigned short) sizeof(name));
	pthread_mutex_lock(&universe_mutex);
	update_ship_sdata(id, subclass, name, shstrength, shwavelength, shwidth, shdepth);
	pthread_mutex_unlock(&universe_mutex);
	return 0;
}

static int process_role_onscreen_packet(void)
{
	char buffer[sizeof(struct role_onscreen_packet)];
	struct packed_buffer pb;
	uint8_t new_displaymode;
	int rc;

	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct role_onscreen_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	new_displaymode = packed_buffer_extract_u8(&pb);

	if (!(role & ROLE_MAIN)) {
		/*
		 * Should "never" happen, server should only send display mode
		 * switching packets to clients that have role MAINSCREEN.
		 */
		printf("Unexpectedly got displaymode packet on "
			"client without mainscreen role.\n");
		return 0;
	}

	if (displaymode == new_displaymode) {
		wwviaudio_add_sound(OFFSCREEN_SOUND);
		displaymode = DISPLAYMODE_MAINSCREEN;
		return 0;
	}
	switch (new_displaymode) {
	case DISPLAYMODE_MAINSCREEN:
	case DISPLAYMODE_NAVIGATION:
	case DISPLAYMODE_WEAPONS:
	case DISPLAYMODE_ENGINEERING:
	case DISPLAYMODE_SCIENCE:
	case DISPLAYMODE_COMMS:
	case DISPLAYMODE_DEBUG:
		wwviaudio_add_sound(ONSCREEN_SOUND);
		displaymode = new_displaymode;
		break;
	default:
		printf("Got unexpected displaymode 0x%08x from server\n",
				new_displaymode);
		break;
	}
	return 0;
}

static struct snis_entity *curr_science_guy = NULL;
static struct snis_entity *prev_science_guy = NULL;
static int process_sci_select_target_packet(void)
{
	char buffer[sizeof(struct snis_sci_select_target_packet)];
	struct packed_buffer pb;
	uint32_t id;
	int rc, i;

	rc = snis_readsocket(gameserver_sock, buffer,
			sizeof(struct snis_sci_select_target_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	id = packed_buffer_extract_u32(&pb);
	i = lookup_object_by_id(id);
	if (i >= 0) {
		curr_science_guy = &go[i];
		wwviaudio_add_sound(SCIENCE_DATA_ACQUIRED_SOUND);
	}
	return 0;
}

static int process_sci_select_coords_packet(void)
{
	char buffer[sizeof(struct snis_sci_select_coords_packet)];
	struct packed_buffer pb;
	double ux, uy;
	int rc;

	rc = snis_readsocket(gameserver_sock, buffer,
			sizeof(struct snis_sci_select_coords_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	ux = packed_buffer_extract_ds32(&pb, UNIVERSE_DIM);
	uy = packed_buffer_extract_ds32(&pb, UNIVERSE_DIM);
	if (my_ship_oid == UNKNOWN_ID)
		return 0;
	go[my_ship_oid].sci_coordx = ux;	
	go[my_ship_oid].sci_coordy = uy;	
	return 0;
}

static int process_update_respawn_time(void)
{
	char buffer[sizeof(struct respawn_time_packet)];
	struct packed_buffer pb;
	int rc;
	uint8_t seconds;

	rc = snis_readsocket(gameserver_sock, buffer,
			sizeof(struct respawn_time_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	seconds = packed_buffer_extract_u8(&pb);
	if (my_ship_oid == UNKNOWN_ID) {
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
		if (my_ship_oid == UNKNOWN_ID)
			return 0;
	}
	go[my_ship_oid].respawn_time = (uint32_t) seconds;
	return 0;
}

static int process_update_netstats(void)
{
	char buffer[sizeof(struct netstats_packet)];
	struct packed_buffer pb;
	int rc;

	rc = snis_readsocket(gameserver_sock, buffer,
			sizeof(struct netstats_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "qqw", &netstats.bytes_sent,
				&netstats.bytes_recd, &netstats.elapsed_seconds);
	return 0;
}

struct comms_ui {
	struct text_window *tw;
	struct button *comms_onscreen_button;
	struct button *nav_onscreen_button;
	struct button *weap_onscreen_button;
	struct button *eng_onscreen_button;
	struct button *sci_onscreen_button;
	struct button *main_onscreen_button;
} comms_ui;

static int process_comm_transmission(void)
{
	char buffer[sizeof(struct comms_transmission_packet) + 100];
	struct packed_buffer pb;
	char string[256];
	uint8_t length;
	int rc;

	rc = snis_readsocket(gameserver_sock, buffer,
			sizeof(struct comms_transmission_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "b", &length);
	rc = snis_readsocket(gameserver_sock, string, length);
	string[79] = '\0';
	string[length] = '\0';
	text_window_add_text(comms_ui.tw, string);
	return 0;
}

static int process_ship_damage_packet(void)
{
	char buffer[sizeof(struct ship_damage_packet)];
	struct packed_buffer pb;
	uint32_t id;
	int rc, i;
	struct ship_damage_data damage;

	rc = snis_readsocket(gameserver_sock, buffer,
			sizeof(struct ship_damage_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	id = packed_buffer_extract_u32(&pb);
	i = lookup_object_by_id(id);
	
	if (i < 0)
		return -1;
	packed_buffer_extract_raw(&pb, (char *) &damage, sizeof(damage));
	pthread_mutex_lock(&universe_mutex);
	go[i].tsd.ship.damage = damage;
	pthread_mutex_unlock(&universe_mutex);
	return 0;
}

static int process_update_planet_packet(void)
{
	unsigned char buffer[100];
	struct packed_buffer pb;
	uint32_t id;
	double dx, dy;
	int rc;

	assert(sizeof(buffer) > sizeof(struct update_planet_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct update_planet_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "wSS", &id,
			&dx, (int32_t) UNIVERSE_DIM, &dy, (int32_t) UNIVERSE_DIM);
	pthread_mutex_lock(&universe_mutex);
	rc = update_planet(id, dx, dy);
	pthread_mutex_unlock(&universe_mutex);
	return (rc < 0);
} 

static int process_update_starbase_packet(void)
{
	unsigned char buffer[100];
	struct packed_buffer pb;
	uint32_t id;
	double dx, dy;
	int rc;

	assert(sizeof(buffer) > sizeof(struct update_starbase_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct update_starbase_packet) - sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "wSS", &id,
			&dx, (int32_t) UNIVERSE_DIM, &dy, (int32_t) UNIVERSE_DIM);
	pthread_mutex_lock(&universe_mutex);
	rc = update_starbase(id, dx, dy);
	pthread_mutex_unlock(&universe_mutex);
	return (rc < 0);
} 

static int process_update_explosion_packet(void)
{
	unsigned char buffer[sizeof(struct update_explosion_packet)];
	struct packed_buffer pb;
	uint32_t id;
	double dx, dy;
	uint16_t nsparks, velocity, time;
	int rc;

	assert(sizeof(buffer) > sizeof(struct update_explosion_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct update_explosion_packet)
				- sizeof(uint16_t));
	if (rc != 0)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	packed_buffer_extract(&pb, "wSShhh", &id,
		&dx, (int32_t) UNIVERSE_DIM, &dy, (int32_t) UNIVERSE_DIM,
		&nsparks, &velocity, &time);
	pthread_mutex_lock(&universe_mutex);
	rc = update_explosion(id, dx, dy, nsparks, velocity, time);
	pthread_mutex_unlock(&universe_mutex);
	return (rc < 0);
}

static int process_client_id_packet(void)
{
	unsigned char buffer[100];
	struct packed_buffer pb;
	uint32_t id;
	int rc;

	assert(sizeof(buffer) > sizeof(struct client_ship_id_packet) - sizeof(uint16_t));
	rc = snis_readsocket(gameserver_sock, buffer, sizeof(struct client_ship_id_packet) - sizeof(uint16_t));
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	id = packed_buffer_extract_u32(&pb);
	if (rc)
		return rc;
	my_ship_id = id;
	printf("SET MY SHIP ID to %u\n", my_ship_id);
	return 0;
}

static void *gameserver_reader(__attribute__((unused)) void *arg)
{
	uint16_t opcode;
	int rc;

	printf("gameserver reader thread\n");
	while (1) {
		/* printf("Client reading from game server %d bytes...\n", sizeof(opcode)); */
		rc = snis_readsocket(gameserver_sock, &opcode, sizeof(opcode));
		/* printf("rc = %d, errno  %s\n", rc, strerror(errno)); */
		if (rc != 0)
			goto protocol_error;
		opcode = ntohs(opcode);
		/* printf("got opcode %hd\n", opcode); */
		switch (opcode)	{
		case OPCODE_UPDATE_SHIP:
		case OPCODE_UPDATE_SHIP2:
			/* printf("processing update ship...\n"); */
			rc = process_update_ship_packet(opcode);
			if (rc != 0)
				goto protocol_error;
			break;
		case OPCODE_ECON_UPDATE_SHIP:
			rc = process_update_econ_ship_packet(opcode);
			if (rc != 0)
				goto protocol_error;
			break;
		case OPCODE_ID_CLIENT_SHIP:
			rc = process_client_id_packet();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_PLANET:
			rc = process_update_planet_packet();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_STARBASE:
			rc = process_update_starbase_packet();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_EXPLOSION:
			rc = process_update_explosion_packet();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_LASER:
			rc = process_update_laser_packet();
			if (rc != 0)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_TORPEDO:
			/* printf("processing update ship...\n"); */
			rc = process_update_torpedo_packet();
			if (rc != 0)
				goto protocol_error;
			break;
		case OPCODE_DELETE_OBJECT:
			rc = process_delete_object_packet();
			if (rc != 0)
				goto protocol_error;
			break;
		case OPCODE_PLAY_SOUND:
			rc = process_play_sound_packet();
			if (rc != 0)
				goto protocol_error;
			break;
		case OPCODE_SHIP_SDATA:
			rc = process_ship_sdata_packet();
			if (rc != 0)
				goto protocol_error; 
			break;
		case OPCODE_ROLE_ONSCREEN:
			rc = process_role_onscreen_packet();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_SCI_SELECT_TARGET:
			rc = process_sci_select_target_packet();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_SCI_SELECT_COORDS:
			rc = process_sci_select_coords_packet();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_DAMAGE:
			rc = process_ship_damage_packet();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_RESPAWN_TIME:
			rc = process_update_respawn_time();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_PLAYER:
			break;
		case OPCODE_ACK_PLAYER:
			break;
		case OPCODE_POS_SHIP:
			break;
		case OPCODE_POS_STARBASE:
			break;
		case OPCODE_POS_LASER:
			break;
		case OPCODE_NOOP:
			break;
		case OPCODE_COMMS_TRANSMISSION:
			rc = process_comm_transmission();
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_UPDATE_NETSTATS:
			rc = process_update_netstats();
			if (rc)
				goto protocol_error;
			break;
		default:
			goto protocol_error;
		}
	}

protocol_error:
	printf("Protocol error in gameserver reader, opcode = %hu\n", opcode);
	close(gameserver_sock);
	gameserver_sock = -1;
	return NULL;
}

static void wait_for_serverbound_packets(void)
{
	int rc;

	pthread_mutex_lock(&to_server_queue_event_mutex);
	while (!have_packets_for_server) {
		rc = pthread_cond_wait(&server_write_cond,
			&to_server_queue_event_mutex);
		if (rc != 0)
			printf("gameserver_writer: pthread_cond_wait failed.\n");
		if (have_packets_for_server)
			break;
	}
	pthread_mutex_unlock(&to_server_queue_event_mutex);
}

static void write_queued_packets_to_server(void)
{
	struct packed_buffer *buffer;
	int rc;

	pthread_mutex_lock(&to_server_queue_event_mutex);
	buffer = packed_buffer_queue_combine(&to_server_queue, &to_server_queue_mutex);
	if (buffer->buffer_size > 0) {
		rc = snis_writesocket(gameserver_sock, buffer->buffer, buffer->buffer_size);
		if (rc) {
			printf("Failed to write to gameserver\n");
			goto badserver;
		}
	} else {
		printf("Hmm, gameserver_writer awakened, but nothing to write.\n");
	}
	packed_buffer_free(buffer);
	if (have_packets_for_server)
		have_packets_for_server = 0;
	pthread_mutex_unlock(&to_server_queue_event_mutex);
	return;

badserver:
	/* Hmm, we are pretty hosed here... we have a combined packed buffer
	 * to write, but server is disconnected... no place to put our buffer.
	 * What happens next is probably "nothing good."
	 */
	printf("client baling on server...\n");
	packed_buffer_free(buffer);
	shutdown(gameserver_sock, SHUT_RDWR);
	close(gameserver_sock);
	gameserver_sock = -1;
}

static void wakeup_gameserver_writer(void)
{
	int rc;

        pthread_mutex_lock(&to_server_queue_event_mutex);
	have_packets_for_server = 1;
	rc = pthread_cond_broadcast(&server_write_cond);
	if (rc)
		printf("huh... pthread_cond_broadcast failed.\n");
	pthread_mutex_unlock(&to_server_queue_event_mutex);
}

static void *gameserver_writer(__attribute__((unused)) void *arg)
{

        pthread_mutex_init(&to_server_queue_mutex, NULL);
        pthread_mutex_init(&to_server_queue_event_mutex, NULL);
        packed_buffer_queue_init(&to_server_queue);

	while (1) {
		wait_for_serverbound_packets();
		write_queued_packets_to_server();
	}
	return NULL;
}

int role_to_displaymode(uint32_t role)
{
	int displaymode, j;

	displaymode = DISPLAYMODE_MAINSCREEN;
	for (j = 0; j < 32; j++) {
		if ((1 << j) & role) {
			switch ((1 << j)) {
			case ROLE_MAIN:
				return DISPLAYMODE_MAINSCREEN;
			case ROLE_NAVIGATION:
				return DISPLAYMODE_NAVIGATION;
			case ROLE_WEAPONS:
				return DISPLAYMODE_WEAPONS;
			case ROLE_ENGINEERING:
				return DISPLAYMODE_ENGINEERING;
			case ROLE_SCIENCE:
				return DISPLAYMODE_SCIENCE;
			case ROLE_COMMS:
				return DISPLAYMODE_COMMS;
			case ROLE_DEBUG:
				return DISPLAYMODE_DEBUG;
			default:
				break;
			}
		}
	}
	return displaymode;
}

static void *connect_to_gameserver_thread(__attribute__((unused)) void *arg)
{
	int rc;
	struct addrinfo *gameserverinfo, *i;
	struct addrinfo hints;
#if 0
	void *addr;
	char *ipver; 
#endif
	char portstr[50];
	char hoststr[50];
	unsigned char *x = (unsigned char *) &lobby_game_server[lobby_selected_server].ipaddr;
	struct add_player_packet app;
	int flag = 1;

	sprintf(portstr, "%d", ntohs(lobby_game_server[lobby_selected_server].port));
	sprintf(hoststr, "%d.%d.%d.%d", x[0], x[1], x[2], x[3]);
	printf("connecting to %s/%s\n", hoststr, portstr);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_NUMERICHOST;
	rc = getaddrinfo(hoststr, portstr, &hints, &gameserverinfo);
	if (rc)
		goto error;

	for (i = gameserverinfo; i != NULL; i = i->ai_next) {
		if (i->ai_family == AF_INET) {
#if 0
			struct sockaddr_in *ipv4 = (struct sockaddr_in *)i->ai_addr;
			addr = &(ipv4->sin_addr);
			ipver = "IPv4";
#endif
			break;
		}
	}
	if (i == NULL)
		goto error;

	gameserver_sock = socket(AF_INET, SOCK_STREAM, i->ai_protocol); 
	if (gameserver_sock < 0)
		goto error;

	rc = connect(gameserver_sock, i->ai_addr, i->ai_addrlen);
	if (rc < 0)
		goto error;

	rc = setsockopt(gameserver_sock, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
	if (rc)
		fprintf(stderr, "setsockopt(TCP_NODELAY) failed.\n");

	rc = snis_writesocket(gameserver_sock, SNIS_PROTOCOL_VERSION, strlen(SNIS_PROTOCOL_VERSION));
	if (rc < 0)
		goto error;;

	displaymode = DISPLAYMODE_CONNECTED;
	done_with_lobby = 1;
	displaymode = role_to_displaymode(role);

	/* Should probably submit this through the packed buffer queue...
	 * but, this works.
	 */
	memset(&app, 0, sizeof(app));
	app.opcode = htons(OPCODE_UPDATE_PLAYER);
	app.role = htonl(role);
	strncpy((char *) app.shipname, shipname, 19);
	strncpy((char *) app.password, password, 19);

	printf("Notifying server, opcode update player\n");
	snis_writesocket(gameserver_sock, &app, sizeof(app));
	printf("Wrote update player opcode\n");

        pthread_attr_init(&gameserver_reader_attr);
        pthread_attr_init(&gameserver_writer_attr);
        pthread_attr_setdetachstate(&gameserver_reader_attr, PTHREAD_CREATE_JOINABLE);
        pthread_attr_setdetachstate(&gameserver_writer_attr, PTHREAD_CREATE_JOINABLE);
	printf("starting gameserver reader thread\n");
	rc = pthread_create(&read_from_gameserver_thread, &gameserver_reader_attr, gameserver_reader, NULL);
	printf("started gameserver reader thread\n");
	printf("starting gameserver writer thread\n");
	rc = pthread_create(&write_to_gameserver_thread, &gameserver_writer_attr, gameserver_writer, NULL);
	printf("started gameserver writer thread\n");

error:
	/* FIXME, this isn't right... */
	freeaddrinfo(gameserverinfo);
	return NULL;
}

void connect_to_gameserver(int selected_server)
{
        pthread_attr_init(&gameserver_connect_attr);
        pthread_attr_setdetachstate(&gameserver_connect_attr, PTHREAD_CREATE_JOINABLE);
	(void) pthread_create(&gameserver_connect_thread, &gameserver_connect_attr, connect_to_gameserver_thread, NULL);
	return;
}

static void show_connecting_screen(GtkWidget *w)
{
	static int connected_to_gameserver = 0;
	sng_set_foreground(WHITE);
	sng_abs_xy_draw_string(w, gc, "CONNECTING TO SERVER...", SMALL_FONT, 100, 300 + LINEHEIGHT);
	if (!connected_to_gameserver) {
		connected_to_gameserver = 1;
		connect_to_gameserver(lobby_selected_server);
	}
}

static void show_connected_screen(GtkWidget *w)
{
	sng_set_foreground(WHITE);
	sng_abs_xy_draw_string(w, gc, "CONNECTED TO SERVER", SMALL_FONT, 100, 300 + LINEHEIGHT);
	sng_abs_xy_draw_string(w, gc, "DOWNLOADING GAME DATA", SMALL_FONT, 100, 300 + LINEHEIGHT * 3);
}

static void show_common_screen(GtkWidget *w, char *title)
{
	sng_set_foreground(GREEN);
	sng_abs_xy_draw_string(w, gc, title, SMALL_FONT, 25, 10 + LINEHEIGHT);
	sng_set_foreground(BLUE);
	snis_draw_line(w->window, gc, 0, 0, SCREEN_WIDTH, 0);
	snis_draw_line(w->window, gc, SCREEN_WIDTH, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	snis_draw_line(w->window, gc, SCREEN_WIDTH, SCREEN_HEIGHT, 0, SCREEN_HEIGHT);
	snis_draw_line(w->window, gc, 0, 0, 0, SCREEN_HEIGHT);
}

static void show_mainscreen(GtkWidget *w)
{
	show_common_screen(w, "Main Screen");	
}

static void snis_draw_torpedo(GdkDrawable *drawable, GdkGC *gc, gint x, gint y, gint r)
{
	int i, dx, dy;

	sng_set_foreground(WHITE);
	for (i = 0; i < 10; i++) {
		dx = x + snis_randn(r * 2) - r; 
		dy = y + snis_randn(r * 2) - r; 
		snis_draw_line(drawable, gc, x, y, dx, dy);
	}
	/* sng_draw_circle(drawable, gc, x, y, (int) (SCREEN_WIDTH * 150.0 / XKNOWN_DIM)); */
}

static void snis_draw_laser(GdkDrawable *drawable, GdkGC *gc, gint x1, gint y1, gint x2, gint y2)
{
	sng_current_bright_line(drawable, gc, x1, y1, x2, y2, RED);
}

/* position and dimensions of science scope */
#define SCIENCE_SCOPE_X 20
#define SCIENCE_SCOPE_Y 70 
#define SCIENCE_SCOPE_W 500
#define SCIENCE_SCOPE_H SCIENCE_SCOPE_W
#define SCIENCE_SCOPE_R (SCIENCE_SCOPE_H / 2)
#define SCIENCE_SCOPE_CX (SCIENCE_SCOPE_X + SCIENCE_SCOPE_R)
#define SCIENCE_SCOPE_CY (SCIENCE_SCOPE_Y + SCIENCE_SCOPE_R)

static void snis_draw_science_guy(GtkWidget *w, GdkGC *gc, struct snis_entity *o,
					gint x, gint y, double dist, int bw, int pwr,
					double range, int selected)
{
	int i;

	double da;
	int dr;
	double tx, ty;
	char buffer[50];
	int divisor;


	/* Compute radius of ship blip */
	divisor = hypot((float) bw + 1, 256.0 - pwr);
	dr = (int) dist / (XKNOWN_DIM / divisor);
	dr = dr * MAX_SCIENCE_SCREEN_RADIUS / range;

	/* if dr is small enough, and ship info is not known, nor recently requested,
	 * then issue OPCODE REQUEST_SHIP_SDATA to server somehow.
	 */
	if (dr < 5 && !o->sdata.science_data_known && !o->sdata.science_data_requested)
		request_ship_sdata(o);

	sng_set_foreground(GREEN);
	for (i = 0; i < 10; i++) {
		float r;
		da = snis_randn(360) * M_PI / 180.0;
#if 1
		tx = (int) ((double) x + sin(da) * (double) snis_randn(dr));
		ty = (int) ((double) y + cos(da) * (double) snis_randn(dr)); 

		r = hypot(tx - SCIENCE_SCOPE_CX, ty - SCIENCE_SCOPE_CY);
		if (r >= SCIENCE_SCOPE_R)
			continue;
#else
		tx = x;
		ty = y;
#endif
		sng_draw_point(w->window, gc, tx, ty);
	}

	if (selected)
		sng_draw_circle(w->window, gc, x, y, 10);
	
	if (o->sdata.science_data_known) {
		switch (o->type) {
		case OBJTYPE_SHIP2:
			sng_set_foreground(GREEN);
			sprintf(buffer, "%s %s\n", o->sdata.name, shipclass[o->sdata.subclass]); 
			break;
		case OBJTYPE_STARBASE:
			sng_set_foreground(WHITE);
			sprintf(buffer, "%s %s\n", "Starbase",  o->sdata.name); 
			break;
		case OBJTYPE_PLANET:
			sng_set_foreground(BLUE);
			sprintf(buffer, "%s %s\n", "Asteroid",  o->sdata.name); 
			break;
		case OBJTYPE_TORPEDO:
			sng_set_foreground(GREEN);
			strcpy(buffer, "TORPEDO");
			break;
		case OBJTYPE_LASER:
			sng_set_foreground(GREEN);
			strcpy(buffer, "ENERGY");
			break;
		default:
			sng_set_foreground(GREEN);
			sprintf(buffer, "%s %s\n", "Unknown", o->sdata.name); 
			break;
		}
		sng_abs_xy_draw_string(w, gc, buffer, NANO_FONT, x + 8, y - 8);
	}
}

static void snis_draw_science_spark(GdkDrawable *drawable, GdkGC *gc, gint x, gint y, double dist)
{
	int i;

	double da;
	int dr;
	double tx, ty;

	dr = (int) dist / (XKNOWN_DIM / 100.0);
	for (i = 0; i < 20; i++) {
		da = snis_randn(360) * M_PI / 180.0;
#if 1
		tx = (int) ((double) x + sin(da) * (double) snis_randn(dr));
		ty = (int) ((double) y + cos(da) * (double) snis_randn(dr)); 
#else
		tx = x;
		ty = y;
#endif
		sng_draw_point(drawable, gc, tx, ty);
	}
}

static void snis_draw_arrow(GtkWidget *w, GdkGC *gc, gint x, gint y, gint r,
		double heading, double scale)
{
	int nx, ny, tx1, ty1, tx2, ty2;

	/* Draw ship... */
#define SHIP_SCALE_DOWN 15.0
	nx = sin(heading) * scale * r / SHIP_SCALE_DOWN;
	ny = -cos(heading) * scale * r / SHIP_SCALE_DOWN;
	snis_draw_line(w->window, gc, x, y, x + nx, y + ny);
	tx1 = sin(heading + PI / 2.0) * scale * r / (SHIP_SCALE_DOWN * 2.0) - nx / 2.0;
	ty1 = -cos(heading + PI / 2.0) * scale * r / (SHIP_SCALE_DOWN * 2.0) - ny / 2.0;
	snis_draw_line(w->window, gc, x, y, x + tx1, y + ty1);
	tx2 = sin(heading - PI / 2.0) * scale * r / (SHIP_SCALE_DOWN * 2.0) - nx / 2.0;
	ty2 = -cos(heading - PI / 2.0) * scale * r / (SHIP_SCALE_DOWN * 2.0) - ny / 2.0;
	snis_draw_line(w->window, gc, x, y, x + tx2, y + ty2);
	snis_draw_line(w->window, gc, x + nx, y + ny, x + tx1, y + ty1);
	snis_draw_line(w->window, gc, x + tx1, y + ty1, x + tx2, y + ty2);
	snis_draw_line(w->window, gc, x + tx2, y + ty2, x + nx, y + ny);
}

static void snis_draw_science_reticule(GtkWidget *w, GdkGC *gc, gint x, gint y, gint r,
		double heading, double beam_width)
{
	int i;
	// int nx, ny, 
	int tx1, ty1, tx2, ty2;

	sng_draw_circle(w->window, gc, x, y, r);

	for (i = 0; i < 36; i++) { /* 10 degree increments */
		int x1 = (int) (cos((10.0 * i) * 3.1415927 / 180.0) * r);
		int y1 = (int) (sin((10.0 * i) * 3.1415927 / 180.0) * r);
		int x2 = x1 * 0.25;
		int y2 = y1 * 0.25;
		x1 += x;
		x2 += x;
		y1 += y;
		y2 += y;
		sng_draw_dotted_line(w->window, gc, x1, y1, x2, y2);
	}

	/* draw the ship */
	snis_draw_arrow(w, gc, x, y, r, heading, 1.0);
	
	tx1 = x + sin(heading - beam_width / 2) * r * 0.05;
	ty1 = y - cos(heading - beam_width / 2) * r * 0.05;
	tx2 = x + sin(heading - beam_width / 2) * r;
	ty2 = y - cos(heading - beam_width / 2) * r;
	sng_set_foreground(GREEN);
	sng_draw_electric_line(w->window, gc, tx1, ty1, tx2, ty2);
	tx1 = x + sin(heading + beam_width / 2) * r * 0.05;
	ty1 = y - cos(heading + beam_width / 2) * r * 0.05;
	tx2 = x + sin(heading + beam_width / 2) * r;
	ty2 = y - cos(heading + beam_width / 2) * r;
	sng_draw_electric_line(w->window, gc, tx1, ty1, tx2, ty2);
}

static void snis_draw_reticule(GtkWidget *w, GdkGC *gc, gint x, gint y, gint r,
		double heading)
{
	int i;
	// int nx, ny, 
	int tx1, ty1, tx2, ty2;
	char buf[10];

	for (i = r; i > r / 4; i -= r / 5)
		sng_draw_circle(w->window, gc, x, y, i);

	for (i = 0; i < 36; i++) { /* 10 degree increments */
		int x3, y3;
		int x1 = (int) (cos((10.0 * i) * M_PI / 180.0) * r);
		int y1 = (int) (sin((10.0 * i) * M_PI / 180.0) * r);
		int x2 = x1 * 0.25;
		int y2 = y1 * 0.25;
		x3 = x1 * 1.08 + x - 15;
		y3 = y1 * 1.08 + y;
		x1 += x;
		x2 += x;
		y1 += y;
		y2 += y;
		snis_draw_line(w->window, gc, x1, y1, x2, y2);
		sprintf(buf, "%3d", (90 + i * 10) % 360);
		sng_abs_xy_draw_string(w, gc, buf, NANO_FONT, x3, y3);
	}

	/* draw the ship */
	snis_draw_arrow(w, gc, x, y, r, heading, 1.0);
	
	tx1 = x + sin(heading) * r * 0.85;
	ty1 = y - cos(heading) * r * 0.85;
	tx2 = x + sin(heading) * r;
	ty2 = y - cos(heading) * r;
	sng_set_foreground(RED);
	snis_draw_line(w->window, gc, tx1, ty1, tx2, ty2);
}

static void draw_all_the_guys(GtkWidget *w, struct snis_entity *o)
{
	int i, cx, cy, r, rx, ry, rw, rh;
	char buffer[200];

	rx = 20;
	ry = 70;
	rw = 500;
	rh = 500;
	cx = rx + (rw / 2);
	cy = ry + (rh / 2);
	r = rh / 2;
	sng_set_foreground(DARKRED);
	/* Draw all the stuff */
#define NAVSCREEN_RADIUS (XKNOWN_DIM / 100.0)
#define NR2 (NAVSCREEN_RADIUS * NAVSCREEN_RADIUS)
	pthread_mutex_lock(&universe_mutex);

	for (i = 0; i <= snis_object_pool_highest_object(pool); i++) {
		int x, y;
		double tx, ty;
		double dist2;

		if (!go[i].alive)
			continue;

		dist2 = ((go[i].x - o->x) * (go[i].x - o->x)) +
			((go[i].y - o->y) * (go[i].y - o->y));
		if (dist2 > NR2)
			continue; /* not close enough */
	

		tx = (go[i].x - o->x) * (double) r / NAVSCREEN_RADIUS;
		ty = (go[i].y - o->y) * (double) r / NAVSCREEN_RADIUS;
		x = (int) (tx + (double) cx);
		y = (int) (ty + (double) cy);

		if (go[i].id == my_ship_id)
			continue; /* skip drawing yourself. */
		else {
			switch (go[i].type) {
			case OBJTYPE_PLANET:
				sng_set_foreground(BLUE);
				sng_draw_circle(w->window, gc, x, y, r / 10);
				break;
			case OBJTYPE_STARBASE:
				sng_set_foreground(MAGENTA);
				sng_draw_circle(w->window, gc, x, y, r / 20);
				break;
			case OBJTYPE_LASER:
				snis_draw_laser(w->window, gc, x, y,
					x - go[i].vx * (double) r / (2 * NAVSCREEN_RADIUS),
					y - go[i].vy * (double) r / (2 * NAVSCREEN_RADIUS));
				break;
			case OBJTYPE_TORPEDO:
				snis_draw_torpedo(w->window, gc, x, y, r / 25);
				break;
			case OBJTYPE_EXPLOSION:
				break;
			case OBJTYPE_SHIP1:
			case OBJTYPE_SHIP2:
				sng_set_foreground(WHITE);
				snis_draw_arrow(w, gc, x, y, r, go[i].heading + M_PI / 2.0, 0.5);
				sng_set_foreground(GREEN);
				if (go[i].sdata.science_data_known) {
					sprintf(buffer, "%s", go[i].sdata.name);
					sng_abs_xy_draw_string(w, gc, buffer, NANO_FONT, x + 10, y - 10);
				}
				break;
			default:
				sng_set_foreground(WHITE);
				snis_draw_arrow(w, gc, x, y, r, go[i].heading + M_PI / 2.0, 0.5);
			}
		}
	}
	pthread_mutex_unlock(&universe_mutex);
}

/* position and dimensions of science scope */
#define SCIENCE_SCOPE_X 20
#define SCIENCE_SCOPE_Y 70 
#define SCIENCE_SCOPE_W 500
#define SCIENCE_SCOPE_H SCIENCE_SCOPE_W
#define SCIENCE_SCOPE_R (SCIENCE_SCOPE_H / 2)
#define SCIENCE_SCOPE_CX (SCIENCE_SCOPE_X + SCIENCE_SCOPE_R)
#define SCIENCE_SCOPE_CY (SCIENCE_SCOPE_Y + SCIENCE_SCOPE_R)

/* this science_guy[] array is used for mouse clicking. */
struct science_data {
	int sx, sy; /* screen coords on scope */
	struct snis_entity *o;
};
static struct science_data science_guy[MAXGAMEOBJS] = { {0}, };
static int nscience_guys = 0;

static void draw_all_the_science_guys(GtkWidget *w, struct snis_entity *o, double range)
{
	int i, x, y, cx, cy, r, bw, pwr;
	double angle, angle2, A1, A2;
	double tx, ty, dist2, dist;
	int selected_guy_still_visible = 0;

	cx = SCIENCE_SCOPE_CX;
	cy = SCIENCE_SCOPE_CY;
	r = SCIENCE_SCOPE_R;
	pwr = 255.0 * ((float) o->tsd.ship.pwrdist.sensors / 255.0) / SENSORS_POWER_FACTOR *
				(float) o->tsd.ship.power / (float) UINT32_MAX;
	if (pwr > 255)
		pwr = 255;
	/* Draw all the stuff */

	/* Draw selected coordinate */
	dist = hypot(o->x - o->sci_coordx, o->y - o->sci_coordy);
	if (dist < range) {
		tx = (o->sci_coordx - o->x) * (double) r / range;
		ty = (o->sci_coordy - o->y) * (double) r / range;
		x = (int) (tx + (double) cx);
		y = (int) (ty + (double) cy);
		snis_draw_line(w->window, gc, x - 5, y, x + 5, y);
		snis_draw_line(w->window, gc, x, y - 5, x, y + 5);
	}

        tx = sin(o->tsd.ship.sci_heading) * range;
        ty = -cos(o->tsd.ship.sci_heading) * range;

	angle2 = atan2(ty, tx);
	A1 = angle2 - o->tsd.ship.sci_beam_width / 2.0;
	A2 = angle2 + o->tsd.ship.sci_beam_width / 2.0;
	if (A1 < -M_PI)
		A1 += 2.0 * M_PI;
	if (A2 > M_PI)
		A2 -= 2.0 * M_PI;
	sng_set_foreground(GREEN);
	pthread_mutex_lock(&universe_mutex);
	nscience_guys = 0;
	for (i = 0; i <= snis_object_pool_highest_object(pool); i++) {
		if (!go[i].alive)
			continue;

		dist2 = ((go[i].x - o->x) * (go[i].x - o->x)) +
			((go[i].y - o->y) * (go[i].y - o->y));
		if (dist2 > range * range)
			continue; /* not close enough */
		dist = sqrt(dist2);
	

		tx = (go[i].x - o->x) * (double) r / range;
		ty = (go[i].y - o->y) * (double) r / range;
		angle = atan2(ty, tx);

		if (!(A2 < 0 && A1 > 0 && fabs(A1) > M_PI / 2.0)) {
			if (angle < A1)
				continue;
			if (angle > A2)
				continue;
		} else {
			if (angle < 0 && angle > A2)
				continue;
			if (angle > 0 && angle < A1)
				continue;
		}
		x = (int) (tx + (double) cx);
		y = (int) (ty + (double) cy);

		if (go[i].id == my_ship_id)
			continue; /* skip drawing yourself. */
		bw = o->tsd.ship.sci_beam_width * 180.0 / M_PI;

		/* If we moved the beam off our guy, and back on, select him again. */
		if (!curr_science_guy && prev_science_guy == &go[i])
			curr_science_guy = prev_science_guy;

		snis_draw_science_guy(w, gc, &go[i], x, y, dist, bw, pwr, range, &go[i] == curr_science_guy);

		/* cache screen coords for mouse picking */
		science_guy[nscience_guys].o = &go[i];
		science_guy[nscience_guys].sx = x;
		science_guy[nscience_guys].sy = y;
		if (&go[i] == curr_science_guy)
			selected_guy_still_visible = 1;
		nscience_guys++;
	}
	if (!selected_guy_still_visible && curr_science_guy) {
		prev_science_guy = curr_science_guy;
		curr_science_guy = NULL;
	}
	pthread_mutex_unlock(&universe_mutex);
}

static void draw_all_the_science_sparks(GtkWidget *w, struct snis_entity *o, double range)
{
	int i, cx, cy, r, rx, ry, rw, rh;

	rx = 20;
	ry = 70;
	rw = 500;
	rh = 500;
	cx = rx + (rw / 2);
	cy = ry + (rh / 2);
	r = rh / 2;
	sng_set_foreground(DARKRED);
	/* Draw all the stuff */
	pthread_mutex_lock(&universe_mutex);

	for (i = 0; i <= snis_object_pool_highest_object(sparkpool); i++) {
		int x, y;
		double tx, ty;
		double dist2, dist;

		if (!spark[i].alive)
			continue;

		dist2 = ((spark[i].x - o->x) * (spark[i].x - o->x)) +
			((spark[i].y - o->y) * (spark[i].y - o->y));
		if (dist2 > range * range)
			continue; /* not close enough */
		dist = sqrt(dist2);
	

		tx = (spark[i].x - o->x) * (double) r / range;
		ty = (spark[i].y - o->y) * (double) r / range;
		x = (int) (tx + (double) cx);
		y = (int) (ty + (double) cy);

		sng_set_foreground(GREEN);
		snis_draw_science_spark(w->window, gc, x, y, dist);
	}
	pthread_mutex_unlock(&universe_mutex);
}


static void draw_all_the_sparks(GtkWidget *w, struct snis_entity *o)
{
	int i, cx, cy, r, rx, ry, rw, rh;

	rx = 20;
	ry = 70;
	rw = 500;
	rh = 500;
	cx = rx + (rw / 2);
	cy = ry + (rh / 2);
	r = rh / 2;
	sng_set_foreground(DARKRED);
	/* Draw all the stuff */
	pthread_mutex_lock(&universe_mutex);

	for (i = 0; i <= snis_object_pool_highest_object(sparkpool); i++) {
		int x, y;
		double tx, ty;
		double dist2;

		if (!spark[i].alive)
			continue;

		dist2 = ((spark[i].x - o->x) * (spark[i].x - o->x)) +
			((spark[i].y - o->y) * (spark[i].y - o->y));
		if (dist2 > NR2)
			continue; /* not close enough */

		tx = (spark[i].x - o->x) * (double) r / NAVSCREEN_RADIUS;
		ty = (spark[i].y - o->y) * (double) r / NAVSCREEN_RADIUS;
		x = (int) (tx + (double) cx);
		y = (int) (ty + (double) cy);

		sng_set_foreground(WHITE);
		snis_draw_line(w->window, gc, x - 1, y - 1, x + 1, y + 1);
		snis_draw_line(w->window, gc, x - 1, y + 1, x + 1, y - 1);
	}
	pthread_mutex_unlock(&universe_mutex);
}

static void snis_draw_dotted_hline(GdkDrawable *drawable,
         GdkGC *gc, int x1, int y1, int x2, int dots)
{
	int i;

	for (i = x1; i <= x2; i += dots)
		sng_draw_point(drawable, gc, i, y1);
}

static void snis_draw_dotted_vline(GdkDrawable *drawable,
         GdkGC *gc, int x1, int y1, int y2, int dots)
{
	int i;

	if (y2 > y1) {
		for (i = y1; i <= y2; i += dots)
			sng_draw_point(drawable, gc, x1, i);
	} else { 
		for (i = y2; i <= y1; i += dots)
			sng_draw_point(drawable, gc, x1, i);
	}
}

static void snis_draw_radar_sector_labels(GtkWidget *w,
         GdkGC *gc, struct snis_entity *o, int cx, int cy, int r, double range)
{
	/* FIXME, this algorithm is really fricken dumb. */
	int x, y;
	double xincrement = (XKNOWN_DIM / 10.0);
	double yincrement = (YKNOWN_DIM / 10.0);
	int x1, y1;
	const char *letters = "ABCDEFGHIJK";
	char label[10];
	int xoffset = 7;
	int yoffset = 10;

	for (x = 0; x < 10; x++) {
		if ((x * xincrement) <= (o->x - range * 0.9))
			continue;
		if ((x * xincrement) >= (o->x + range * 0.9))
			continue;
		for (y = 0; y < 10; y++) {
			if ((y * yincrement) <= (o->y - range * 0.9))
				continue;
			if ((y * yincrement) >= (o->y + range * 0.9))
				continue;
			x1 = (int) (((double) r) / range * (x * xincrement - o->x)) + cx + xoffset;
			y1 = (int) (((double) r) / range * (y * yincrement - o->y)) + cy + yoffset;
			snprintf(label, sizeof(label), "%c%d", letters[y], x);
			sng_abs_xy_draw_string(w, gc, label, NANO_FONT, x1, y1);
		}
	}
}

static void snis_draw_radar_grid(GdkDrawable *drawable,
         GdkGC *gc, struct snis_entity *o, int cx, int cy, int r, double range, int small_grids)
{
	/* FIXME, this algorithm is really fricken dumb. */
	int x, y;
	double increment = (XKNOWN_DIM / 10.0);
	double lx1, ly1, lx2, ly2;
	int x1, y1, x2, y2; 
	int xlow, xhigh, ylow, yhigh;
	double range2 = (range * range);

	xlow = (int) ((double) r / range * (0 -o->x)) + cx;
	xhigh = (int) ((double) r / range * (XKNOWN_DIM - o->x)) + cx;
	ylow = (int) (((double) r) / range * (0.0 - o->y)) + cy;
	yhigh = (int) (((double) r) / range * (YKNOWN_DIM - o->y)) + cy;
	/* vertical lines */
	increment = (XKNOWN_DIM / 10.0);
	for (x = 0; x <= 10; x++) {
		if ((x * increment) <= (o->x - range))
			continue;
		if ((x * increment) >= (o->x + range))
			continue;
		/* find y intersections with radar circle by pyth. theorem. */
		lx1 = x * increment - o->x;
		ly1 = sqrt((range2 - (lx1 * lx1)));
		ly2 = -ly1;

		x1 = (int) (((double) r) / range * lx1) + cx;
		y1 = (int) (((double) r) / range * ly1) + cy;
		y2 = (int) (((double) r) / range * ly2) + cy;

		if (y1 < ylow)
			y1 = ylow;
		if (y1 > yhigh)
			y1 = yhigh;
		if (y2 < ylow)
			y2 = ylow;
		if (y2 > yhigh)
			y2 = yhigh;
		snis_draw_dotted_vline(drawable, gc, x1, y2, y1, 5);
	}
	/* horizontal lines */
	increment = (YKNOWN_DIM / 10.0);
	for (y = 0; y <= 10; y++) {
		if ((y * increment) <= (o->y - range))
			continue;
		if ((y * increment) >= (o->y + range))
			continue;
		/* find x intersections with radar circle by pyth. theorem. */
		ly1 = y * increment - o->y;
		lx1 = sqrt((range2 - (ly1 * ly1)));
		lx2 = -lx1;
		y1 = (int) (((double) r) / range * ly1) + cy;
		x1 = (int) (((double) r) / range * lx1) + cx;
		x2 = (int) (((double) r) / range * lx2) + cx;
		if (x1 < xlow)
			x1 = xlow;
		if (x1 > xhigh)
			x1 = xhigh;
		if (x2 < xlow)
			x2 = xlow;
		if (x2 > xhigh)
			x2 = xhigh;
		snis_draw_dotted_hline(drawable, gc, x2, y1, x1, 5);
	}

	if (!small_grids)
		return;

	increment = (XKNOWN_DIM / 100.0);
	/* vertical lines */
	for (x = 0; x <= 100; x++) {
		if ((x * increment) <= (o->x - range))
			continue;
		if ((x * increment) >= (o->x + range))
			continue;
		/* find y intersections with radar circle by pyth. theorem. */
		lx1 = x * increment - o->x;
		ly1 = sqrt((range2 - (lx1 * lx1)));
		ly2 = -ly1;

		x1 = (int) (((double) r) / range * lx1) + cx;
		y1 = (int) (((double) r) / range * ly1) + cy;
		y2 = (int) (((double) r) / range * ly2) + cy;
		if (y1 < ylow)
			y1 = ylow;
		if (y1 > yhigh)
			y1 = yhigh;
		if (y2 < ylow)
			y2 = ylow;
		if (y2 > yhigh)
			y2 = yhigh;
		snis_draw_dotted_vline(drawable, gc, x1, y2, y1, 10);
	}
	/* horizontal lines */
	increment = (YKNOWN_DIM / 100.0);
	for (y = 0; y <= 100; y++) {
		if ((y * increment) <= (o->y - range))
			continue;
		if ((y * increment) >= (o->y + range))
			continue;
		/* find x intersections with radar circle by pyth. theorem. */
		ly1 = y * increment - o->y;
		lx1 = sqrt((range2 - (ly1 * ly1)));
		lx2 = -lx1;
		y1 = (int) (((double) r) / range * ly1) + cy;
		x1 = (int) (((double) r) / range * lx1) + cx;
		x2 = (int) (((double) r) / range * lx2) + cx;
		if (x1 < xlow)
			x1 = xlow;
		if (x1 > xhigh)
			x1 = xhigh;
		if (x2 < xlow)
			x2 = xlow;
		if (x2 > xhigh)
			x2 = xhigh;
		snis_draw_dotted_hline(drawable, gc, x2, y1, x1, 10);
	}
}

static void load_torpedo_button_pressed()
{
	struct snis_entity *o;
	struct packed_buffer *pb;

	if (my_ship_oid == UNKNOWN_ID) {
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
		if (my_ship_oid == UNKNOWN_ID)
			return;
	}
	o = &go[my_ship_oid];

	if (o->tsd.ship.torpedoes <= 0)
		return;
	if (o->tsd.ship.torpedoes_loaded >= 2)
		return;
	if (o->tsd.ship.torpedoes_loading != 0)
		return;
	pb = packed_buffer_allocate(sizeof(uint16_t));
	packed_buffer_append_u16(pb, OPCODE_LOAD_TORPEDO);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	wakeup_gameserver_writer();
}

static void fire_phaser_button_pressed(__attribute__((unused)) void *notused)
{
#if 0
	struct snis_entity *o;
	struct packed_buffer *pb;

	if (my_ship_oid == UNKNOWN_ID) {
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
		if (my_ship_oid == UNKNOWN_ID)
			return;
	}
	o = &go[my_ship_oid];
	if (o->tsd.ship.phaser_bank_charge < 128.0)
		return;
	pb = packed_buffer_allocate(sizeof(uint16_t));
	packed_buffer_append_u16(pb, OPCODE_REQUEST_PHASER);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	wakeup_gameserver_writer();
#endif
	do_laser();
}

static void fire_torpedo_button_pressed(__attribute__((unused)) void *notused)
{
	do_torpedo();
}

static void do_adjust_byte_value(uint8_t value,  uint16_t opcode)
{
	struct packed_buffer *pb;
	struct snis_entity *o;

	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return;
	o = &go[my_ship_oid];

	pb = packed_buffer_allocate(sizeof(struct request_throttle_packet));
	packed_buffer_append(pb, "hwb", opcode, o->id, value);
	packed_buffer_queue_add(&to_server_queue, pb, &to_server_queue_mutex);
	wakeup_gameserver_writer();
}

static void do_adjust_slider_value(struct slider *s,  uint16_t opcode)
{
	uint8_t value = (uint8_t) (255.0 * snis_slider_get_input(s));
	do_adjust_byte_value(value, opcode);
}

static void do_scizoom(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_SCIZOOM);
}
	
static void do_throttle(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_THROTTLE);
}
	
static void do_warpdrive(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_WARPDRIVE);
}

static void do_shieldadj(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_SHIELD);
}

static void do_maneuvering_pwr(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_MANEUVERING_PWR);
}
	
static void do_shields_pwr(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_SHIELDS_PWR);
}
	
static void do_impulse_pwr(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_IMPULSE_PWR);
}

static void do_warp_pwr(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_WARP_PWR);
}

static void do_sensors_pwr(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_SENSORS_PWR);
}
	
static void do_phaserbanks_pwr(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_PHASERBANKS_PWR);
}
	
static void do_comms_pwr(struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_COMMS_PWR);
}
	
static double sample_shields(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (double) 100.0 * go[my_ship_oid].tsd.ship.pwrdist.shields / 255.0;
}

static double sample_rpm(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (100 * go[my_ship_oid].tsd.ship.rpm) / UINT8_MAX;
}

static double sample_power(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (100.0 * go[my_ship_oid].tsd.ship.power) / UINT32_MAX;
}

static double sample_temp(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (100 * go[my_ship_oid].tsd.ship.temp) / UINT8_MAX;
}

static double sample_throttle(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (double) 100.0 * go[my_ship_oid].tsd.ship.throttle / 255.0;
}

static double sample_reqshield(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (double) 100.0 * go[my_ship_oid].tsd.ship.requested_shield / 255.0;
}

static double sample_reqwarpdrive(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (double) 100.0 * go[my_ship_oid].tsd.ship.requested_warpdrive / 255.0;
}

static double sample_warpdrive_power_avail(void)
{
	int my_ship_oid;
	double answer;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	answer = (double) 10.0 * (double) go[my_ship_oid].tsd.ship.pwrdist.warp / 255.0 *
			(sample_power() / 100.0) / WARP_POWER_FACTOR;
	if (answer > 10.0)
		answer = 10.0;
	return answer;
}

static double sample_warpdrive(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (double) 10.0 * go[my_ship_oid].tsd.ship.warpdrive / 255.0;
}

static double sample_scizoom(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (double) 100.0 * go[my_ship_oid].tsd.ship.scizoom / 255.0;
}

static double sample_fuel(void)
{
	int my_ship_oid;

	my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	return (100.0 * go[my_ship_oid].tsd.ship.fuel) / UINT32_MAX;
}

static double sample_phaserbanks(void)
{
	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	return (o->tsd.ship.pwrdist.phaserbanks / 255.0) * 100.0;
}

static double sample_phasercharge(void)
{
	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	return (o->tsd.ship.phaser_charge / 255.0) * 100.0;
}

static double sample_phaser_wavelength(void)
{
	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	return 50.0 * o->tsd.ship.phaser_wavelength / 255.0 + 10.0;
}

static void do_phaser_wavelength(__attribute__((unused)) struct slider *s)
{
	do_adjust_slider_value(s, OPCODE_REQUEST_LASER_WAVELENGTH);
}

static void wavelen_updown_button_pressed(int direction)
{
	uint8_t value = (uint8_t) (255.0 * sample_phaser_wavelength());
	struct snis_entity *o;
	int inc;

	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return;

	inc = (int) (256.0 / 50.0);

	o = &go[my_ship_oid];
	value = o->tsd.ship.phaser_wavelength;
	if (direction > 0 && value + inc > 255)
		return;
	if (direction < 0 && value - inc < 0)
		return;
	value += direction < 0 ? -inc : direction > 0 ? inc : 0;
	do_adjust_byte_value(value, OPCODE_REQUEST_LASER_WAVELENGTH);
}

static void wavelen_up_button_pressed(__attribute__((unused)) void *s)
{
	wavelen_updown_button_pressed(1);
}

static void wavelen_down_button_pressed(__attribute__((unused)) void *s)
{
	wavelen_updown_button_pressed(-1);
}

static double sample_warp(void)
{
	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	return 100.0 * o->tsd.ship.pwrdist.warp / 255.0;
}

static double sample_maneuvering(void)
{
	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	return 100.0 * o->tsd.ship.pwrdist.maneuvering / 255.0;
}

static double sample_impulse(void)
{
	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	return 100.0 * o->tsd.ship.pwrdist.impulse / 255.0;
}

static double sample_comms(void)
{
	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	return 100.0 * o->tsd.ship.pwrdist.comms / 255.0;
}

static double sample_sensors(void)
{
	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	return 100.0 * o->tsd.ship.pwrdist.sensors / 255.0;
}

static double sample_generic_damage_data(int field_offset)
{
	uint8_t *field;

	struct snis_entity *o;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0.0;
	o = &go[my_ship_oid];
	field = (uint8_t *) &o->tsd.ship.damage + field_offset; 

	return 100.0 * (255 - *field) / 255.0;
}

#define CREATE_DAMAGE_SAMPLER_FUNC(fieldname) \
	static double sample_##fieldname(void) \
	{ \
		return sample_generic_damage_data(offsetof(struct ship_damage_data, fieldname)); \
	}

CREATE_DAMAGE_SAMPLER_FUNC(shield_damage) /* sample_shield_damage defined here */
CREATE_DAMAGE_SAMPLER_FUNC(impulse_damage) /* sample_impulse_damage defined here */
CREATE_DAMAGE_SAMPLER_FUNC(warp_damage) /* sample_warp_damage defined here */
CREATE_DAMAGE_SAMPLER_FUNC(torpedo_tubes_damage) /* sample_torpedo_tubes_damage defined here */
CREATE_DAMAGE_SAMPLER_FUNC(phaser_banks_damage) /* sample_phaser_banks_damage defined here */
CREATE_DAMAGE_SAMPLER_FUNC(sensors_damage) /* sample_sensors_damage defined here */
CREATE_DAMAGE_SAMPLER_FUNC(comms_damage) /* sample_comms__damage defined here */

static struct navigation_ui {
	struct slider *warp_slider;
	struct slider *shield_slider;
	struct gauge *warp_gauge;
	struct button *engage_warp_button;
	struct button *warp_up_button;
	struct button *warp_down_button;
} nav_ui;

static void engage_warp_button_pressed(__attribute__((unused)) void *cookie)
{
	do_adjust_byte_value(0,  OPCODE_ENGAGE_WARP);
}

static void warp_updown_button_pressed(int direction)
{
	int value;
	struct snis_entity *o;
	double inc;

	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return;

	inc = 2.5;

	o = &go[my_ship_oid];
	value = o->tsd.ship.requested_warpdrive;
	if (direction > 0 && value + inc > 255)
		return;
	if (direction < 0 && value - inc < 0)
		return;
	value += direction < 0 ? -inc : direction > 0 ? inc : 0;
	snis_slider_set_input(nav_ui.warp_slider, (double) value / 255.0);
	do_adjust_slider_value(nav_ui.warp_slider, OPCODE_REQUEST_WARPDRIVE);
}

static void warp_up_button_pressed(__attribute__((unused)) void *s)
{
	warp_updown_button_pressed(1);
}

static void warp_down_button_pressed(__attribute__((unused)) void *s)
{
	warp_updown_button_pressed(-1);
}

struct weapons_ui {
	struct button *fire_torpedo, *load_torpedo, *fire_phaser;
	struct gauge *phaser_bank_gauge;
	struct gauge *phaser_wavelength;
	struct slider *wavelen_slider;
	struct button *wavelen_up_button;
	struct button *wavelen_down_button;
} weapons;

static void ui_add_slider(struct slider *s, int active_displaymode)
{
	struct ui_element *uie;

	uie = ui_element_init(s, ui_slider_draw, ui_slider_button_press,
						active_displaymode, &displaymode);
	ui_element_list_add_element(&uiobjs, uie); 
}

static double sample_phaserbanks(void);
static double sample_phaser_wavelength(void);
static void init_weapons_ui(void)
{
	int y = 450;

	weapons.fire_phaser = snis_button_init(550, y, 200, 25, "FIRE PHASER", RED,
			TINY_FONT, fire_phaser_button_pressed, NULL, DISPLAYMODE_WEAPONS,
			&displaymode);
	y += 50;
	weapons.load_torpedo = snis_button_init(550, y, 200, 25, "LOAD TORPEDO", GREEN,
			TINY_FONT, load_torpedo_button_pressed, NULL, DISPLAYMODE_WEAPONS,
			&displaymode);
	y += 50;
	weapons.fire_torpedo = snis_button_init(550, y, 200, 25, "FIRE TORPEDO", RED,
			TINY_FONT, fire_torpedo_button_pressed, NULL, DISPLAYMODE_WEAPONS,
			&displaymode);
	weapons.phaser_bank_gauge = gauge_init(650, 100, 90, 0.0, 100.0, -120.0 * M_PI / 180.0,
			120.0 * 2.0 * M_PI / 180.0, RED, WHITE,
			10, "CHARGE", sample_phasercharge);
	weapons.phaser_wavelength = gauge_init(650, 300, 90, 10.0, 60.0, -120.0 * M_PI / 180.0,
			120.0 * 2.0 * M_PI / 180.0, RED, WHITE,
			10, "WAVE LEN", sample_phaser_wavelength);
	weapons.wavelen_down_button = snis_button_init(550, 400, 60, 25, "DOWN", WHITE,
			NANO_FONT, wavelen_down_button_pressed, NULL, DISPLAYMODE_WEAPONS,
			&displaymode);
	weapons.wavelen_up_button = snis_button_init(700, 400, 30, 25, "UP", WHITE,
			NANO_FONT, wavelen_up_button_pressed, NULL, DISPLAYMODE_WEAPONS,
			&displaymode);
	weapons.wavelen_slider = snis_slider_init(620, 400, 70, AMBER, "",
				"10", "60", 10, 60, sample_phaser_wavelength,
				do_phaser_wavelength, DISPLAYMODE_WEAPONS, &displaymode);
	snis_add_button(weapons.fire_phaser);
	snis_add_button(weapons.load_torpedo);
	snis_add_button(weapons.fire_torpedo);
	snis_add_button(weapons.wavelen_up_button);
	snis_add_button(weapons.wavelen_down_button);
	ui_add_slider(weapons.wavelen_slider, DISPLAYMODE_WEAPONS);
}

static void show_death_screen(GtkWidget *w)
{
	char buf[100];

	sng_set_foreground(RED);
	sprintf(buf, "YOUR SHIP");
	sng_abs_xy_draw_string(w, gc, buf, BIG_FONT, 20, 150);
	sprintf(buf, "HAS BEEN");
	sng_abs_xy_draw_string(w, gc, buf, BIG_FONT, 20, 250);
	sprintf(buf, "BLOWN TO");
	sng_abs_xy_draw_string(w, gc, buf, BIG_FONT, 20, 350);
	sprintf(buf, "SMITHEREENS");
	sng_abs_xy_draw_string(w, gc, buf, BIG_FONT, 20, 450);
	sprintf(buf, "RESPAWNING IN %d SECONDS", go[my_ship_oid].respawn_time);
	sng_abs_xy_draw_string(w, gc, buf, TINY_FONT, 20, 500);
}

static void show_weapons(GtkWidget *w)
{
	char buf[100];
	struct snis_entity *o;
	int rx, ry, rw, rh, cx, cy;
	int r;
	int buttoncolor;

	show_common_screen(w, "Weapons");
	sng_set_foreground(GREEN);

	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return;
	o = &go[my_ship_oid];
	sprintf(buf, "PHOTON TORPEDOES: %03d", o->tsd.ship.torpedoes);
	sng_abs_xy_draw_string(w, gc, buf, NANO_FONT, 250, 15);
	sprintf(buf, "TORPEDOES LOADED: %03d", o->tsd.ship.torpedoes_loaded);
	sng_abs_xy_draw_string(w, gc, buf, NANO_FONT, 250, 15 + 0.5 * LINEHEIGHT);
	sprintf(buf, "TORPEDOES LOADING: %03d", o->tsd.ship.torpedoes_loading);
	sng_abs_xy_draw_string(w, gc, buf, NANO_FONT, 250, 15 + LINEHEIGHT);
/*
	sprintf(buf, "vx: %5.2lf", o->vx);
	sng_abs_xy_draw_string(w, gc, buf, TINY_FONT, 600, LINEHEIGHT * 3);
	sprintf(buf, "vy: %5.2lf", o->vy);
	sng_abs_xy_draw_string(w, gc, buf, TINY_FONT, 600, LINEHEIGHT * 4);
*/

	buttoncolor = RED;
	if (o->tsd.ship.torpedoes > 0 && o->tsd.ship.torpedoes_loading == 0 &&
		o->tsd.ship.torpedoes_loaded < 2)
		buttoncolor = GREEN;
	snis_button_set_color(weapons.load_torpedo, buttoncolor);
	buttoncolor = RED;
	if (o->tsd.ship.torpedoes_loaded)
		buttoncolor = GREEN;
	snis_button_set_color(weapons.fire_torpedo, buttoncolor);

	rx = 40;
	ry = 90;
	rw = 470;
	rh = 470;
	cx = rx + (rw / 2);
	cy = ry + (rh / 2);
	r = rh / 2;
	sng_set_foreground(GREEN);
	snis_draw_radar_sector_labels(w, gc, o, cx, cy, r, NAVSCREEN_RADIUS);
	snis_draw_radar_grid(w->window, gc, o, cx, cy, r, NAVSCREEN_RADIUS, 1);
	sng_set_foreground(BLUE);
	snis_draw_reticule(w, gc, cx, cy, r, o->tsd.ship.gun_heading);
	draw_all_the_guys(w, o);
	draw_all_the_sparks(w, o);
	gauge_draw(w, gc, weapons.phaser_bank_gauge);
	gauge_draw(w, gc, weapons.phaser_wavelength);
}

static double sample_reqwarpdrive(void);
static double sample_warpdrive(void);
static void init_nav_ui(void)
{
	nav_ui.shield_slider = snis_slider_init(540, 270, 160, AMBER, "SHIELDS",
				"0", "100", 0.0, 100.0, sample_reqshield,
				do_shieldadj, DISPLAYMODE_NAVIGATION, &displaymode);
	nav_ui.warp_slider = snis_slider_init(500, SCREEN_HEIGHT - 40, 200, AMBER, "Warp",
				"0", "100", 0.0, 100.0, sample_reqwarpdrive,
				do_warpdrive, DISPLAYMODE_NAVIGATION, &displaymode);
	nav_ui.warp_gauge = gauge_init(650, 410, 100, 0.0, 10.0, -120.0 * M_PI / 180.0,
				120.0 * 2.0 * M_PI / 180.0, RED, AMBER,
				10, "WARP", sample_warpdrive);
	gauge_add_needle(nav_ui.warp_gauge, sample_warpdrive_power_avail, RED);
	nav_ui.engage_warp_button = snis_button_init(570, 520, 150, 25, "ENGAGE WARP", AMBER,
				NANO_FONT, engage_warp_button_pressed, NULL,
				DISPLAYMODE_NAVIGATION, &displaymode);
	nav_ui.warp_up_button = snis_button_init(500, 490, 40, 25, "UP", AMBER,
			NANO_FONT, warp_up_button_pressed, NULL, DISPLAYMODE_NAVIGATION,
			&displaymode);
	nav_ui.warp_down_button = snis_button_init(500, 520, 60, 25, "DOWN", AMBER,
			NANO_FONT, warp_down_button_pressed, NULL, DISPLAYMODE_NAVIGATION,
			&displaymode);
	ui_add_slider(nav_ui.warp_slider, DISPLAYMODE_NAVIGATION);
	ui_add_slider(nav_ui.shield_slider, DISPLAYMODE_NAVIGATION);
	snis_add_button(nav_ui.engage_warp_button);
	snis_add_button(nav_ui.warp_up_button);
	snis_add_button(nav_ui.warp_down_button);
}

#if 0
#define NAV_SCOPE_X 20
#define NAV_SCOPE_Y 70 
#define NAV_SCOPE_W 500
#define NAV_SCOPE_H NAV_SCOPE_W
#define NAV_SCOPE_R (NAV_SCOPE_H / 2)
#define NAV_SCOPE_CX (NAV_SCOPE_X + NAV_SCOPE_R)
#define NAV_SCOPE_CY (NAV_SCOPE_Y + NAV_SCOPE_R)
#endif
#define NAV_DATA_X 530 
#define NAV_DATA_Y 40 
#define NAV_DATA_W ((SCREEN_WIDTH - 5) - NAV_DATA_X)
#define NAV_DATA_H 270 

static void draw_science_graph(GtkWidget *w, struct snis_entity *ship, struct snis_entity *o,
		int x1, int y1, int x2, int y2);

static void show_navigation(GtkWidget *w)
{
	char buf[100];
	struct snis_entity *o;
	int rx, ry, rw, rh, cx, cy, gx1, gy1, gx2, gy2;
	int r, sectorx, sectory;

	show_common_screen(w, "Navigation");
	sng_set_foreground(GREEN);

	if (my_ship_id == UNKNOWN_ID)
		return;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return;
	o = &go[my_ship_oid];
	sectorx = floor(10.0 * o->x / (double) XKNOWN_DIM);
	sectory = floor(10.0 * o->y / (double) YKNOWN_DIM);
	sprintf(buf, "SECTOR: %c%d (%5.2lf, %5.2lf)", sectory + 'A', sectorx, o->x, o->y);
	sng_abs_xy_draw_string(w, gc, buf, NANO_FONT, 200, LINEHEIGHT);
	sprintf(buf, "HEADING: %3.1lf", 360.0 * o->heading / (2.0 * M_PI));
	sng_abs_xy_draw_string(w, gc, buf, NANO_FONT, 200, 1.5 * LINEHEIGHT);
#if 0
	sprintf(buf, "vx: %5.2lf", o->vx);
	sng_abs_xy_draw_string(w, gc, buf, TINY_FONT, 600, LINEHEIGHT * 3);
	sprintf(buf, "vy: %5.2lf", o->vy);
	sng_abs_xy_draw_string(w, gc, buf, TINY_FONT, 600, LINEHEIGHT * 4);
#endif
	rx = 40;
	ry = 90;
	rw = 470;
	rh = 470;
	cx = rx + (rw / 2);
	cy = ry + (rh / 2);
	r = rh / 2;
	sng_set_foreground(GREEN);
	snis_draw_radar_sector_labels(w, gc, o, cx, cy, r, NAVSCREEN_RADIUS);
	snis_draw_radar_grid(w->window, gc, o, cx, cy, r, NAVSCREEN_RADIUS, 1);
	sng_set_foreground(DARKRED);
	snis_draw_reticule(w, gc, cx, cy, r, o->heading);

	draw_all_the_guys(w, o);
	draw_all_the_sparks(w, o);
	gauge_draw(w, gc, nav_ui.warp_gauge);

	gx1 = NAV_DATA_X + 10;
	gy1 = 15;
	gx2 = NAV_DATA_X + NAV_DATA_W - 10;
	gy2 = NAV_DATA_Y + NAV_DATA_H - 80;
	sng_set_foreground(AMBER);
	draw_science_graph(w, o, o, gx1, gy1, gx2, gy2);
}

struct enginerring_ui {
	struct gauge *fuel_gauge;
	struct gauge *power_gauge;
	struct gauge *rpm_gauge;
	struct gauge *temp_gauge;
	struct slider *shield_slider;
	struct slider *maneuvering_slider;
	struct slider *warp_slider;
	struct slider *impulse_slider;
	struct slider *sensors_slider;
	struct slider *comm_slider;
	struct slider *phaserbanks_slider;
	struct slider *throttle_slider;

	struct slider *shield_damage;
	struct slider *impulse_damage;
	struct slider *warp_damage;
	struct slider *torpedo_tubes_damage;
	struct slider *phaser_banks_damage;
	struct slider *sensors_damage;
	struct slider *comms_damage;

} eng_ui;

static void init_engineering_ui(void)
{
	int y = 220;
	int x = 100;
	int xinc = 190;
	int yinc = 40; 
	eng_ui.rpm_gauge = gauge_init(x, 140, 90, 0.0, 100.0, -120.0 * M_PI / 180.0,
			120.0 * 2.0 * M_PI / 180.0, RED, AMBER,
			10, "RPM", sample_rpm);
	x += xinc;
	eng_ui.fuel_gauge = gauge_init(x, 140, 90, 0.0, 100.0, -120.0 * M_PI / 180.0,
			120.0 * 2.0 * M_PI / 180.0, RED, AMBER,
			10, "FUEL", sample_fuel);
	x += xinc;
	eng_ui.power_gauge = gauge_init(x, 140, 90, 0.0, 100.0, -120.0 * M_PI / 180.0,
			120.0 * 2.0 * M_PI / 180.0, RED, AMBER,
			10, "POWER", sample_power);
	x += xinc;
	eng_ui.temp_gauge = gauge_init(x, 140, 90, 0.0, 100.0, -120.0 * M_PI / 180.0,
			120.0 * 2.0 * M_PI / 180.0, RED, AMBER,
			10, "TEMP", sample_temp);
	x += xinc;
	eng_ui.throttle_slider = snis_slider_init(350, y + yinc, 200, AMBER, "THROTTLE", "0", "100",
				0.0, 100.0, sample_throttle, do_throttle,
				DISPLAYMODE_ENGINEERING, &displaymode);

	y += yinc;
	eng_ui.shield_slider = snis_slider_init(20, y += yinc, 150, AMBER, "SHIELDS", "0", "100",
				0.0, 100.0, sample_shields, do_shields_pwr, 
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.phaserbanks_slider = snis_slider_init(20, y += yinc, 150, AMBER, "PHASERS", "0", "100",
				0.0, 100.0, sample_phaserbanks, do_phaserbanks_pwr, 
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.comm_slider = snis_slider_init(20, y += yinc, 150, AMBER, "COMMS", "0", "100",
				0.0, 100.0, sample_comms, do_comms_pwr, 
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.sensors_slider = snis_slider_init(20, y += yinc, 150, AMBER, "SENSORS", "0", "100",
				0.0, 100.0, sample_sensors, do_sensors_pwr, 
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.impulse_slider = snis_slider_init(20, y += yinc, 150, AMBER, "IMPULSE DR", "0", "100",
				0.0, 100.0, sample_impulse, do_impulse_pwr, 
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.warp_slider = snis_slider_init(20, y += yinc, 150, AMBER, "WARP DR", "0", "100",
				0.0, 100.0, sample_warp, do_warp_pwr, 
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.maneuvering_slider = snis_slider_init(20, y += yinc, 150, AMBER, "MANEUVERING", "0", "100",
				0.0, 100.0, sample_maneuvering, do_maneuvering_pwr, 
				DISPLAYMODE_ENGINEERING, &displaymode);
	ui_add_slider(eng_ui.shield_slider, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.phaserbanks_slider, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.comm_slider, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.sensors_slider, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.impulse_slider, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.warp_slider, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.maneuvering_slider, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.throttle_slider, DISPLAYMODE_ENGINEERING);

	y = 220 + yinc;
	eng_ui.shield_damage = snis_slider_init(350, y += yinc, 150, AMBER, "SHIELD STATUS", "0", "100",
				0.0, 100.0, sample_shield_damage, NULL,
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.impulse_damage = snis_slider_init(350, y += yinc, 150, AMBER, "IMPULSE STATUS", "0", "100",
				0.0, 100.0, sample_impulse_damage, NULL,
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.warp_damage = snis_slider_init(350, y += yinc, 150, AMBER, "WARP STATUS", "0", "100",
				0.0, 100.0, sample_warp_damage, NULL,
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.torpedo_tubes_damage = snis_slider_init(350, y += yinc, 150, AMBER, "TORPEDO STATUS", "0", "100",
				0.0, 100.0, sample_torpedo_tubes_damage, NULL,
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.phaser_banks_damage = snis_slider_init(350, y += yinc, 150, AMBER, "PHASER STATUS", "0", "100",
				0.0, 100.0, sample_phaser_banks_damage, NULL,
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.sensors_damage = snis_slider_init(350, y += yinc, 150, AMBER, "SENSORS STATUS", "0", "100",
				0.0, 100.0, sample_sensors_damage, NULL,
				DISPLAYMODE_ENGINEERING, &displaymode);
	eng_ui.comms_damage = snis_slider_init(350, y += yinc, 150, AMBER, "COMMS STATUS", "0", "100",
				0.0, 100.0, sample_comms_damage, NULL,
				DISPLAYMODE_ENGINEERING, &displaymode);
	ui_add_slider(eng_ui.shield_damage, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.impulse_damage, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.warp_damage, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.torpedo_tubes_damage, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.phaser_banks_damage, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.sensors_damage, DISPLAYMODE_ENGINEERING);
	ui_add_slider(eng_ui.comms_damage, DISPLAYMODE_ENGINEERING);
}

static void show_engineering(GtkWidget *w)
{
	show_common_screen(w, "Engineering");
	gauge_draw(w, gc, eng_ui.fuel_gauge);
	gauge_draw(w, gc, eng_ui.rpm_gauge);
	gauge_draw(w, gc, eng_ui.power_gauge);
	gauge_draw(w, gc, eng_ui.temp_gauge);
}

struct science_ui {
	struct slider *scizoom;
} sci_ui;

static void init_science_ui(void)
{
	sci_ui.scizoom = snis_slider_init(350, 50, 300, DARKGREEN, "Range", "0", "100",
				0.0, 100.0, sample_scizoom, do_scizoom, DISPLAYMODE_SCIENCE,
				&displaymode);
	ui_add_slider(sci_ui.scizoom, DISPLAYMODE_SCIENCE);
}

static void comms_screen_button_pressed(void *x)
{
	unsigned long screen = (unsigned long) x;

	switch (screen) {
	case 0: do_onscreen((uint8_t) DISPLAYMODE_COMMS);
		break;
	case 1: do_onscreen((uint8_t) DISPLAYMODE_NAVIGATION);
		break;
	case 2: do_onscreen((uint8_t) DISPLAYMODE_WEAPONS);
		break;
	case 3: do_onscreen((uint8_t) DISPLAYMODE_ENGINEERING);
		break;
	case 4: do_onscreen((uint8_t) DISPLAYMODE_SCIENCE);
		break;
	case 5: do_onscreen((uint8_t) DISPLAYMODE_MAINSCREEN);
		break;
	default:
		break;
	}
	return;
}
static void init_comms_ui(void)
{
	int x = 200;
	int y = 20;

	comms_ui.comms_onscreen_button = snis_button_init(x, y, 75, 25, "COMMS", GREEN,
			NANO_FONT, comms_screen_button_pressed, (void *) 0, DISPLAYMODE_COMMS,
			&displaymode);
	x += 75;
	comms_ui.nav_onscreen_button = snis_button_init(x, y, 75, 25, "NAV", GREEN,
			NANO_FONT, comms_screen_button_pressed, (void *) 1, DISPLAYMODE_COMMS,
			&displaymode);
	x += 75;
	comms_ui.weap_onscreen_button = snis_button_init(x, y, 75, 25, "WEAP", GREEN,
			NANO_FONT, comms_screen_button_pressed, (void *) 2, DISPLAYMODE_COMMS,
			&displaymode);
	x += 75;
	comms_ui.eng_onscreen_button = snis_button_init(x, y, 75, 25, "ENG", GREEN,
			NANO_FONT, comms_screen_button_pressed, (void *) 3, DISPLAYMODE_COMMS,
			&displaymode);
	x += 75;
	comms_ui.sci_onscreen_button = snis_button_init(x, y, 75, 25, "SCI", GREEN,
			NANO_FONT, comms_screen_button_pressed, (void *) 4, DISPLAYMODE_COMMS,
			&displaymode);
	x += 75;
	comms_ui.main_onscreen_button = snis_button_init(x, y, 75, 25, "MAIN", GREEN,
			NANO_FONT, comms_screen_button_pressed, (void *) 5, DISPLAYMODE_COMMS,
			&displaymode);
	comms_ui.tw = text_window_init(10, 70, SCREEN_WIDTH - 20,
			40, 20, DISPLAYMODE_COMMS, &displaymode, GREEN);
	text_window_add(comms_ui.tw);
	snis_add_button(comms_ui.comms_onscreen_button);
	snis_add_button(comms_ui.nav_onscreen_button);
	snis_add_button(comms_ui.weap_onscreen_button);
	snis_add_button(comms_ui.eng_onscreen_button);
	snis_add_button(comms_ui.sci_onscreen_button);
	snis_add_button(comms_ui.main_onscreen_button);
}

#define SCIDIST2 100
static int science_button_press(int x, int y)
{
	int i;
	int xdist, ydist, dist2;
	struct snis_entity *selected;
	struct snis_entity *o;
	double ur, ux, uy;
	int cx, cy, r;
	double dx, dy;

	selected = NULL;
	pthread_mutex_lock(&universe_mutex);
	for (i = 0; i < nscience_guys; i++) {
		xdist = (x - science_guy[i].sx);
		ydist = (y - science_guy[i].sy);
		dist2 = xdist * xdist + ydist * ydist; 
		if (dist2 < SCIDIST2 && science_guy[i].o->sdata.science_data_known) {
			// curr_science_guy = science_guy[i].o;
			selected = science_guy[i].o;
		}
	}
	if (selected)
		request_sci_select_target(selected->id);
	else {
		o = &go[my_ship_oid];
		cx = SCIENCE_SCOPE_CX;
		cy = SCIENCE_SCOPE_CY;
		r = SCIENCE_SCOPE_R;
		ur = (MAX_SCIENCE_SCREEN_RADIUS - MIN_SCIENCE_SCREEN_RADIUS) *
				(o->tsd.ship.scizoom / 255.0) +
				MIN_SCIENCE_SCREEN_RADIUS;
		dx = x - cx;
		dy = y - cy;
		ux = o->x + dx * ur / r;
		uy = o->y + dy * ur / r;
		request_sci_select_coords(ux, uy);
	}
	pthread_mutex_unlock(&universe_mutex);

	return 0;
}

#define SCIENCE_DATA_X (SCIENCE_SCOPE_X + SCIENCE_SCOPE_W + 20)
#define SCIENCE_DATA_Y (SCIENCE_SCOPE_Y + 20)
#define SCIENCE_DATA_W (SCREEN_WIDTH - 20 - SCIENCE_DATA_X)
#define SCIENCE_DATA_H (SCREEN_HEIGHT - 20 - SCIENCE_DATA_Y)

static void draw_science_graph(GtkWidget *w, struct snis_entity *ship, struct snis_entity *o,
		int x1, int y1, int x2, int y2)
{
	int i, x;
	double sx, sy, sy1, sy2, dist;
	int dy1, dy2, bw, probes, dx, pwr;
	int initial_noise;

	sng_current_draw_rectangle(w->window, gc, 0, x1, y1, (x2 - x1), (y2 - y1));
	snis_draw_dotted_hline(w->window, gc, x1, y1 + (y2 - y1) / 4, x2, 10);
	snis_draw_dotted_hline(w->window, gc, x1, y1 + (y2 - y1) / 2, x2, 10);
	snis_draw_dotted_hline(w->window, gc, x1, y1 + 3 * (y2 - y1) / 4, x2, 10);

	x = x1 + (x2 - x1) / 4; 
	snis_draw_dotted_vline(w->window, gc, x, y1, y2, 10);
	x += (x2 - x1) / 4; 
	snis_draw_dotted_vline(w->window, gc, x, y1, y2, 10);
	x += (x2 - x1) / 4; 
	snis_draw_dotted_vline(w->window, gc, x, y1, y2, 10);
	
	if (o) {
		if (o != ship) {
			dist = hypot(o->x - go[my_ship_oid].x, o->y - go[my_ship_oid].y);
			bw = (int) (go[my_ship_oid].tsd.ship.sci_beam_width * 180.0 / M_PI);
			pwr = (( (float) go[my_ship_oid].tsd.ship.pwrdist.sensors / 255.0) /
					SENSORS_POWER_FACTOR) * go[my_ship_oid].tsd.ship.power;
			pwr = 255.0 * ((float) o->tsd.ship.pwrdist.sensors / 255.0) / SENSORS_POWER_FACTOR *
						(float) o->tsd.ship.power / (float) UINT32_MAX;
			if (pwr > 255)
				pwr = 255;
		} else {
			dist = 0.1;
			bw = 5.0;
			pwr = 255;
		}

		sng_set_foreground(LIMEGREEN);
		/* TODO, make sample count vary based on sensor power,damage */
		probes = (30 * 10) / (bw / 2 + ((dist * 2.0) / XKNOWN_DIM));
		initial_noise = (int) ((hypot((float) bw, 256.0 - pwr) / 256.0) * 20.0);
		for (i = 0; i < probes; i++) {
			double ss;
			int nx, ny;

			nx = ny = initial_noise;
			if (nx <= 0)
				nx = 1;
			if (ny <= 0)
				ny = 1;
			dy1 = snis_randn(ny) - ny / 2; /* TODO: make this vary based on damage */
			dy2 = snis_randn(ny) - ny / 2;
			dx = snis_randn(nx) - nx / 2;

			x = snis_randn(256);
			ss = shield_strength((uint8_t) x, o->sdata.shield_strength,
						o->sdata.shield_width,
						o->sdata.shield_depth,
						o->sdata.shield_wavelength);
			sx = (int) (((double) x / 255.0) * (double) (x2 - x1)) + x1;
			sy = (int) (((1.0 - ss) * (double) (y2 - y1)) + y1);

			sy1 = sy + dy1;
			if (sy1 < y1)
				sy1 = y1;
			if (sy1 > y2)
				sy1 = y2;
			sy2 = sy + dy2;
			if (sy2 < y1)
				sy2 = y1;
			if (sy2 > y2)
				sy2 = y2;
			sx += dx;
			if (sx < x1)
				sx = x1;
			if (x > x2)
				sx = x2;

			snis_draw_dotted_vline(w->window, gc, sx, sy1, sy2, 4);
		}
	}
	sng_set_foreground(GREEN);
	sng_abs_xy_draw_string(w, gc, "10", NANO_FONT, x1, y2 + 10);
	sng_abs_xy_draw_string(w, gc, "20", NANO_FONT, x1 + (x2 - x1) / 4 - 10, y2 + 10);
	sng_abs_xy_draw_string(w, gc, "30", NANO_FONT, x1 + 2 * (x2 - x1) / 4 - 10, y2 + 10);
	sng_abs_xy_draw_string(w, gc, "40", NANO_FONT, x1 + 3 * (x2 - x1) / 4 - 10, y2 + 10);
	sng_abs_xy_draw_string(w, gc, "50", NANO_FONT, x1 + 4 * (x2 - x1) / 4 - 20, y2 + 10);
	sng_abs_xy_draw_string(w, gc, "Shield Profile (nm)", NANO_FONT, x1 + (x2 - x1) / 4 - 10, y2 + 30);
}

static void draw_science_warp_data(GtkWidget *w, struct snis_entity *ship)
{
	double bearing, dx, dy;
	char buffer[40];

	if (!ship)
		return;
	sng_set_foreground(GREEN);
	dx = ship->x - ship->sci_coordx;
	dy = ship->y - ship->sci_coordy;
	bearing = atan2(dx, dy) * 180 / M_PI;
	if (bearing < 0)
		bearing = -bearing;
	else
		bearing = 360.0 - bearing;
	sng_abs_xy_draw_string(w, gc, "WARP DATA:", NANO_FONT, 10, SCREEN_HEIGHT - 40);
	sprintf(buffer, "BEARING: %3.2lf", bearing);
	sng_abs_xy_draw_string(w, gc, buffer, NANO_FONT, 10, SCREEN_HEIGHT - 25);
	sprintf(buffer, "WARP FACTOR: %2.2lf", 10.0 * hypot(dy, dx) / (XKNOWN_DIM / 2.0));
	sng_abs_xy_draw_string(w, gc, buffer, NANO_FONT, 10, SCREEN_HEIGHT - 10);
}
 
static void draw_science_data(GtkWidget *w, struct snis_entity *ship, struct snis_entity *o)
{
	char buffer[40];
	int x, y, gx1, gy1, gx2, gy2;
	double bearing, dx, dy, range;

	if (!ship)
		return;
	x = SCIENCE_DATA_X + 10;
	y = SCIENCE_DATA_Y + 15;
	sng_current_draw_rectangle(w->window, gc, 0, SCIENCE_DATA_X, SCIENCE_DATA_Y,
					SCIENCE_DATA_W, SCIENCE_DATA_H);
	sprintf(buffer, "NAME: %s", o ? o->sdata.name : "");
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);

	if (o) {
		switch (o->type) {
		case OBJTYPE_SHIP2:
			sprintf(buffer, "TYPE: %s", shipclass[o->sdata.subclass]); 
			break;
		case OBJTYPE_STARBASE:
			sprintf(buffer, "TYPE: %s", "Starbase"); 
			break;
		case OBJTYPE_PLANET:
			sprintf(buffer, "TYPE: %s", "Asteroid"); 
			break;
		default:
			sprintf(buffer, "TYPE: %s", "Unknown"); 
			break;
		}
	} else  {
		sprintf(buffer, "TYPE:"); 
	}
	y += 25;
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);

	if (o)
		sprintf(buffer, "X: %8.2lf", o->x);
	else
		sprintf(buffer, "X:");
	y += 25;
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);

	if (o)
		sprintf(buffer, "Y: %8.2lf", o->y);
	else
		sprintf(buffer, "Y:");
	y += 25;
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);

	if (o) { 
		dx = go[my_ship_oid].x - o->x;
		dy = go[my_ship_oid].y - o->y;
		bearing = atan2(dx, dy) * 180 / M_PI;
		if (bearing < 0)
			bearing = -bearing;
		else
			bearing = 360.0 - bearing;
		sprintf(buffer, "BEARING: %3.2lf", bearing);
	} else {
		sprintf(buffer, "BEARING");
	}
	y += 25;
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);

	if (o) {
		range = sqrt(dx * dx + dy * dy);
		sprintf(buffer, "RANGE: %8.2lf", range);
	} else {
		sprintf(buffer, "RANGE:");
	}
	y += 25;
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);

	if (o)
		sprintf(buffer, "HEADING: %3.2lf", o->heading);
	else
		sprintf(buffer, "HEADING:");
	y += 25;
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);
#if 0
	sprintf(buffer, "STRENGTH: %hhu", o->sdata.shield_strength);
	y += 25;
	sng_abs_xy_draw_string(w, gd, buffer, TINY_FONT, x, y);
	sprintf(buffer, "WAVELENGTH: %hhu", o->sdata.shield_wavelength);
	y += 25;
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);
	sprintf(buffer, "WIDTH: %hhu", o->sdata.shield_width);
	y += 25;
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, x, y);
#endif

	gx1 = x;
	gy1 = y + 25;
	gx2 = SCIENCE_DATA_X + SCIENCE_DATA_W - 10;
	gy2 = SCIENCE_DATA_Y + SCIENCE_DATA_H - 40;
	draw_science_graph(w, ship, o, gx1, gy1, gx2, gy2);
}
 
static void show_science(GtkWidget *w)
{
	int /* rx, ry, rw, rh, */ cx, cy, r;
	struct snis_entity *o;
	char buf[80];
	double zoom;

	if (my_ship_id == UNKNOWN_ID)
		return;
	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return;
	o = &go[my_ship_oid];

	show_common_screen(w, "Science");
	if ((timer & 0x3f) == 0)
		wwviaudio_add_sound(SCIENCE_PROBE_SOUND);
	sng_set_foreground(GREEN);
	sprintf(buf, "Location: (%5.2lf, %5.2lf)  Heading: %3.1lf", o->x, o->y,
			360.0 * o->tsd.ship.sci_heading / (2.0 * 3.1415927));
	sng_abs_xy_draw_string(w, gc, buf, TINY_FONT, 250, 10 + LINEHEIGHT);
#if 0
	rx = SCIENCE_SCOPE_X;
	ry = SCIENCE_SCOPE_Y;
	rw = SCIENCE_SCOPE_W;
	rh = SCIENCE_SCOPE_H;
#endif
	cx = SCIENCE_SCOPE_CX;
	cy = SCIENCE_SCOPE_CY;
	r = SCIENCE_SCOPE_R;
	zoom = (MAX_SCIENCE_SCREEN_RADIUS - MIN_SCIENCE_SCREEN_RADIUS) *
			(o->tsd.ship.scizoom / 255.0) +
			MIN_SCIENCE_SCREEN_RADIUS;
	sng_set_foreground(DARKGREEN);
	snis_draw_radar_sector_labels(w, gc, o, cx, cy, r, zoom);
	snis_draw_radar_grid(w->window, gc, o, cx, cy, r, zoom, o->tsd.ship.scizoom < 50);
	sng_set_foreground(DARKRED);
	snis_draw_science_reticule(w, gc, cx, cy, r,
			o->tsd.ship.sci_heading, fabs(o->tsd.ship.sci_beam_width));
	draw_all_the_science_guys(w, o, zoom);
	draw_all_the_science_sparks(w, o, zoom);
	// snis_draw_sliders(w);
	draw_science_warp_data(w, o);
	draw_science_data(w, o, curr_science_guy);
}

static void show_comms(GtkWidget *w)
{
	show_common_screen(w, "Comms");
}

static void debug_draw_object(GtkWidget *w, struct snis_entity *o)
{
	int x, y, x1, y1, x2, y2;

	if (!o->alive)
		return;

	x = (int) ((double) SCREEN_WIDTH * o->x / XKNOWN_DIM);
	y = (int) ((double) SCREEN_HEIGHT * o->y / YKNOWN_DIM);
	x1 = x - 1;
	y2 = y + 1;
	y1 = y - 1;
	x2 = x + 1;

	switch (o->type) {
	case OBJTYPE_SHIP1:
	case OBJTYPE_SHIP2:
		if (o->id == my_ship_id)
			sng_set_foreground(GREEN);
		else
			sng_set_foreground(WHITE);
		break;
	case OBJTYPE_PLANET:
		sng_set_foreground(BLUE);
		break;
	case OBJTYPE_STARBASE:
		sng_set_foreground(MAGENTA);
		break;
	default:
		sng_set_foreground(WHITE);
	}
	snis_draw_line(w->window, gc, x1, y1, x2, y2);
	snis_draw_line(w->window, gc, x1, y2, x2, y1);
}

static void show_debug(GtkWidget *w)
{
	int x, y, i, ix, iy;
	const char *letters = "ABCDEFGHIJK";
	char label[10];
	int xoffset = 7;
	int yoffset = 10;
	char buffer[100];

	show_common_screen(w, "Debug");

	if (go[my_ship_oid].alive > 0)
		sng_set_foreground(GREEN);
	else
		sng_set_foreground(RED);

	ix = SCREEN_WIDTH / 10.0;
	for (x = 0; x <= 10; x++)
		snis_draw_dotted_vline(w->window, gc, x * ix, 0, SCREEN_HEIGHT, 5);

	iy = SCREEN_HEIGHT / 10.0;
	for (y = 0; y <= 10; y++)
		snis_draw_dotted_hline(w->window, gc, 0, y * iy, SCREEN_WIDTH, 5);

	for (x = 0; x < 10; x++)
		for (y = 0; y < 10; y++) {
			snprintf(label, sizeof(label), "%c%d", letters[y], x);
			sng_abs_xy_draw_string(w, gc, label, NANO_FONT, x * ix + xoffset, y * iy + yoffset); 
		}
	pthread_mutex_lock(&universe_mutex);

	for (i = 0; i <= snis_object_pool_highest_object(pool); i++)
		debug_draw_object(w, &go[i]);
	for (i = 0; i <= snis_object_pool_highest_object(sparkpool); i++)
		debug_draw_object(w, &spark[i]);
	pthread_mutex_unlock(&universe_mutex);

	sng_set_foreground(GREEN);
	sng_abs_xy_draw_string(w, gc, "SERVER NET STATS:", TINY_FONT, 10, SCREEN_HEIGHT - 40); 
	sprintf(buffer, "TX:%llu RX:%llu T=%lu SECS. BW=%llu BYTES/SEC",
		(unsigned long long) netstats.bytes_sent,
		(unsigned long long) netstats.bytes_recd, 
		(unsigned long) netstats.elapsed_seconds,
		(unsigned long long) (netstats.bytes_recd + netstats.bytes_sent) / netstats.elapsed_seconds);
	sng_abs_xy_draw_string(w, gc, buffer, TINY_FONT, 10, SCREEN_HEIGHT - 10); 
}

static void make_science_forget_stuff(void)
{
	int i;

	/* After awhile, science forgets... this is so the scientist
	 * can't just scan everything then sit back and relax for the
	 * rest of the game.
	 */
	pthread_mutex_lock(&universe_mutex);
	for (i = 0; i <= snis_object_pool_highest_object(pool); i++) {
		struct snis_entity *o = &go[i];
		if (o->sdata.science_data_known) /* forget after awhile */
			o->sdata.science_data_known--;
	}
	pthread_mutex_unlock(&universe_mutex);
}

static int main_da_scroll(GtkWidget *w, GdkEvent *event, gpointer p)
{
	struct _GdkEventScroll *e = (struct _GdkEventScroll *) event;
	struct snis_entity *o;
	int16_t newval;

	if (my_ship_oid == UNKNOWN_ID)
		my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
	if (my_ship_oid == UNKNOWN_ID)
		return 0;

	o = &go[my_ship_oid];

	switch (displaymode) {
	case DISPLAYMODE_SCIENCE:
		if (e->direction == GDK_SCROLL_UP)
			newval = o->tsd.ship.scizoom - 30;
		if (e->direction == GDK_SCROLL_DOWN)
			newval = o->tsd.ship.scizoom + 30;
		if (newval < 0)
			newval = 0;
		if (newval > 255)
			newval = 255;
		do_adjust_byte_value((uint8_t) newval, OPCODE_REQUEST_SCIZOOM);
		return 0;
	default:
		return 0;
	}
	return 0;
}

static int main_da_expose(GtkWidget *w, GdkEvent *event, gpointer p)
{
	sng_set_foreground(WHITE);
	
#if 0	
	for (i = 0; i <= highest_object_number;i++) {
		if (!game_state.go[i].alive)
			continue;
		if (onscreen(&game_state.go[i]))
			game_state.go[i].draw(&game_state.go[i], main_da); 
	}
#endif

	make_science_forget_stuff();

	if (displaymode < DISPLAYMODE_FONTTEST) {
		if (my_ship_id == UNKNOWN_ID)
			return 0;
		if (my_ship_oid == UNKNOWN_ID)
			my_ship_oid = (uint32_t) lookup_object_by_id(my_ship_id);
		if (my_ship_oid == UNKNOWN_ID)
			return 0;
		if (go[my_ship_oid].alive <= 0 && displaymode != DISPLAYMODE_DEBUG) {
			show_death_screen(w);
			return 0;
		}
	}
	switch (displaymode) {
	case DISPLAYMODE_FONTTEST:
		show_fonttest(w);
		break;
	case DISPLAYMODE_INTROSCREEN:
		show_introscreen(w);
		break;
	case DISPLAYMODE_LOBBYSCREEN:
		show_lobbyscreen(w);
		break;
	case DISPLAYMODE_CONNECTING:
		show_connecting_screen(w);
		break;
	case DISPLAYMODE_CONNECTED:
		show_connected_screen(w);
		break;
	case DISPLAYMODE_FINDSERVER:
		show_fonttest(w);
		break;
	case DISPLAYMODE_MAINSCREEN:
		show_mainscreen(w);
		break;
	case DISPLAYMODE_NAVIGATION:
		show_navigation(w);
		break;
	case DISPLAYMODE_WEAPONS:
		show_weapons(w);
		break;
	case DISPLAYMODE_ENGINEERING:
		show_engineering(w);
		break;
	case DISPLAYMODE_SCIENCE:
		show_science(w);
		break;
	case DISPLAYMODE_COMMS:
		show_comms(w);
		break;
	case DISPLAYMODE_DEBUG:
		show_debug(w);
		break;
	default:
		show_fonttest(w);
		break;
	}
	snis_draw_sliders(w, gc);
	ui_element_list_draw(w, gc, uiobjs);
	snis_draw_buttons(w, gc);
	text_window_draw_all(w, gc);
	return 0;
}

void really_quit(void);

gint advance_game(gpointer data)
{
	timer++;

#if 0
	for (i = 0; i <= highest_object_number; i++) {
		if (!game_state.go[i].alive)
			continue;
		game_state.go[i].move(&game_state.go[i]);
	}
	move_viewport();
#endif	
	gdk_threads_enter();
	gtk_widget_queue_draw(main_da);
	move_sparks();
	nframes++;
	gdk_threads_leave();
	if (in_the_process_of_quitting)
		really_quit();
	return TRUE;
}

/* call back for configure_event (for window resize) */
static gint main_da_configure(GtkWidget *w, GdkEventConfigure *event)
{
	GdkRectangle cliprect;

	/* first time through, gc is null, because gc can't be set without */
	/* a window, but, the window isn't there yet until it's shown, but */
	/* the show generates a configure... chicken and egg.  And we can't */
	/* proceed without gc != NULL...  but, it's ok, because 1st time thru */
	/* we already sort of know the drawing area/window size. */
 
	if (gc == NULL)
		return TRUE;

	real_screen_width =  w->allocation.width;
	real_screen_height =  w->allocation.height;
	xscale_screen = (float) real_screen_width / (float) SCREEN_WIDTH;
	yscale_screen = (float) real_screen_height / (float) SCREEN_HEIGHT;
	sng_set_scale(xscale_screen, yscale_screen);
	if (real_screen_width == 800 && real_screen_height == 600) {
		sng_use_unscaled_drawing_functions();
	} else {
		sng_use_scaled_drawing_functions();
		if (thicklines)
			sng_use_thick_lines();
	}
	gdk_gc_set_clip_origin(gc, 0, 0);
	cliprect.x = 0;	
	cliprect.y = 0;	
	cliprect.width = real_screen_width;	
	cliprect.height = real_screen_height;	
	gdk_gc_set_clip_rectangle(gc, &cliprect);
	return TRUE;
}

static int main_da_button_press(GtkWidget *w, GdkEventButton *event,
	__attribute__((unused)) void *unused)
		
{
	switch (displaymode) {
	case DISPLAYMODE_LOBBYSCREEN:
		lobbylast1clickx = (int) ((0.0 + event->x) / (0.0 + real_screen_width) * SCREEN_WIDTH);
		lobbylast1clicky = (int) ((0.0 + event->y) / (0.0 + real_screen_height) * SCREEN_HEIGHT);
		
		return TRUE;
	case DISPLAYMODE_SCIENCE:
		science_button_press((int) ((0.0 + event->x) / (0.0 + real_screen_width) * SCREEN_WIDTH),
				(int) ((0.0 + event->y) / (0.0 + real_screen_height) * SCREEN_HEIGHT));
		break;
	default:
		break;
	}
	ui_element_list_button_press(uiobjs,
		(int) ((0.0 + event->x) / (0.0 + real_screen_width) * SCREEN_WIDTH),
		(int) ((0.0 + event->y) / (0.0 + real_screen_height) * SCREEN_HEIGHT));
	snis_sliders_button_press((int) ((0.0 + event->x) / (0.0 + real_screen_width) * SCREEN_WIDTH),
			(int) ((0.0 + event->y) / (0.0 + real_screen_height) * SCREEN_HEIGHT));
	snis_buttons_button_press((int) ((0.0 + event->x) / (0.0 + real_screen_width) * SCREEN_WIDTH),
			(int) ((0.0 + event->y) / (0.0 + real_screen_height) * SCREEN_HEIGHT));
	return TRUE;
}

static gboolean delete_event(GtkWidget *widget, 
	GdkEvent *event, gpointer data)
{
    /* If you return FALSE in the "delete_event" signal handler,
     * GTK will emit the "destroy" signal. Returning TRUE means
     * you don't want the window to be destroyed.
     * This is useful for popping up 'are you sure you want to quit?'
     * type dialogs. */

    /* Change TRUE to FALSE and the main window will be destroyed with
     * a "delete_event". */
    gettimeofday(&end_time, NULL);
    printf("%d frames / %d seconds, %g frames/sec\n", 
		nframes, (int) (end_time.tv_sec - start_time.tv_sec),
		(0.0 + nframes) / (0.0 + end_time.tv_sec - start_time.tv_sec));
    return FALSE;
}

static void destroy(GtkWidget *widget, gpointer data)
{
    gtk_main_quit ();
}

void really_quit(void)
{
	gettimeofday(&end_time, NULL);
	printf("%d frames / %d seconds, %g frames/sec\n",
		nframes, (int) (end_time.tv_sec - start_time.tv_sec),
		(0.0 + nframes) / (0.0 + end_time.tv_sec - start_time.tv_sec));
	printf("server netstats: %llu bytes sent, %llu bytes recd, secs = %llu, bw = %llu bytes/sec\n",
			netstats.bytes_sent, netstats.bytes_recd, (unsigned long long) netstats.elapsed_seconds,
			(netstats.bytes_sent + netstats.bytes_recd) / netstats.elapsed_seconds);
        wwviaudio_cancel_all_sounds();
        wwviaudio_stop_portaudio();
	exit(1); /* probably bad form... oh well. */
}

static void usage(void)
{
	fprintf(stderr, "usage: snis_client lobbyhost starship password\n");
	fprintf(stderr, "       Example: ./snis_client localhost Enterprise tribbles\n");
	exit(1);
}

static void read_sound_clips(void)
{
	printf("Decoding audio data..."); fflush(stdout);
	wwviaudio_read_ogg_clip(EXPLOSION_SOUND, "share/big_explosion.ogg");
	wwviaudio_read_ogg_clip(TORPEDO_LAUNCH_SOUND, "share/flak_gun_sound.ogg");
	wwviaudio_read_ogg_clip(LASER_FIRE_SOUND, "share/bigshotlaser.ogg");
	wwviaudio_read_ogg_clip(ONSCREEN_SOUND, "share/onscreen.ogg");
	wwviaudio_read_ogg_clip(OFFSCREEN_SOUND, "share/offscreen.ogg");
	wwviaudio_read_ogg_clip(CHANGESCREEN_SOUND, "share/changescreen.ogg");
	wwviaudio_read_ogg_clip(SLIDER_SOUND, "share/slider-noise.ogg");
	wwviaudio_read_ogg_clip(SCIENCE_DATA_ACQUIRED_SOUND, "share/science-data-acquired.ogg");
	wwviaudio_read_ogg_clip(SCIENCE_PROBE_SOUND, "share/science-probe.ogg");
	wwviaudio_read_ogg_clip(TTY_CHATTER_SOUND, "share/tty-chatter.ogg");
	printf("Done.\n");
}

static void setup_sound(void)
{
	if (wwviaudio_initialize_portaudio(MAX_CONCURRENT_SOUNDS, NSOUND_CLIPS) != 0) {
		printf("Failed to initialize sound system\n");
		return;
	}
	read_sound_clips();
}

int main(int argc, char *argv[])
{
	GtkWidget *vbox;
	int i;

	if (argc < 3)
		usage();

	lobbyhost = argv[1];
	shipname = argv[2];
	password = argv[3];

	role = 0;
	for (i = 4; i < argc; i++) {
		if (strcmp(argv[i], "--allroles") == 0)
			role |= ROLE_ALL;
		if (strcmp(argv[i], "--main") == 0)
			role |= ROLE_MAIN;
		if (strcmp(argv[i], "--navigation") == 0)
			role |= ROLE_NAVIGATION;
		if (strcmp(argv[i], "--weapons") == 0)
			role |= ROLE_WEAPONS;
		if (strcmp(argv[i], "--engineering") == 0)
			role |= ROLE_ENGINEERING;
		if (strcmp(argv[i], "--science") == 0)
			role |= ROLE_SCIENCE;
		if (strcmp(argv[i], "--comms") == 0)
			role |= ROLE_COMMS;
		if (strcmp(argv[i], "--debug") == 0)
			role |= ROLE_DEBUG;
		if (strcmp(argv[i], "--soundserver") == 0)
			role |= ROLE_SOUNDSERVER;
	}
	if (role == 0)
		role = ROLE_ALL;

	snis_object_pool_setup(&pool, MAXGAMEOBJS);
	snis_object_pool_setup(&sparkpool, MAXSPARKS);

	ignore_sigpipe();

	setup_sound();

	connect_to_lobby();
	real_screen_width = SCREEN_WIDTH;
	real_screen_height = SCREEN_HEIGHT;
	sng_set_scale(xscale_screen, yscale_screen);

	gtk_set_locale();
	gtk_init (&argc, &argv);

	init_keymap();
#if 0
	init_vects();
	init_player();
	init_game_state(the_player);
#endif

	snis_typefaces_init();

	window = gtk_window_new (GTK_WINDOW_TOPLEVEL);		
	gtk_container_set_border_width (GTK_CONTAINER (window), 0);
	vbox = gtk_vbox_new(FALSE, 0);
        main_da = gtk_drawing_area_new();

	g_signal_connect (G_OBJECT (window), "delete_event",
		G_CALLBACK (delete_event), NULL);
	g_signal_connect (G_OBJECT (window), "destroy",
		G_CALLBACK (destroy), NULL);
	g_signal_connect(G_OBJECT (window), "key_press_event",
		G_CALLBACK (key_press_cb), "window");
	g_signal_connect(G_OBJECT (window), "key_release_event",
		G_CALLBACK (key_release_cb), "window");
	g_signal_connect(G_OBJECT (main_da), "expose_event",
		G_CALLBACK (main_da_expose), NULL);
        g_signal_connect(G_OBJECT (main_da), "configure_event",
		G_CALLBACK (main_da_configure), NULL);
        g_signal_connect(G_OBJECT (main_da), "scroll_event",
		G_CALLBACK (main_da_scroll), NULL);
	gtk_widget_add_events(main_da, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(G_OBJECT (main_da), "button_press_event",
                      G_CALLBACK (main_da_button_press), NULL);

	gtk_container_add (GTK_CONTAINER (window), vbox);
	gtk_box_pack_start(GTK_BOX (vbox), main_da, TRUE /* expand */, TRUE /* fill */, 0);

        gtk_window_set_default_size(GTK_WINDOW(window), real_screen_width, real_screen_height);

        gtk_widget_show (vbox);
        gtk_widget_show (main_da);
        gtk_widget_show (window);

	sng_setup_colors(main_da);

        gc = gdk_gc_new(GTK_WIDGET(main_da)->window);
	sng_set_gc(gc);
	sng_set_foreground(WHITE);

	timer_tag = g_timeout_add(1000 / frame_rate_hz, advance_game, NULL);

	/* Apparently (some versions of?) portaudio calls g_thread_init(). */
	/* It may only be called once, and subsequent calls abort, so */
	/* only call it if the thread system is not already initialized. */
	if (!g_thread_supported ())
		g_thread_init(NULL);
	gdk_threads_init();

	gettimeofday(&start_time, NULL);

	snis_slider_set_sound(SLIDER_SOUND);
	text_window_set_chatter_sound(TTY_CHATTER_SOUND);
	text_window_set_timer(&timer);
	init_trig_arrays();
	init_nav_ui();
	init_engineering_ui();
	init_weapons_ui();
	init_science_ui();
	init_comms_ui();

	gtk_main ();
        wwviaudio_cancel_all_sounds();
        wwviaudio_stop_portaudio();
	return 0;
}
