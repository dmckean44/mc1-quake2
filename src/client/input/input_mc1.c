/*
 * Copyright (C) 2024 Marcus Geelnard
 * Copyright (C) 2010 Yamagi Burmeister
 * Copyright (C) 1997-2005 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * This is the Quake II input system backend, implemented for MC1.
 *
 * =======================================================================
 */

#include <mc1/keyboard.h>
#include <mc1/mmio.h>

#include "header/input.h"
#include "../header/keyboard.h"
#include "../header/client.h"

// ----

// Maximal mouse move per frame
#define MOUSE_MAX 3000

// Minimal mouse move per frame
#define MOUSE_MIN 40

// ----

enum {
	LAYOUT_DEFAULT			= 0,
	LAYOUT_SOUTHPAW,
	LAYOUT_LEGACY,
	LAYOUT_LEGACY_SOUTHPAW,
	LAYOUT_FLICK_STICK,
	LAYOUT_FLICK_STICK_SOUTHPAW
};

typedef struct
{
	float x;
	float y;
} thumbstick_t;

typedef enum
{
	REASON_NONE,
	REASON_CONTROLLERINIT,
	REASON_GYROCALIBRATION
} updates_countdown_reasons;

// ----

// These are used to communicate the events collected by
// IN_Update() called at the beginning of a frame to the
// actual movement functions called at a later time.
static float mouse_x, mouse_y;
static qboolean mlooking;

// The last time input events were processed.
// Used throughout the client.
int sys_frame_time;

// the joystick altselector that turns K_BTN_X into K_BTN_X_ALT
// is pressed
qboolean joy_altselector_pressed = false;

// Console Variables
cvar_t *freelook;
cvar_t *lookstrafe;
cvar_t *m_forward;
cvar_t *m_pitch;
cvar_t *m_side;
cvar_t *m_up;
cvar_t *m_yaw;
static cvar_t *sensitivity;

static cvar_t *exponential_speedup;
static cvar_t *in_grab;
static cvar_t *m_filter;
static cvar_t *windowed_pauseonfocuslost;
static cvar_t *windowed_mouse;

cvar_t *joy_layout;

// Gyro mode (0=off, 3=on, 1-2=uses button to enable/disable)
cvar_t *gyro_mode;
cvar_t *gyro_turning_axis;	// yaw or roll

// ----

qboolean show_gamepad = false, show_haptic = false, show_gyro = false;

/* ------------------------------------------------------------------ */

