/*
 * OROS - OROS' Real-Time Operating System
 * Copyright (C) 2026  Tiloup9291 <-> John Doe
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * tcp_shell.c — "telnet-like" TCP server for the remote shell.
 *
 * lwIP RAW API (NO_SYS=1). Only one active session at a time. The service is
 * entirely callback-driven, invoked from Core2's lwIP polling loop
 * (netif_r8152_poll + sys_check_timeouts).
 *
 * How a session works:
 *   - accept  : we remember the pcb, send the banner + prompt.
 *   - recv    : we accumulate bytes into a line buffer; on each '\n'
 *               (or '\r'), we execute the command via net_shell_exec(), then
 *               redisplay the prompt. The shell output is sent via
 *               tcp_write() (buffered, flushed by tcp_output()).
 *   - err/close : we free the session.
 *
 * Minimal telnet handling: we IGNORE the IAC negotiation sequences (0xFF
 * followed by 2 bytes) that `telnet` sends at connection, and we filter out
 * NUL and an isolated '\r' (client-side canonical mode). `nc`/`ncat` send
 * raw text line by line: no IAC, it also works.
 */
#include "tcp_shell.h"
#include "net_shell.h"

#include <string.h>

#include "lwip/tcp.h"
#include "lwip/err.h"

/* ------------------------------------------------------------------ */
/* State of a session (only one at a time)                            */
/* ------------------------------------------------------------------ */
#define SHELL_LINE_MAX   256

typedef struct {
    struct tcp_pcb *pcb;            /* connection pcb (NULL = free) */
    char    line[SHELL_LINE_MAX];   /* line accumulation buffer */
    uint16_t len;             /* current length in line[] */
    uint8_t  in_iac;          /* telnet IAC parsing state (0..2 bytes remaining) */
    uint8_t  closing;         /* session being closed */
} shell_session_t;

static shell_session_t s_sess;               /* single session */
static struct tcp_pcb *s_listen_pcb;         /* listening pcb */
static uint32_t s_sessions;                  /* accepted sessions counter */
static uint32_t s_commands;                  /* executed commands counter */

/* ------------------------------------------------------------------ */
/* Shell output -> TCP (shell_out_fn callback)                        */
/* ------------------------------------------------------------------ */
/* Writes `len` bytes to the session (ctx = shell_session_t*). Uses
 * tcp_write copying the data (TCP_WRITE_FLAG_COPY) since the shell's buffer
 * is temporary. If there is not enough room (ERR_MEM), we silently truncate
 * (the shell stays usable, output is best-effort). */
static void session_out(void *ctx, const char *data, size_t len)
{
    shell_session_t *s = (shell_session_t *)ctx;
    if (!s || !s->pcb || s->closing || len == 0)
        return;

    size_t off = 0;
    while (off < len) {
        u16_t avail = tcp_sndbuf(s->pcb);
        if (avail == 0) {
            /* No room: flush what is already queued and drop the rest
             * (best-effort, no blocking under NO_SYS). */
            tcp_output(s->pcb);
            break;
        }
        u16_t chunk = (len - off < avail) ? (u16_t)(len - off) : avail;
        err_t e = tcp_write(s->pcb, data + off, chunk, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK)
            break;
        off += chunk;
    }
    tcp_output(s->pcb);
}

/* ------------------------------------------------------------------ */
/* Clean shutdown                                                     */
/* ------------------------------------------------------------------ */
static void session_reset(shell_session_t *s)
{
    s->pcb     = NULL;
    s->len     = 0;
    s->in_iac  = 0;
    s->closing = 0;
    s->line[0] = '\0';
}

static void session_close(shell_session_t *s)
{
    if (!s->pcb)
        return;
    struct tcp_pcb *pcb = s->pcb;
    /* Detach all callbacks before closing. */
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (tcp_close(pcb) != ERR_OK)
        tcp_abort(pcb);
    session_reset(s);
}

/* ------------------------------------------------------------------ */
/* Processing a complete line                                         */
/* ------------------------------------------------------------------ */
static void handle_line(shell_session_t *s)
{
    s->line[s->len] = '\0';
    s_commands++;

    int should_quit = net_shell_exec(s->line, session_out, s);
    s->len = 0;

    if (should_quit) {
        s->closing = 1;
        session_close(s);
    } else {
        net_shell_prompt(session_out, s);
    }
}

