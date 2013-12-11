/* Copyright (c) 2010
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Sadrul Habib Chowdhury (sadrul@users.sourceforge.net)
 * Copyright (c) 2008, 2009
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 *      Micah Cowan (micah@cowan.name)
 *      Sadrul Habib Chowdhury (sadrul@users.sourceforge.net)
 * Copyright (c) 1993-2002, 2003, 2005, 2006, 2007
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 * Copyright (c) 1987 Oliver Laumann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, see
 * http://www.gnu.org/licenses/, or contact Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 ****************************************************************
 */

#include "winmsg.h"
#include "fileio.h"
#include "process.h"
#include "sched.h"
#include "mark.h"

/* TODO: rid global variable */
WinMsgBuf *winmsg;

#define CHRPAD 127

/* redundant definition abstraction for escape character handlers; note that
 * a variable varadic macro name is a gcc extension and is not portable, so
 * we instead use two separate macros */
#define WINMSG_ESC_PARAMS \
	__attribute__((unused)) WinMsgEsc *esc, \
	char *s, \
	WinMsgBufContext *wmbc, \
	__attribute__((unused)) WinMsgCond *cond
#define winmsg_esc__name(name) __WinMsgEsc##name
#define winmsg_esc__def(name) static char *winmsg_esc__name(name)
#define winmsg_esc(name) winmsg_esc__def(name)(WINMSG_ESC_PARAMS)
#define winmsg_esc_ex(name, ...) winmsg_esc__def(name)(WINMSG_ESC_PARAMS, __VA_ARGS__)
#define WINMSG_ESC_ARGS &esc, s, wmbc, cond
#define WinMsgDoEsc(name) winmsg_esc__name(name)(WINMSG_ESC_ARGS)
#define WinMsgDoEscEx(name, ...) winmsg_esc__name(name)(WINMSG_ESC_ARGS, __VA_ARGS__)

struct backtick {
	struct backtick *next;
	int num;
	int tick;
	int lifespan;
	time_t bestbefore;
	char result[MAXSTR];
	char **cmdv;
	Event ev;
	char *buf;
	int bufi;
} *backticks;


static char *pad_expand(char *buf, char *p, int numpad, int padlen)
{
	char *pn, *pn2;
	int i, r;

	padlen = padlen - (p - buf);	/* space for rent */
	if (padlen < 0)
		padlen = 0;
	pn2 = pn = p + padlen;
	r = winmsg->numrend;
	while (p >= buf) {
		if (r && *p != CHRPAD && p - buf == winmsg->rendpos[r - 1]) {
			winmsg->rendpos[--r] = pn - buf;
			continue;
		}
		*pn-- = *p;
		if (*p-- == CHRPAD) {
			pn[1] = ' ';
			i = numpad > 0 ? (padlen + numpad - 1) / numpad : 0;
			padlen -= i;
			while (i-- > 0)
				*pn-- = ' ';
			numpad--;
			if (r && p - buf == winmsg->rendpos[r - 1])
				winmsg->rendpos[--r] = pn - buf;
		}
	}
	return pn2;
}

static void backtick_filter(struct backtick *bt)
{
	char *p, *q;
	int c;

	for (p = q = bt->result; (c = (unsigned char)*p++) != 0;) {
		if (c == '\t')
			c = ' ';
		if (c >= ' ' || c == '\005')
			*q++ = c;
	}
	*q = 0;
}

static void backtick_fn(Event *ev, char *data)
{
	struct backtick *bt;
	int i, j, k, l;

	bt = (struct backtick *)data;
	i = bt->bufi;
	l = read(ev->fd, bt->buf + i, MAXSTR - i);
	if (l <= 0) {
		evdeq(ev);
		close(ev->fd);
		ev->fd = -1;
		return;
	}
	i += l;
	for (j = 0; j < l; j++)
		if (bt->buf[i - j - 1] == '\n')
			break;
	if (j < l) {
		for (k = i - j - 2; k >= 0; k--)
			if (bt->buf[k] == '\n')
				break;
		k++;
		memmove(bt->result, bt->buf + k, i - j - k);
		bt->result[i - j - k - 1] = 0;
		backtick_filter(bt);
		WindowChanged(0, '`');
	}
	if (j == l && i == MAXSTR) {
		j = MAXSTR / 2;
		l = j + 1;
	}
	if (j < l) {
		if (j)
			memmove(bt->buf, bt->buf + i - j, j);
		i = j;
	}
	bt->bufi = i;
}