static int
IN_TranslateMC1toQ2Key(uint32_t keysym)
{
	int key = 0;

#define MY_SC_CASE(X) case KB_ ## X : return K_SC_ ## X;

	switch (keysym)
	{
		// case KB_A : return K_SC_A;
		MY_SC_CASE(A)
		MY_SC_CASE(B)
		MY_SC_CASE(C)
		MY_SC_CASE(D)
		MY_SC_CASE(E)
		MY_SC_CASE(F)
		MY_SC_CASE(G)
		MY_SC_CASE(H)
		MY_SC_CASE(I)
		MY_SC_CASE(J)
		MY_SC_CASE(K)
		MY_SC_CASE(L)
		MY_SC_CASE(M)
		MY_SC_CASE(N)
		MY_SC_CASE(O)
		MY_SC_CASE(P)
		MY_SC_CASE(Q)
		MY_SC_CASE(R)
		MY_SC_CASE(S)
		MY_SC_CASE(T)
		MY_SC_CASE(U)
		MY_SC_CASE(V)
		MY_SC_CASE(W)
		MY_SC_CASE(X)
		MY_SC_CASE(Y)
		MY_SC_CASE(Z)

		case KB_TAB:
			key = K_TAB;
			break;
		case KB_ENTER:
			key = K_ENTER;
			break;
		case KB_ESC:
			key = K_ESCAPE;
			break;
		case KB_BACKSPACE:
			key = K_BACKSPACE;
			break;
		case KB_LMETA:
		case KB_RMETA:
			key = K_COMMAND; // Win key
			break;
		case KB_ACPI_POWER:
			key = K_POWER;
			break;
		case KB_MM_PLAY_PAUSE:
			key = K_PAUSE;
			break;

		case KB_UP:
			key = K_UPARROW;
			break;
		case KB_DOWN:
			key = K_DOWNARROW;
			break;
		case KB_LEFT:
			key = K_LEFTARROW;
			break;
		case KB_RIGHT:
			key = K_RIGHTARROW;
			break;

		case KB_RALT:
		case KB_LALT:
			key = K_ALT;
			break;
		case KB_LCTRL:
		case KB_RCTRL:
			key = K_CTRL;
			break;
		case KB_LSHIFT:
		case KB_RSHIFT:
			key = K_SHIFT;
			break;
		case KB_INSERT:
			key = K_INS;
			break;
		case KB_DEL:
			key = K_DEL;
			break;
		case KB_PGDN:
			key = K_PGDN;
			break;
		case KB_PGUP:
			key = K_PGUP;
			break;
		case KB_HOME:
			key = K_HOME;
			break;
		case KB_END:
			key = K_END;
			break;

		case KB_F1:
			key = K_F1;
			break;
		case KB_F2:
			key = K_F2;
			break;
		case KB_F3:
			key = K_F3;
			break;
		case KB_F4:
			key = K_F4;
			break;
		case KB_F5:
			key = K_F5;
			break;
		case KB_F6:
			key = K_F6;
			break;
		case KB_F7:
			key = K_F7;
			break;
		case KB_F8:
			key = K_F8;
			break;
		case KB_F9:
			key = K_F9;
			break;
		case KB_F10:
			key = K_F10;
			break;
		case KB_F11:
			key = K_F11;
			break;
		case KB_F12:
			key = K_F12;
			break;

		case KB_KP_7:
			key = K_KP_HOME;
			break;
		case KB_KP_8:
			key = K_KP_UPARROW;
			break;
		case KB_KP_9:
			key = K_KP_PGUP;
			break;
		case KB_KP_4:
			key = K_KP_LEFTARROW;
			break;
		case KB_KP_5:
			key = K_KP_5;
			break;
		case KB_KP_6:
			key = K_KP_RIGHTARROW;
			break;
		case KB_KP_1:
			key = K_KP_END;
			break;
		case KB_KP_2:
			key = K_KP_DOWNARROW;
			break;
		case KB_KP_3:
			key = K_KP_PGDN;
			break;
		case KB_KP_ENTER:
			key = K_KP_ENTER;
			break;
		case KB_KP_0:
			key = K_KP_INS;
			break;
		case KB_KP_PERIOD:
			key = K_KP_DEL;
			break;
		case KB_KP_DIV:
			key = K_KP_SLASH;
			break;
		case KB_KP_MINUS:
			key = K_KP_MINUS;
			break;
		case KB_KP_PLUS:
			key = K_KP_PLUS;
			break;
		case KB_KP_MUL:
			key = K_KP_STAR;
			break;

		default:
			break;
	}

#undef MY_SC_CASE

	return key;
}

/* ------------------------------------------------------------------ */

qboolean IN_NumpadIsOn()
{
    return true;
}

/*
 * Updates the input queue state. Called every
 * frame by the client and does nearly all the
 * input magic.
 */
void
IN_Update(void)
{
	// We need to save the frame time so other subsystems
	// know the exact time of the last input events.
	sys_frame_time = Sys_Milliseconds();

	/* Process keyboard events */
	/* TODO(m): This is very incomplete and probably wrong.  */
	kb_poll();
	uint32_t event;
	while ((event = kb_get_next_event()) != 0U)
	{
		uint32_t key = IN_TranslateMC1toQ2Key(kb_event_scancode(event));

		if (kb_event_is_press(event)) {
			uint32_t c = kb_event_to_char(event);
			if (c != 0U) {
				Char_Event((int)c);
			} else {
				Key_Event(key, true, false);
			}
		} else {
			Key_Event(key, false, false);
		}
	}

	/* Get the mouse position and calculate the mouse delta */
	int mouse_dx;
	int mouse_dy;
	{
		static uint16_t old_x;
		static uint16_t old_y;
		uint32_t mouse_pos = MMIO(MOUSEPOS);
		uint16_t x = (uint16_t)(mouse_pos >> 16);
		uint16_t y = (uint16_t)(mouse_pos & 0xffffU);
		mouse_dx = (int)(int16_t)(x - old_x);
		mouse_dy = (int)(int16_t)(y - old_y);
		old_x = x;
		old_y = y;
	}

	/* Increment the global mouse coordinate (used by IN_Move) */
	mouse_x += (float)mouse_dx;
	mouse_y += (float)mouse_dy;
}

/*
 * Move handling
 */
