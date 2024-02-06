/*
 * Copyright (C) 1997-2001 Id Software, Inc.
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
 * Low level network code for MC1. We only implement the LOOPBACK type.
 *
 * =======================================================================
 */

#include "../../common/header/common.h"

#define MAX_LOOPBACK 4

typedef struct
{
	byte data[MAX_MSGLEN];
	int datalen;
} loopmsg_t;

typedef struct
{
	loopmsg_t msgs[MAX_LOOPBACK];
	int get, send;
} loopback_t;

static loopback_t loopbacks[2];

static netadr_t net_local_adr;

void
NET_Init()
{
	/* Set the local address to something meaningful */
	net_local_adr.ip[0] = 127;
	net_local_adr.ip[1] = 0;
	net_local_adr.ip[2] = 0;
	net_local_adr.ip[3] = 1;
	net_local_adr.type = NA_LOOPBACK;
}

qboolean
NET_CompareAdr(netadr_t a, netadr_t b)
{
	if (a.type != b.type)
	{
		return false;
	}

	if (a.type == NA_LOOPBACK)
	{
		return true;
	}

	if (a.type == NA_IP)
	{
		if ((a.ip[0] == b.ip[0]) && (a.ip[1] == b.ip[1]) &&
			(a.ip[2] == b.ip[2]) && (a.ip[3] == b.ip[3]) && (a.port == b.port))
		{
			return true;
		}
	}

	if (a.type == NA_IP6)
	{
		if ((memcmp(a.ip, b.ip, 16) == 0) && (a.port == b.port))
		{
			return true;
		}
	}

	return false;
}

/*
 * Compares without the port
 */
qboolean
NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
	if (a.type != b.type)
	{
		return false;
	}

	if (a.type == NA_LOOPBACK)
	{
		return true;
	}

	if (a.type == NA_IP)
	{
		if ((a.ip[0] == b.ip[0]) && (a.ip[1] == b.ip[1]) &&
			(a.ip[2] == b.ip[2]) && (a.ip[3] == b.ip[3]))
		{
			return true;
		}

		return false;
	}

	if (a.type == NA_IP6)
	{
		if ((memcmp(a.ip, b.ip, 16) == 0))
		{
			return true;
		}

		return false;
	}

	if (a.type == NA_IPX)
	{
		if ((memcmp(a.ipx, b.ipx, 10) == 0))
		{
			return true;
		}

		return false;
	}

	return false;
}

char *
NET_AdrToString(netadr_t a)
{
	static char s[64];

	switch (a.type)
	{
		case NA_IP:
		case NA_LOOPBACK:
			Com_sprintf(s, sizeof(s), "%i.%i.%i.%i", a.ip[0],
				a.ip[1], a.ip[2], a.ip[3]);
			break;

		case NA_BROADCAST:
			Com_sprintf(s, sizeof(s), "255.255.255.255");
			break;

		default:
			Com_sprintf(s, sizeof(s), "invalid IP address family type");
			break;
	}

	return s;
}

/*
 * This is a hackish implementation, in lack of proper parsing routines.
 */
qboolean
NET_StringToAdr(const char *s, netadr_t *a)
{
	memset(a, 0, sizeof(*a));

	if (!strcmp(s, "localhost"))
	{
		a->type = NA_LOOPBACK;
		return true;
	}

	/* Parse base address */
	int pos = 0;
	int idx = 0;
	int num = 0;
	for (; idx < 4; ++pos)
	{
		int c = s[pos];
		if (c >= '0' && c <= '9')
		{
			num = (num * 10) + (c - '0');
		}
		else if (c == '.' || c == ':' || c == 0)
		{
			a->ip[idx++] = num;
			num = 0;
			if (c != '.')
			{
				break;
			}
		}
		else
		{
			Com_Printf("NET_StringToAdr: Unsupported address '%s'\n", s);
			return false;
		}
	}
	if (idx != 4)
	{
		Com_Printf("NET_StringToAdr: Unsupported address '%s'\n", s);
		return false;
	}

	/* Parse port */
	int port = 0;
	if (s[pos] == ':')
	{
		++pos;
		for (; s[pos] != 0; ++pos)
		{
			int c = s[pos];
			if (c >= '0' && c <= '9')
			{
				port = (port * 10) + (c - '0');
			}
			else
			{
				Com_Printf("NET_StringToAdr: Unsupported address '%s'\n", s);
				return false;
			}
		}
	}
	a->port = port;

	/* We only support IPv4 */
	a->type = NA_IP;

	return true;
}

qboolean
NET_IsLocalAddress(netadr_t adr)
{
	return NET_CompareAdr(adr, net_local_adr);
}

qboolean
NET_GetPacket(netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
	int i;
	loopback_t *loop;

	loop = &loopbacks[sock];

	if (loop->send - loop->get > MAX_LOOPBACK)
	{
		loop->get = loop->send - MAX_LOOPBACK;
	}

	if (loop->get >= loop->send)
	{
		return false;
	}

	i = loop->get & (MAX_LOOPBACK - 1);
	loop->get++;

	memcpy(net_message->data, loop->msgs[i].data, loop->msgs[i].datalen);
	net_message->cursize = loop->msgs[i].datalen;
	*net_from = net_local_adr;
	return true;
}

void
NET_SendPacket(netsrc_t sock, int length, void *data, netadr_t to)
{
	int i;
	loopback_t *loop;

	if (to.type == NA_LOOPBACK)
	{
		loop = &loopbacks[sock ^ 1];

		i = loop->send & (MAX_LOOPBACK - 1);
		loop->send++;

		memcpy(loop->msgs[i].data, data, length);
		loop->msgs[i].datalen = length;
	}
	else
	{
		Com_Printf("NET_SendPacket: Unsupported address type\n");
		return;
	}
}

/*
 * A single player game will only use the loopback code
 */
void
NET_Config(qboolean multiplayer)
{
	(void)multiplayer;
}

/* =================================================================== */

void
NET_Shutdown(void)
{
	NET_Config(false);
}

/*
 * sleeps msec or until net socket is ready
 */
void
NET_Sleep(int msec)
{
	(void)msec;
}