void setbacktick(int num, int lifespan, int tick, char **cmdv)
{
	struct backtick **btp, *bt;
	char **v;

	for (btp = &backticks; (bt = *btp) != 0; btp = &bt->next)
		if (bt->num == num)
			break;
	if (!bt && !cmdv)
		return;
	if (bt) {
		for (v = bt->cmdv; *v; v++)
			free(*v);
		free(bt->cmdv);
		if (bt->buf)
			free(bt->buf);
		if (bt->ev.fd >= 0)
			close(bt->ev.fd);
		evdeq(&bt->ev);
	}
	if (bt && !cmdv) {
		*btp = bt->next;
		free(bt);
		return;
	}
	if (!bt) {
		bt = malloc(sizeof *bt);
		if (!bt) {
			Msg(0, "%s", strnomem);
			return;
		}
		memset(bt, 0, sizeof(*bt));
		bt->next = 0;
		*btp = bt;
	}
	bt->num = num;
	bt->tick = tick;
	bt->lifespan = lifespan;
	bt->bestbefore = 0;
	bt->result[0] = 0;
	bt->buf = 0;
	bt->bufi = 0;
	bt->cmdv = cmdv;
	bt->ev.fd = -1;
	if (bt->tick == 0 && bt->lifespan == 0) {
		bt->buf = malloc(MAXSTR);
		if (bt->buf == 0) {
			Msg(0, "%s", strnomem);
			setbacktick(num, 0, 0, (char **)0);
			return;
		}
		bt->ev.type = EV_READ;
		bt->ev.fd = readpipe(bt->cmdv);
		bt->ev.handler = backtick_fn;
		bt->ev.data = (char *)bt;
		if (bt->ev.fd >= 0)
			evenq(&bt->ev);
	}
}


static char *runbacktick(struct backtick *bt, int *tickp, time_t now)
{
	int f, i, l, j;
	time_t now2;

	if (bt->tick && (!*tickp || bt->tick < *tickp))
		*tickp = bt->tick;
	if ((bt->lifespan == 0 && bt->tick == 0) || now < bt->bestbefore) {
		return bt->result;
	}
	f = readpipe(bt->cmdv);
	if (f == -1)
		return bt->result;
	i = 0;
	while ((l = read(f, bt->result + i, sizeof(bt->result) - i)) > 0) {
		i += l;
		for (j = 1; j < l; j++)
			if (bt->result[i - j - 1] == '\n')
				break;
		if (j == l && i == sizeof(bt->result)) {
			j = sizeof(bt->result) / 2;
			l = j + 1;
		}
		if (j < l) {
			memmove(bt->result, bt->result + i - j, j);
			i = j;
		}
	}
	close(f);
	bt->result[sizeof(bt->result) - 1] = '\n';
	if (i && bt->result[i - 1] == '\n')
		i--;
	bt->result[i] = 0;
	backtick_filter(bt);
	(void)time(&now2);
	bt->bestbefore = now2 + bt->lifespan;
	return bt->result;
}

int AddWinMsgRend(const char *str, uint64_t r)
{
	if (winmsg->numrend >= MAX_WINMSG_REND || str < winmsg->buf || str >= winmsg->buf + MAXSTR)
		return -1;

	winmsg->rend[winmsg->numrend] = r;
	winmsg->rendpos[winmsg->numrend] = str - winmsg->buf;
	winmsg->numrend++;

	return 0;
}


winmsg_esc_ex(Wflags, Window *win, int plen)
{
	*wmbc->p = '\0';

	if (win)
		AddWindowFlags(wmbc->p, plen - 1, win);

	if (*winmsg->buf)
		wmc_set(cond);

	wmbc_fastfw(wmbc);
	return s;
}

winmsg_esc(Pid)
{
	sprintf(wmbc->p, "%d", (esc->flags.plus && display) ? D_userpid : getpid());
	wmbc_fastfw(wmbc);

	return s;
}