void
IN_Move(usercmd_t *cmd)
{
	static float old_mouse_x;
	static float old_mouse_y;

	if (m_filter->value)
	{
		if ((mouse_x > 1) || (mouse_x < -1))
		{
			mouse_x = (mouse_x + old_mouse_x) * 0.5;
		}

		if ((mouse_y > 1) || (mouse_y < -1))
		{
			mouse_y = (mouse_y + old_mouse_y) * 0.5;
		}
	}

	old_mouse_x = mouse_x;
	old_mouse_y = mouse_y;

	if (mouse_x || mouse_y)
	{
		if (!exponential_speedup->value)
		{
			mouse_x *= sensitivity->value;
			mouse_y *= sensitivity->value;
		}
		else
		{
			if ((mouse_x > MOUSE_MIN) || (mouse_y > MOUSE_MIN) ||
				(mouse_x < -MOUSE_MIN) || (mouse_y < -MOUSE_MIN))
			{
				mouse_x = (mouse_x * mouse_x * mouse_x) / 4;
				mouse_y = (mouse_y * mouse_y * mouse_y) / 4;

				if (mouse_x > MOUSE_MAX)
				{
					mouse_x = MOUSE_MAX;
				}
				else if (mouse_x < -MOUSE_MAX)
				{
					mouse_x = -MOUSE_MAX;
				}

				if (mouse_y > MOUSE_MAX)
				{
					mouse_y = MOUSE_MAX;
				}
				else if (mouse_y < -MOUSE_MAX)
				{
					mouse_y = -MOUSE_MAX;
				}
			}
		}

		// add mouse X/Y movement to cmd
		if ((in_strafe.state & 1) || (lookstrafe->value && mlooking))
		{
			cmd->sidemove += m_side->value * mouse_x;
		}
		else
		{
			cl.viewangles[YAW] -= m_yaw->value * mouse_x;
		}

		if ((mlooking || freelook->value) && !(in_strafe.state & 1))
		{
			cl.viewangles[PITCH] += m_pitch->value * mouse_y;
		}
		else
		{
			cmd->forwardmove -= m_forward->value * mouse_y;
		}

		mouse_x = mouse_y = 0;
	}
}

/* ------------------------------------------------------------------ */

/*
 * Look down
 */
static void
IN_MLookDown(void)
{
	mlooking = true;
}

/*
 * Look up
 */
static void
IN_MLookUp(void)
{
	mlooking = false;
	IN_CenterView();
}

/*
 * Removes all pending events from the queue.
 */
void
In_FlushQueue(void)
{
	kb_poll();
	while (kb_get_next_event()) ;
}

/* ------------------------------------------------------------------ */

/*
 * Initializes the backend
 */
void
IN_Init(void)
{
	Com_Printf("------- input initialization -------\n");

	kb_init();
	mouse_x = mouse_y = 0;

	exponential_speedup = Cvar_Get("exponential_speedup", "0", CVAR_ARCHIVE);
	freelook = Cvar_Get("freelook", "1", CVAR_ARCHIVE);
	in_grab = Cvar_Get("in_grab", "2", CVAR_ARCHIVE);
	lookstrafe = Cvar_Get("lookstrafe", "0", CVAR_ARCHIVE);
	m_filter = Cvar_Get("m_filter", "0", CVAR_ARCHIVE);
	m_up = Cvar_Get("m_up", "1", CVAR_ARCHIVE);
	m_forward = Cvar_Get("m_forward", "1", CVAR_ARCHIVE);
	m_pitch = Cvar_Get("m_pitch", "0.022", CVAR_ARCHIVE);
	m_side = Cvar_Get("m_side", "0.8", CVAR_ARCHIVE);
	m_yaw = Cvar_Get("m_yaw", "0.022", CVAR_ARCHIVE);
	sensitivity = Cvar_Get("sensitivity", "3", CVAR_ARCHIVE);

	joy_layout = Cvar_Get("joy_layout", "0", CVAR_ARCHIVE);

	gyro_mode = Cvar_Get("gyro_mode", "2", CVAR_ARCHIVE);
	gyro_turning_axis = Cvar_Get("gyro_turning_axis", "0", CVAR_ARCHIVE);

	windowed_pauseonfocuslost = Cvar_Get("vid_pauseonfocuslost", "0", CVAR_USERINFO | CVAR_ARCHIVE);
	windowed_mouse = Cvar_Get("windowed_mouse", "1", CVAR_USERINFO | CVAR_ARCHIVE);

	Cmd_AddCommand("+mlook", IN_MLookDown);
	Cmd_AddCommand("-mlook", IN_MLookUp);

	Com_Printf("------------------------------------\n\n");
}

/*
 * Shuts the backend down
 */
void
IN_Shutdown(void)
{
	Com_Printf("Shutting down input.\n");

	Cmd_RemoveCommand("+mlook");
	Cmd_RemoveCommand("-mlook");
}

/*
 * Gyro calibration functions, called from menu
 */
void
StartCalibration(void)
{
}

qboolean
IsCalibrationZero(void)
{
	return true;
}

/*
 * Controller
 */
void
Controller_Rumble(const char *, vec3_t, qboolean,
		unsigned int, unsigned short int)
{
}

void
Haptic_Feedback(const char *, int, int,
				int, int, int,
				int, int, int, float)
{
}

/* ------------------------------------------------------------------ */