/* Consumes a received byte: handles telnet IAC + line accumulation. */
static void feed_byte(shell_session_t *s, uint8_t b)
{
    /* Telnet negotiation: IAC (0xFF) + 2 bytes (command + option). */
    if (s->in_iac) {
        s->in_iac--;
        return;
    }
    if (b == 0xFF) {          /* IAC */
        s->in_iac = 2;
        return;
    }

    if (b == '\n' || b == '\r') {
        /* End of line. We execute even on an empty line (redisplays the
         * prompt). A '\r\n' produces two ends: the 2nd (empty line) is
         * harmless. We requested WILL ECHO (character mode) -> it is WE who
         * echo the CRLF so the client moves to a new line. */
        if (b == '\r' || s->len > 0)   /* avoids a duplicate CRLF on the '\n' of \r\n */
            session_out(s, "\r\n", 2);
        handle_line(s);
        return;
    }
    if (b == 0x00 || b == 0x7F) {
        /* NULL (often after \r in telnet). DEL (0x7F) = backspace on some
         * terminals: ignored here (the real backspace 0x08 is handled). */
        return;
    }
    if (b == 0x08) {          /* backspace: erase the last character + echo */
        if (s->len > 0) {
            s->len--;
            session_out(s, "\b \b", 3);   /* visually erases (WILL ECHO) */
        }
        return;
    }
    if (s->len < SHELL_LINE_MAX - 1) {
        s->line[s->len++] = (char)b;
        /* WILL ECHO: the server redisplays the typed character (character
         * mode, no more local echo client-side -> no double keystroke). Only
         * echo printable ASCII characters. */
        if (b >= 0x20 && b < 0x7F)
            session_out(s, (const char *)&b, 1);
    }
    /* if the line overflows, we ignore the surplus until the next '\n' */
}

/* ------------------------------------------------------------------ */
/* lwIP callbacks                                                     */
/* ------------------------------------------------------------------ */
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    shell_session_t *s = (shell_session_t *)arg;

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        return err;
    }
    if (p == NULL) {
        /* The client closed the connection. */
        if (s) session_close(s);
        return ERR_OK;
    }

    /* Walk through all the pbufs in the chain. */
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *d = (const uint8_t *)q->payload;
        for (u16_t i = 0; i < q->len; i++) {
            if (s && s->pcb && !s->closing)
                feed_byte(s, d[i]);
        }
    }

    /* Acknowledge the received data to lwIP. */
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)pcb; (void)len;
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)err;
    shell_session_t *s = (shell_session_t *)arg;
    /* The pcb is already freed by lwIP: don't touch it, just reset. */
    if (s) session_reset(s);
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
    shell_session_t *s = (shell_session_t *)arg;
    if (s && s->closing) {
        session_close(s);
        return ERR_OK;
    }
    /* Nothing to do periodically; possible flush. */
    tcp_output(pcb);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || newpcb == NULL)
        return ERR_VAL;

    /* Only one session: refuse if busy. */
    if (s_sess.pcb != NULL) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    session_reset(&s_sess);
    s_sess.pcb = newpcb;
    s_sessions++;

    tcp_arg(newpcb, &s_sess);
    tcp_recv(newpcb, on_recv);
    tcp_sent(newpcb, on_sent);
    tcp_err(newpcb, on_err);
    tcp_poll(newpcb, on_poll, 4 /* ~2 s (unit = 500 ms) */);
    tcp_nagle_disable(newpcb);   /* immediate interactive response */

    /* Telnet negotiation: the SERVER takes charge of echo (IAC WILL ECHO) +
     * "suppress go-ahead" mode (IAC WILL SGA). This switches a `telnet`
     * client from its line mode (local echo -> double prompt
     * `oros> oros>`) to character mode: no more double echo, clean
     * behavior. `nc`, which does not interpret these IAC, silently ignores
     * them (our parser also filters them on the RX side). RFC 854/857/858
     * codes: IAC=255, WILL=251, ECHO=1, SGA=3. */
    {
        static const char telnet_init[] = {
            (char)255, (char)251, (char)1,   /* IAC WILL ECHO */
            (char)255, (char)251, (char)3,   /* IAC WILL SGA  */
        };
        session_out(&s_sess, telnet_init, sizeof(telnet_init));
    }

    /* Banner + prompt. */
    net_shell_welcome(session_out, &s_sess);
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
int tcp_shell_start(void)
{
    session_reset(&s_sess);
    s_sessions = 0;
    s_commands = 0;

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb)
        return -1;

    if (tcp_bind(pcb, IP_ANY_TYPE, TCP_SHELL_PORT) != ERR_OK) {
        tcp_close(pcb);
        return -2;
    }

    struct tcp_pcb *lp = tcp_listen(pcb);
    if (!lp) {
        tcp_close(pcb);
        return -3;
    }
    s_listen_pcb = lp;
    tcp_accept(lp, on_accept);
    return 0;
}

uint32_t tcp_shell_sessions(void)
{
    return s_sessions;
}

uint32_t tcp_shell_commands(void)
{
    return s_commands;
}