winmsg_esc_ex(CopyMode, Event *ev)
{
	wmbc->p--;
	if (display && ev && ev != &D_hstatusev) {	/* Hack */
		/* Is the layer in the current canvas in copy mode? */
		Canvas *cv = (Canvas *)ev->data;
		if (ev == &cv->c_captev && cv->c_layer->l_layfn == &MarkLf)
			wmc_set(cond);
	}

	return s;
}

winmsg_esc(EscSeen)
{
	wmbc->p--;
	if (display && D_ESCseen) {
		wmc_set(cond);
	}

	return s;
}

winmsg_esc_ex(Focus, Window *win, Event *ev)
{
	wmbc->p--;

	/* small hack (TODO: explain.) */
	if (display && ((ev && ev == &D_forecv->c_captev) || (!ev && win && win == D_fore)))
		esc->flags.minus ^= 1;

	if (esc->flags.minus)
		wmc_set(cond);

	return s;
}

winmsg_esc_ex(HostName, int plen)
{
	*wmbc->p = '\0';
	if ((int)strlen(HostName) < plen) {
		strncpy(wmbc->p, HostName, plen);
		if (*wmbc->p)
			wmc_set(cond);
	}
	wmbc_fastfw(wmbc);

	return s;
}

/**
 * Processes rendition
 *
 * The first character of s is assumed to be (unverified) the opening brace
 * of the sequence.
 */
winmsg_esc(Rend)
{
	char rbuf[RENDBUF_SIZE];
	uint8_t i;
	uint64_t r;

	s++;
	for (i = 0; i < (RENDBUF_SIZE-1); i++)
		if (s[i] && s[i] != WINESC_REND_END)
			rbuf[i] = s[i];
		else
			break;

	if ((s[i] == WINESC_REND_END) && (winmsg->numrend < MAX_WINMSG_REND)) {
		r = 0;
		rbuf[i] = '\0';
		if (i != 1 || rbuf[0] != WINESC_REND_POP)
			r = ParseAttrColor(rbuf, 0);
		if (r != 0 || (i == 1 && (rbuf[0] == WINESC_REND_POP))) {
			AddWinMsgRend(wmbc->p, r);
		}
	}
	s += i;
	wmbc->p--;

	return s;
}

winmsg_esc_ex(SessName, int plen)
{
	char *session_name = strchr(SocketName, '.') + 1;

	*wmbc->p = '\0';
	if ((int)strlen(session_name) < plen) {
		strncpy(wmbc->p, session_name, plen);
		if (*wmbc->p)
			wmc_set(cond);
	}

	wmbc_fastfw(wmbc);
	return s;
}

winmsg_esc_ex(WinNames, const bool hide_cur, Window *win, int plen)
{
	Window *oldfore = 0;

	if (display) {
		oldfore = D_fore;
		D_fore = win;
	}

	AddWindows(wmbc->p, plen - 1,
		hide_cur
			| (esc->flags.lng ? 0 : 2)
			| (esc->flags.plus ? 4 : 0)
			| (esc->flags.minus ? 8 : 0),
		win ? win->w_number : -1);

	if (display)
		D_fore = oldfore;

	if (*wmbc->p)
		wmc_set(cond);

	wmbc_fastfw(wmbc);
	return s;
}

winmsg_esc_ex(WinArgv, Window *win)
{
	if (!win || !win->w_cmdargs[0]) {
		wmbc->p--;
		return s;
	}

	sprintf(wmbc->p, "%s", win->w_cmdargs[0]);
	wmbc_fastfw0(wmbc);

	if (*s == WINESC_CMD_ARGS) {
		int i;
		for (i = 1; win->w_cmdargs[i]; i++) {
			sprintf(wmbc->p, " %s", win->w_cmdargs[i]);
			wmbc_fastfw0(wmbc);
		}
	}

	wmbc->p--;
	return s;
}

winmsg_esc_ex(WinTitle, Window *win, int plen)
{
	*wmbc->p = '\0';
	if (win && (int)strlen(win->w_title) < plen) {
		strncpy(wmbc->p, win->w_title, plen);
		if (*wmbc->p)
			wmc_set(cond);
	}

	wmbc_fastfw(wmbc);
	return s;
}

