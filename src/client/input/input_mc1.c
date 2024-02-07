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
IN_TranslateScancode(const uint32_t scancode)
{
	switch (scancode)
	{
		case KB_A: return K_SC_A;
		case KB_B: return K_SC_B;
		case KB_C: return K_SC_C;
		case KB_D: return K_SC_D;
		case KB_E: return K_SC_E;
		case KB_F: return K_SC_F;
		case KB_G: return K_SC_G;
		case KB_H: return K_SC_H;
		case KB_I: return K_SC_I;
		case KB_J: return K_SC_J;
		case KB_K: return K_SC_K;
		case KB_L: return K_SC_L;
		case KB_M: return K_SC_M;
		case KB_N: return K_SC_N;
		case KB_O: return K_SC_O;
		case KB_P: return K_SC_P;
		case KB_Q: return K_SC_Q;
		case KB_R: return K_SC_R;
		case KB_S: return K_SC_S;
		case KB_T: return K_SC_T;
		case KB_U: return K_SC_U;
		case KB_V: return K_SC_V;
		case KB_W: return K_SC_W;
		case KB_X: return K_SC_X;
		case KB_Y: return K_SC_Y;
		case KB_Z: return K_SC_Z;

		case KB_TAB: return K_TAB;
		case KB_ENTER: return K_ENTER;
		case KB_ESC: return K_ESCAPE;
		case KB_BACKSPACE: return K_BACKSPACE;
		case KB_LMETA:
		case KB_RMETA: return K_COMMAND; // Win key
		case KB_ACPI_POWER: return K_POWER;
		case KB_MM_PLAY_PAUSE: return K_PAUSE;

		case KB_UP: return K_UPARROW;
		case KB_DOWN: return K_DOWNARROW;
		case KB_LEFT: return K_LEFTARROW;
		case KB_RIGHT: return K_RIGHTARROW;

		case KB_RALT:
		case KB_LALT: return K_ALT;
		case KB_LCTRL:
		case KB_RCTRL: return K_CTRL;
		case KB_LSHIFT:
		case KB_RSHIFT: return K_SHIFT;
		case KB_INSERT: return K_INS;
		case KB_DEL: return K_DEL;
		case KB_PGDN: return K_PGDN;
		case KB_PGUP: return K_PGUP;
		case KB_HOME: return K_HOME;
		case KB_END: return K_END;

		case KB_F1: return K_F1;
		case KB_F2: return K_F2;
		case KB_F3: return K_F3;
		case KB_F4: return K_F4;
		case KB_F5: return K_F5;
		case KB_F6: return K_F6;
		case KB_F7: return K_F7;
		case KB_F8: return K_F8;
		case KB_F9: return K_F9;
		case KB_F10: return K_F10;
		case KB_F11: return K_F11;
		case KB_F12: return K_F12;

		case KB_KP_7: return K_KP_HOME;
		case KB_KP_8: return K_KP_UPARROW;
		case KB_KP_9: return K_KP_PGUP;
		case KB_KP_4: return K_KP_LEFTARROW;
		case KB_KP_5: return K_KP_5;
		case KB_KP_6: return K_KP_RIGHTARROW;
		case KB_KP_1: return K_KP_END;
		case KB_KP_2: return K_KP_DOWNARROW;
		case KB_KP_3: return K_KP_PGDN;
		case KB_KP_ENTER: return K_KP_ENTER;
		case KB_KP_0: return K_KP_INS;
		case KB_KP_PERIOD: return K_KP_DEL;
		case KB_KP_DIV: return K_KP_SLASH;
		case KB_KP_MINUS: return K_KP_MINUS;
		case KB_KP_PLUS: return K_KP_PLUS;
		case KB_KP_MUL: return K_KP_STAR;

		default:
			return 0;
	}
}

static void
IN_HandleKeyboardEvents(void)
{
	kb_poll();
	uint32_t event;
	while ((event = kb_get_next_event()) != 0U)
	{
		const uint32_t sc = kb_event_scancode(event);
		const uint32_t key = IN_TranslateScancode(sc);
		const uint32_t c = kb_event_to_char(event);
		const qboolean press = kb_event_is_press(event);

		if (c != 0U && press)
		{
			Char_Event((int)c);
		}

		if (key != 0)
		{
			/* Most non-printable characters are considered special */
			const qboolean special = (c == 0U) || !(c >= 32 && c <= 126);

			Key_Event(key, press, special);
		}
	}
}

static void
IN_HandleOneMouseButton(const uint32_t btns, const uint32_t old_btns, unsigned bit, int key)
{
	const unsigned mask = 1U << bit;
	const unsigned state = btns & mask;
	const unsigned old_state = old_btns & mask;
	if (state != old_state)
	{
		Key_Event(key, state != 0U, true);
	}
}

static void
IN_HandleMouseButtonEvents(void)
{
	static uint32_t old_btns;
	const uint32_t btns = MMIO(MOUSEBTNS);
	IN_HandleOneMouseButton(btns, old_btns, 0, K_MOUSE1);
	IN_HandleOneMouseButton(btns, old_btns, 1, K_MOUSE2);
	IN_HandleOneMouseButton(btns, old_btns, 2, K_MOUSE3);
	old_btns = btns;
}

static void
IN_HandleMouseMovementEvents(void)
{
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

	/* Process input events */
	IN_HandleKeyboardEvents();
	IN_HandleMouseButtonEvents();
	IN_HandleMouseMovementEvents();
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