static inline char *_WinMsgCondProcess(char *posnew, char *pos, int condrend, int *destrend)
{
	if (posnew == pos)
		return pos;

	/* position has changed, so be sure to also restore renditions */
	*destrend = condrend;
	return posnew;
}

winmsg_esc_ex(Cond, int *condrend)
{
	wmbc->p--;

	if (wmc_is_active(cond)) {
		wmbc->p = _WinMsgCondProcess(wmc_end(cond, wmbc->p), wmbc->p,
			*condrend, &winmsg->numrend);
		wmc_deinit(cond);
		return s;
	}

	wmc_init(cond, wmbc->p);
	*condrend = winmsg->numrend;
	return s;
}

winmsg_esc_ex(CondElse, int *condrend)
{
	wmbc->p--;

	if (wmc_is_active(cond)) {
		wmbc->p = _WinMsgCondProcess(wmc_else(cond, wmbc->p), wmbc->p,
			*condrend, &winmsg->numrend);
	}

	return s;
}


char *MakeWinMsgEv(char *str, Window *win, int chesc, int padlen, Event *ev, int rec)
{
	static int tick;
	char *s = str;
	register int ctrl;
	struct timeval now;
	int l;
	int qmnumrend = 0;
	int numpad = 0;
	int lastpad = 0;
	int truncpos = -1;
	int truncper = 0;
	int trunclong = 0;
	uint64_t r;
	struct backtick *bt = NULL;
	WinMsgBufContext *wmbc;
	WinMsgEsc esc;
	WinMsgCond *cond = alloca(sizeof(WinMsgCond));

	/* XXX: This allocation exists temporarily during the buffer refactoring; it
	 * is not freed, as it exists as an alternative to the previous global
	 * static storage so that wmb_create may be eased in */
	if (!winmsg)
		winmsg = wmb_create();

	if (cond == NULL)
		Panic(0, "%s", strnomem);

	/* set to sane state (clear garbage) */
	wmc_deinit(cond);

	if (winmsg->numrend >= 0)
		winmsg->numrend = 0;
	else
		winmsg->numrend = -winmsg->numrend;

	wmb_reset(winmsg);
	wmbc = wmbc_create(winmsg);

	tick = 0;
	ctrl = 0;
	gettimeofday(&now, NULL);
	for (s = str; *s && (l = winmsg->buf + MAXSTR - 1 - wmbc->p) > 0; s++, wmbc->p++) {
		*wmbc->p = *s;

		if (ctrl) {
			ctrl = 0;
			if (*s != '^' && *s >= 64)
				*wmbc->p &= 0x1f;
			continue;
		}
		if (*s != chesc) {
			if (chesc == '%') {
				switch (*s) {
				case '^':
					ctrl = 1;
					*wmbc->p-- = '^';
					break;
				default:
					break;
				}
			}
			continue;
		}

		if (*++s == chesc)	/* double escape ? */
			continue;

		/* initialize escape */
		if ((esc.flags.plus = (*s == '+')) != 0)
			s++;
		if ((esc.flags.minus = (*s == '-')) != 0)
			s++;
		if ((esc.flags.zero = (*s == '0')) != 0)
			s++;
		esc.num = 0;
		while (*s >= '0' && *s <= '9')
			esc.num = esc.num * 10 + (*s++ - '0');
		if ((esc.flags.lng = (*s == 'L')) != 0)
			s++;

		switch (*s) {
		case WINESC_COND:
			s = WinMsgDoEscEx(Cond, &qmnumrend);
			break;
		case WINESC_COND_ELSE:
			s = WinMsgDoEscEx(CondElse, &qmnumrend);
			break;
		case '`':
		case 'h':
			if (rec >= 10 || (*s == 'h' && (win == 0 || win->w_hstatus == 0 || *win->w_hstatus == 0))) {
				wmbc->p--;
				break;
			}
			if (*s == '`') {
				for (bt = backticks; bt; bt = bt->next)
					if (bt->num == esc.num)
						break;
				if (bt == 0) {
					wmbc->p--;
					break;
				}
			}
			{
				char savebuf[sizeof(winmsg->buf)];
				int oldtick = tick;
				int oldnumrend = winmsg->numrend;

				*wmbc->p = '\0';
				strncpy(savebuf, winmsg->buf, sizeof(winmsg->buf));
				winmsg->numrend = -winmsg->numrend;
				MakeWinMsgEv(*s == 'h' ? win->w_hstatus : runbacktick(bt, &oldtick, now.tv_sec), win,
					     '\005', 0, (Event *)0, rec + 1);
				if (!tick || oldtick < tick)
					tick = oldtick;
				if ((int)strlen(winmsg->buf) < l)
					strncat(savebuf, winmsg->buf, sizeof(savebuf) - strlen(savebuf));
				strncpy(winmsg->buf, savebuf, sizeof(winmsg->buf));
				while (oldnumrend < winmsg->numrend)
					winmsg->rendpos[oldnumrend++] += wmbc->p - winmsg->buf;
				if (*wmbc->p)
					wmc_set(cond);
				wmbc_fastfw(wmbc);
			}
			break;
		case WINESC_CMD:
		case WINESC_CMD_ARGS:
			s = WinMsgDoEscEx(WinArgv, win);
			break;
		case WINESC_WIN_NAMES:
		case WINESC_WIN_NAMES_NOCUR:
			s = WinMsgDoEscEx(WinNames, (*s == WINESC_WIN_NAMES_NOCUR), win, l);
			break;
		case WINESC_WFLAGS:
			s = WinMsgDoEscEx(Wflags, win, l);
			break;
		case WINESC_WIN_TITLE:
			s = WinMsgDoEscEx(WinTitle, win, l);
			break;
		case WINESC_REND_START:
			s = WinMsgDoEsc(Rend);
			break;
		case WINESC_HOST:
			s = WinMsgDoEscEx(HostName, l);
			break;
		case WINESC_SESS_NAME:
			s = WinMsgDoEscEx(SessName, l);
			break;
		case WINESC_PID:
			s = WinMsgDoEsc(Pid);
			break;
		case WINESC_FOCUS:
			s = WinMsgDoEscEx(Focus, win, ev);
			break;
		case WINESC_COPY_MODE:
			s = WinMsgDoEscEx(CopyMode, ev);
			break;
		case WINESC_ESC_SEEN:
			s = WinMsgDoEsc(EscSeen);
			break;
		case '>':
			truncpos = wmbc->p - winmsg->buf;
			truncper = esc.num > 100 ? 100 : esc.num;
			trunclong = esc.flags.lng;
			wmbc->p--;
			break;
		case '=':
		case '<':
			*wmbc->p = ' ';
			if (esc.num || esc.flags.zero || esc.flags.plus || esc.flags.lng || (*s != '=')) {
				/* expand all pads */
				if (esc.flags.minus) {
					esc.num = (esc.flags.plus ? lastpad : padlen) - esc.num;
					if (!esc.flags.plus && padlen == 0)
						esc.num = wmbc->p - winmsg->buf;
					esc.flags.plus = 0;
				} else if (!esc.flags.zero) {
					if (*s != '=' && esc.num == 0 && !esc.flags.plus)
						esc.num = 100;
					if (esc.num > 100)
						esc.num = 100;
					if (padlen == 0)
						esc.num = wmbc->p - winmsg->buf;
					else
						esc.num = (padlen - (esc.flags.plus ? lastpad : 0)) * esc.num / 100;
				}
				if (esc.num < 0)
					esc.num = 0;
				if (esc.flags.plus)
					esc.num += lastpad;
				if (esc.num > MAXSTR - 1)
					esc.num = MAXSTR - 1;
				if (numpad)
					wmbc->p = pad_expand(winmsg->buf, wmbc->p, numpad, esc.num);
				numpad = 0;
				if (wmbc->p - winmsg->buf > esc.num && !esc.flags.lng) {
					int left, trunc;

					if (truncpos == -1) {
						truncpos = lastpad;
						truncper = 0;
					}
					trunc = lastpad + truncper * (esc.num - lastpad) / 100;
					if (trunc > esc.num)
						trunc = esc.num;
					if (trunc < lastpad)
						trunc = lastpad;
					left = truncpos - trunc;
					if (left > wmbc->p - winmsg->buf - esc.num)
						left = wmbc->p - winmsg->buf - esc.num;
					if (left > 0) {
						if (left + lastpad > wmbc->p - winmsg->buf)
							left = wmbc->p - winmsg->buf - lastpad;
						if (wmbc->p - winmsg->buf - lastpad - left > 0)
							memmove(winmsg->buf + lastpad, winmsg->buf + lastpad + left,
								wmbc->p - winmsg->buf - lastpad - left);
						wmbc->p -= left;
						r = winmsg->numrend;
						while (r && winmsg->rendpos[r - 1] > lastpad) {
							r--;
							winmsg->rendpos[r] -= left;
							if (winmsg->rendpos[r] < lastpad)
								winmsg->rendpos[r] = lastpad;
						}
						if (trunclong) {
							if (wmbc->p - winmsg->buf > lastpad)
								winmsg->buf[lastpad] = '.';
							if (wmbc->p - winmsg->buf > lastpad + 1)
								winmsg->buf[lastpad + 1] = '.';
							if (wmbc->p - winmsg->buf > lastpad + 2)
								winmsg->buf[lastpad + 2] = '.';
						}
					}
					if (wmbc->p - winmsg->buf > esc.num) {
						wmbc->p = winmsg->buf + esc.num;
						if (trunclong) {
							if (esc.num - 1 >= lastpad)
								wmbc->p[-1] = '.';
							if (esc.num - 2 >= lastpad)
								wmbc->p[-2] = '.';
							if (esc.num - 3 >= lastpad)
								wmbc->p[-3] = '.';
						}
						r = winmsg->numrend;
						while (r && winmsg->rendpos[r - 1] > esc.num)
							winmsg->rendpos[--r] = esc.num;
					}
					truncpos = -1;
					trunclong = 0;
					if (lastpad > wmbc->p - winmsg->buf)
						lastpad = wmbc->p - winmsg->buf;
				}
				if (*s == '=') {
					while (wmbc->p - winmsg->buf < esc.num)
						*wmbc->p++ = ' ';
					lastpad = wmbc->p - winmsg->buf;
					truncpos = -1;
					trunclong = 0;
				}
				wmbc->p--;
			} else if (padlen) {
				*wmbc->p = CHRPAD;	/* internal pad representation */
				numpad++;
			}
			break;
		case 's':
			*wmbc->p = '\0';
			if (!win)
				sprintf(wmbc->p, "--x--");
			else
				sprintf(wmbc->p, "%dx%d", win->w_width, win->w_height);
			wmbc_fastfw(wmbc);
			break;
		case 'n':
			s++;
			/* FALLTHROUGH */
		default:
			s--;
			if (l > 10 + esc.num) {
				if (esc.num == 0)
					esc.num = 1;
				if (!win)
					sprintf(wmbc->p, "%*s", esc.num, esc.num > 1 ? "--" : "-");
				else
					sprintf(wmbc->p, "%*d", esc.num, win->w_number);
				wmc_set(cond);
				wmbc_fastfw(wmbc);
			}
			break;
		}
	}
	if (wmc_is_active(cond) && !wmc_is_set(cond))
		wmbc->p = wmc_end(cond, wmbc->p) + 1;
	*wmbc->p = '\0';
	if (numpad) {
		if (padlen > MAXSTR - 1)
			padlen = MAXSTR - 1;
		pad_expand(winmsg->buf, wmbc->p, numpad, padlen);
	}
	if (ev) {
		evdeq(ev);	/* just in case */
		ev->timeout.tv_sec = 0;
		ev->timeout.tv_usec = 0;
	}
	if (ev && tick) {
		now.tv_usec = 100000;
		if (tick == 1)
			now.tv_sec++;
		else
			now.tv_sec += tick - (now.tv_sec % tick);
		ev->timeout = now;
	}

	wmbc_free(wmbc);
	return winmsg->buf;
}

char *MakeWinMsg(char *s, Window *win, int esc)
{
	return MakeWinMsgEv(s, win, esc, 0, (Event *)0, 0);
}