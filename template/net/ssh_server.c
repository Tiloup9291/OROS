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
 * ssh_server.c — SSH server (wolfSSH + wolfCrypt) over lwIP raw TCP.
 *
 * Architecture (NO_SYS=1, Core2 IO_SOFT, one session at a time):
 *
 *   [lwIP raw TCP]  --tcp_recv-->  [rx ring]  --IORecv cb-->  [wolfSSH]
 *   [lwIP raw TCP]  <--tcp_write--            <--IOSend cb--  [wolfSSH]
 *
 * wolfSSH is driven in NON-BLOCKING mode: its I/O callbacks return
 * WS_CBIO_ERR_WANT_READ when there is no data (yet); the state machine
 * (ssh_server_poll) calls back wolfSSH_accept()/wolfSSH_stream_read() until
 * progress is made. No blocking loop -> consistent with Core2's lwIP
 * main-loop (ping + telnet + ecat keep running).
 *
 * The remote shell reuses net_shell_exec() (identical to telnet).
 *
 * API ref.: wolfssh/ssh.h (wolfSSH_Init, wolfSSH_CTX_new, SetIORecv/Send,
 * SetUserAuth, CTX_UsePrivateKey_buffer, wolfSSH_new, set_fd/SetIO*Ctx,
 * wolfSSH_accept, wolfSSH_stream_read/send, wolfSSH_get_error).
 */
#define WOLFSSL_USER_SETTINGS
#define WOLFSSH_USER_SETTINGS

#include <string.h>
#include <stdio.h>

#include "ssh_server.h"
#include "net_shell.h"
#include "ssh_hostkey.h"

#include <wolfssh/ssh.h>
#include <wolfssh/error.h>
#include <wolfssl/wolfcrypt/settings.h>
#ifdef WOLFSSH_SFTP
#include <wolfssh/wolfsftp.h>
#include "sftp_jail.h"
#endif

#include "lwip/tcp.h"
#include "lwip/pbuf.h"

/* ------------------------------------------------------------------ */
/* Session buffers (only one active session).                         */
/* ------------------------------------------------------------------ */
#define SSH_RX_RING     8192u    /* received encrypted bytes, awaiting wolfSSH */
#define SSH_TX_RING     8192u    /* PLAINTEXT shell bytes awaiting send        */
#define SSH_LINE_MAX     256u    /* shell command line                         */
#define SSH_PLAIN_MAX   1024u    /* decrypted data read from the channel       */


typedef enum {
    SSH_ST_IDLE = 0,     /* no session                          */
    SSH_ST_ACCEPT,       /* SSH handshake in progress            */
    SSH_ST_RUN,          /* session established, shell exchange  */
    SSH_ST_SFTP,         /* session established, SFTP subsystem  */
    SSH_ST_CLOSING       /* close requested                      */
} ssh_state_t;

typedef struct {
    struct tcp_pcb *pcb;         /* TCP PCB of the session                */

    /* Receive ring (encrypted data coming from tcp_recv). */
    uint8_t  rx[SSH_RX_RING];
    volatile uint32_t rx_head;   /* write (tcp_recv)                       */
    volatile uint32_t rx_tail;   /* read (wolfSSH IORecv)                  */

    /* Transmit ring (PLAINTEXT shell text, awaiting encryption+send).
     * ssh_shell_out() WRITES into it without ever blocking; ssh_flush_tx()
     * progressively drains it via wolfSSH_stream_send across poll rounds
     * (when the TCP sndbuf has room). This avoids the busy-loop that FROZE
     * the shell. */
    uint8_t  tx[SSH_TX_RING];
    uint32_t tx_head;            /* write (ssh_shell_out)                 */
    uint32_t tx_tail;            /* read (ssh_flush_tx)                   */

    ssh_state_t state;


    /* Accumulation of the (decrypted) command line. */
    char   line[SSH_LINE_MAX];
    uint32_t line_len;

    int    want_close;           /* the shell requested quit/exit         */
    int    banner_sent;
} ssh_session_t;

static ssh_session_t s_sess;

/* Global wolfSSH context (created once). */
static WOLFSSH_CTX *s_ctx = NULL;
static WOLFSSH     *s_ssh = NULL;   /* current session's object              */

static struct tcp_pcb *s_listen = NULL;

/* Counters for the summary. */
static uint32_t s_sessions = 0;
static uint32_t s_auth_ok  = 0;
static uint32_t s_commands = 0;
#ifdef WOLFSSH_SFTP
static uint32_t s_sftp_sessions = 0;   /* sessions that opened the subsystem */
#endif

/* ------------------------------------------------------------------ */
/* RX ring: helpers                                                   */
/* ------------------------------------------------------------------ */
static uint32_t rx_used(void)
{
    return s_sess.rx_head - s_sess.rx_tail;
}

static uint32_t rx_free(void)
{
    return SSH_RX_RING - rx_used();
}

static void rx_push(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (rx_free() == 0)
            break;                          /* overflow: drop (rare) */
        s_sess.rx[s_sess.rx_head % SSH_RX_RING] = data[i];
        s_sess.rx_head++;
    }
}

static uint32_t rx_pop(uint8_t *dst, uint32_t max)
{
    uint32_t n = rx_used();
    if (n > max)
        n = max;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = s_sess.rx[s_sess.rx_tail % SSH_RX_RING];
        s_sess.rx_tail++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* wolfSSH <-> lwIP raw TCP I/O callbacks                             */
/* ------------------------------------------------------------------ */

/* wolfSSH wants to READ `sz` encrypted bytes. We take them from the RX
 * ring. Empty ring -> WS_CBIO_ERR_WANT_READ (non-blocking). */
static int ssh_io_recv(WOLFSSH *ssh, void *buf, word32 sz, void *ctx)
{
    (void)ssh; (void)ctx;
    if (s_sess.pcb == NULL)
        return WS_CBIO_ERR_GENERAL;

    uint32_t n = rx_pop((uint8_t *)buf, sz);
    if (n == 0)
        return WS_CBIO_ERR_WANT_READ;
    return (int)n;
}

/* wolfSSH wants to WRITE `sz` encrypted bytes -> tcp_write + tcp_output.
 *
 * IMPORTANT: an encrypted SSH packet must be transmitted WHOLE and in
 * order. Returning a PARTIAL send (n < sz) is allowed by wolfSSH's I/O API
 * (it will call back with the rest), BUT we must NEVER write into the TCP
 * sndbuf a fragment we couldn't complete, nor return WANT_WRITE after
 * having already consumed bytes. We therefore bound the send to what
 * tcp_write actually accepts (sndbuf), flush (tcp_output), and return
 * EXACTLY the number of accepted bytes (or WANT_WRITE if 0). */
static int ssh_io_send(WOLFSSH *ssh, void *buf, word32 sz, void *ctx)
{
    (void)ssh; (void)ctx;
    struct tcp_pcb *pcb = s_sess.pcb;
    if (pcb == NULL)
        return WS_CBIO_ERR_GENERAL;

    if (sz == 0)
        return 0;

    uint32_t avail = tcp_sndbuf(pcb);
    if (avail == 0) {
        /* Nothing can be buffered right now: force a flush of what is
         * already queued and ask wolfSSH to come back later. */
        tcp_output(pcb);
        return WS_CBIO_ERR_WANT_WRITE;
    }

    uint32_t n = sz;
    if (n > avail)
        n = avail;

    /* tcp_write may refuse (ERR_MEM) if pbufs/segments are exhausted even
     * when sndbuf seems sufficient: we then progressively shrink. */
    err_t e;
    while (n > 0) {
        e = tcp_write(pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (e == ERR_OK)
            break;
        if (e != ERR_MEM)
            return WS_CBIO_ERR_GENERAL;
        n /= 2;                         /* retry with a smaller block */
    }
    if (n == 0) {
        tcp_output(pcb);
        return WS_CBIO_ERR_WANT_WRITE;  /* nothing accepted this round -> retry */
    }

    tcp_output(pcb);
    return (int)n;                      /* bytes actually accepted */
}


/* ------------------------------------------------------------------ */
/* Password authentication                                            */
/* ------------------------------------------------------------------ */
static int ssh_user_auth(byte authType, WS_UserAuthData *authData, void *ctx)
{
    (void)ctx;

    if (authType == WOLFSSH_USERAUTH_PASSWORD) {
        const char *user = SSH_USER;
        const char *pass = SSH_PASS;

        int user_ok = (authData->usernameSz == strlen(user)) &&
                      (memcmp(authData->username, user,
                              authData->usernameSz) == 0);
        int pass_ok = (authData->sf.password.passwordSz == strlen(pass)) &&
                      (memcmp(authData->sf.password.password, pass,
                              authData->sf.password.passwordSz) == 0);

        if (user_ok && pass_ok) {
            s_auth_ok++;
            return WOLFSSH_USERAUTH_SUCCESS;
        }
        return WOLFSSH_USERAUTH_INVALID_PASSWORD;
    }

    /* Any other auth type is refused (we force password). */
    return WOLFSSH_USERAUTH_FAILURE;
}

/* ------------------------------------------------------------------ */
/* TX ring (PLAINTEXT shell text): helpers                            */
/* ------------------------------------------------------------------ */
static uint32_t tx_used(void) { return s_sess.tx_head - s_sess.tx_tail; }

/* ------------------------------------------------------------------ */
/* Shell: output callback — BUFFERS ONLY (NEVER blocks).              */
/* ------------------------------------------------------------------ */
/* ssh_shell_out simply PUSHES the text into a TX ring (no
 * encryption, no send here). It is ssh_flush_tx() (called in ssh_advance,
 * outside the blocking path) that progressively drains the ring across
 * polls. */
static void ssh_shell_out(void *c, const char *data, size_t len)
{
    (void)c;
    if (s_ssh == NULL || len == 0)
        return;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        if (tx_used() >= SSH_TX_RING)
            break;                          /* ring full: truncate (rare) */
        s_sess.tx[s_sess.tx_head % SSH_TX_RING] = p[i];
        s_sess.tx_head++;
    }
}

/* Drains the TX ring to wolfSSH_stream_send WITHOUT blocking: we send as
 * long as wolfSSH accepts; as soon as it returns WANT_WRITE (sndbuf full)
 * we stop and resume on the next poll (once the ACK has freed room). */
static void ssh_flush_tx(void)
{
    if (s_ssh == NULL)
        return;
    int guard = 0;
    while (tx_used() > 0 && guard++ < 64) {
        uint32_t idx = s_sess.tx_tail % SSH_TX_RING;
        uint32_t contig = SSH_TX_RING - idx;      /* until the end of the ring */
        uint32_t avail  = tx_used();
        uint32_t chunk  = (avail < contig) ? avail : contig;
        if (chunk > 1024) chunk = 1024;           /* bound per call */

        int sent = wolfSSH_stream_send(s_ssh, &s_sess.tx[idx], chunk);
        if (sent == WS_WANT_WRITE || sent == WS_WANT_READ)
            break;                                /* sndbuf full: later */
        if (sent <= 0) {
            /* Real error: give up (log once). */
            static int s_out_err_logged = 0;
            if (!s_out_err_logged) {
                s_out_err_logged = 1;
                printf("[ssh] stream_send err=%d (%s)\n", sent,
                       wolfSSH_ErrorToName(wolfSSH_get_error(s_ssh)));
            }
            break;
        }
        s_sess.tx_tail += (uint32_t)sent;
    }
    if (s_sess.pcb)
        tcp_output(s_sess.pcb);
}



/* ------------------------------------------------------------------ */
/* Processing decrypted bytes (line accumulation + execution)         */
/* ------------------------------------------------------------------ */
static void ssh_feed_plain(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        char ch = (char)data[i];

        if (ch == '\r' || ch == '\n') {
            /* CRLF echo then line execution. */
            ssh_shell_out(NULL, "\r\n", 2);
            s_sess.line[s_sess.line_len] = '\0';

            if (s_sess.line_len > 0) {
                s_commands++;
                int close = net_shell_exec(s_sess.line, ssh_shell_out, NULL);
                if (close)
                    s_sess.want_close = 1;
            }
            s_sess.line_len = 0;
            if (!s_sess.want_close)
                net_shell_prompt(ssh_shell_out, NULL);
        }
        else if (ch == 0x7F || ch == 0x08) {   /* backspace / delete         */
            if (s_sess.line_len > 0) {
                s_sess.line_len--;
                ssh_shell_out(NULL, "\b \b", 3);
            }
        }
        else if (ch >= 0x20 && ch < 0x7F) {     /* printable character        */
            if (s_sess.line_len < SSH_LINE_MAX - 1) {
                s_sess.line[s_sess.line_len++] = ch;
                ssh_shell_out(NULL, &ch, 1);    /* local echo                 */
            }
        }
        /* other control bytes ignored */
    }
}

/* ------------------------------------------------------------------ */
/* Clean session shutdown                                             */
/* ------------------------------------------------------------------ */
static void ssh_close_session(void)
{
    if (s_ssh) {
        wolfSSH_free(s_ssh);
        s_ssh = NULL;
    }
    if (s_sess.pcb) {
        tcp_arg(s_sess.pcb, NULL);
        tcp_recv(s_sess.pcb, NULL);
        tcp_err(s_sess.pcb, NULL);
        tcp_close(s_sess.pcb);
    }
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.state = SSH_ST_IDLE;
}

/* ------------------------------------------------------------------ */
/* SSH state machine — called from ssh_server_poll()                  */
/* ------------------------------------------------------------------ */
static void ssh_advance(void)
{
    if (s_sess.state == SSH_ST_ACCEPT) {
        int r = wolfSSH_accept(s_ssh);
        if (r == WS_SUCCESS) {
            s_sess.state = SSH_ST_RUN;
            /* Banner + prompt (after the shell channel is opened). */
            net_shell_welcome(ssh_shell_out, NULL);
            s_sess.banner_sent = 1;
        }
#ifdef WOLFSSH_SFTP
        /* The peer asked for the "sftp" subsystem instead of a shell.
         * wolfSSH signals this by returning WS_SFTP_COMPLETE from the SAME
         * wolfSSH_accept() call: same TCP port 22, same handshake, same
         * password auth. From now on the session is driven by
         * wolfSSH_SFTP_read() rather than by the line-based shell. */
        else if (r == WS_SFTP_COMPLETE) {
            s_sess.state = SSH_ST_SFTP;
            s_sftp_sessions++;
            printf("[ssh] sftp subsystem opened (rooted at %s)\n",
                   OROS_SFTP_JAIL_ROOT);
        }
#endif
        else {
            int e = wolfSSH_get_error(s_ssh);
            if (e == WS_WANT_READ || e == WS_WANT_WRITE) {
                /* Handshake not finished: we'll come back on the next poll. */
                return;
            }
#ifdef WOLFSSH_SFTP
            /* Some paths report the subsystem completion through the error
             * slot rather than the return value; treat it the same way. */
            if (e == WS_SFTP_COMPLETE) {
                s_sess.state = SSH_ST_SFTP;
                s_sftp_sessions++;
                printf("[ssh] sftp subsystem opened (rooted at %s)\n",
                       OROS_SFTP_JAIL_ROOT);
                return;
            }
#endif
            /* Fatal handshake error. */
            printf("[ssh] handshake failed (err=%d %s)\n", e,
                   wolfSSH_ErrorToName(e));
            ssh_close_session();
        }
        return;
    }

    if (s_sess.state == SSH_ST_RUN) {
        /* Drain ALL decrypted data available this round. wolfSSH may have
         * several SSH packets pending in the RX ring; a single stream_read
         * pass per loop round would make the session very slow (1 packet
         * every ~200 us). We loop until WANT_READ (empty ring), bounding
         * the iterations (anti Core2 monopolization). */
        uint8_t plain[SSH_PLAIN_MAX];
        int guard = 0;
        while (guard++ < 64) {
            int n = wolfSSH_stream_read(s_ssh, plain, sizeof(plain));
            if (n > 0) {
                ssh_feed_plain(plain, (uint32_t)n);
                continue;               /* maybe more data */
            }
            int e = wolfSSH_get_error(s_ssh);
            if (e == WS_WANT_READ || e == WS_WANT_WRITE) {
                break;                  /* nothing more decryptable this round */
            }
            else if (n == WS_EOF || e == WS_EOF) {
                s_sess.want_close = 1;
                break;
            }
            else if (n < 0) {
                /* error: close */
                s_sess.want_close = 1;
                break;
            }
            else {
                break;                  /* n == 0 without error: nothing more */
            }
        }

        /* Drain the shell output ring (non-blocking) — this is WHAT
         * actually sends the commands' response, on every poll, without
         * freezing. */
        ssh_flush_tx();

        /* Only close once all pending output has been sent (the
         * "bye." of quit must go out before cutting the session). */
        if (s_sess.want_close && tx_used() == 0) {
            wolfSSH_stream_exit(s_ssh, 0);
            ssh_close_session();
        }
        return;
    }

#ifdef WOLFSSH_SFTP
    if (s_sess.state == SSH_ST_SFTP) {
        /* SFTP request pump. Same principle as the shell branch: process as
         * many requests as are already decodable this round, then hand Core2
         * back its main-loop. wolfSSH_SFTP_read() handles ONE request per
         * call and parks a reply in the session when the socket is full, so
         * we must also keep turning while PendingSend() is set. */
        int guard = 0;
        while (guard++ < 16) {
            int r = wolfSSH_SFTP_read(s_ssh);
            int e = wolfSSH_get_error(s_ssh);

            /* Nothing more to decode / socket full: resume on the next poll,
             * unless a reply is still queued (then keep draining it). */
            if (e == WS_WANT_READ || e == WS_WANT_WRITE ||
                    e == WS_WINDOW_FULL) {
                if (!wolfSSH_SFTP_PendingSend(s_ssh))
                    break;
                continue;
            }

            /* Rekey in progress: not an error, just keep the crank turning. */
            if (r == WS_REKEYING)
                continue;

            if (r == WS_EOF || e == WS_EOF ||
                    r == WS_CHANNEL_CLOSED || e == WS_CHANNEL_CLOSED) {
                s_sess.want_close = 1;
                break;
            }

            if (r < 0 && r != WS_CHAN_RXD) {
                printf("[ssh] sftp error (err=%d %s)\n", e,
                       wolfSSH_ErrorToName(e));
                s_sess.want_close = 1;
                break;
            }

            /* A reply may still be pending after a successful request. */
            if (wolfSSH_SFTP_PendingSend(s_ssh))
                continue;
        }

        if (s_sess.want_close)
            ssh_close_session();
        return;
    }
#endif /* WOLFSSH_SFTP */

}


/* ------------------------------------------------------------------ */
/* lwIP raw TCP callbacks                                             */
/* ------------------------------------------------------------------ */
static void ssh_tcp_err(void *arg, err_t err)
{
    (void)arg; (void)err;
    /* The PCB is already freed by lwIP: don't close it again. */
    if (s_ssh) {
        wolfSSH_free(s_ssh);
        s_ssh = NULL;
    }
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.state = SSH_ST_IDLE;
}

static err_t ssh_tcp_recv(void *arg, struct tcp_pcb *pcb,
                          struct pbuf *p, err_t err)
{
    (void)arg;

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        return err;
    }
    if (p == NULL) {
        /* The client closed the connection. */
        s_sess.want_close = 1;
        return ERR_OK;
    }

    /* Copy the encrypted bytes into the RX ring for wolfSSH. */
    struct pbuf *q = p;
    while (q) {
        rx_push((const uint8_t *)q->payload, q->len);
        q = q->next;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t ssh_tcp_poll(void *arg, struct tcp_pcb *pcb)
{
    (void)arg; (void)pcb;
    /* Advances the session even without new data (flush WANT_WRITE). */
    ssh_advance();
    return ERR_OK;
}

static err_t ssh_tcp_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || newpcb == NULL)
        return ERR_VAL;

    /* Only one session: decline additional connections. */
    if (s_sess.state != SSH_ST_IDLE) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    /* New session: reset the state + create the wolfSSH object. */
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.pcb = newpcb;
    s_sess.state = SSH_ST_ACCEPT;

    s_ssh = wolfSSH_new(s_ctx);
    if (s_ssh == NULL) {
        printf("[ssh] wolfSSH_new failed\n");
        tcp_abort(newpcb);
        memset(&s_sess, 0, sizeof(s_sess));
        return ERR_ABRT;
    }

    /* The I/O callbacks don't need a ctx (single global session). */
    wolfSSH_SetIOReadCtx(s_ssh, &s_sess);
    wolfSSH_SetIOWriteCtx(s_ssh, &s_sess);

#ifdef WOLFSSH_SFTP
    /* Confine SFTP to /srv BEFORE the handshake starts.
     *
     * This is the first of the two barriers: wolfSSH stores the default path
     * canonically and then refuses (WS_PERMISSIONS) any request resolving
     * outside that subtree. Setting it here also avoids the lazy
     * "no default path yet" code path, which would call WGETCWD -> f_getcwd(),
     * a function our FatFs build does not even compile in (FF_FS_RPATH=0).
     * The second barrier is the fs/sftp_jail.c translation layer.
     * If the jail root is missing (no SD card, no /srv), SFTP is simply not
     * armed: the peer then gets a subsystem failure and can still use the
     * interactive shell. */
    if (oros_sftp_jail_ready()) {
        if (wolfSSH_SFTP_SetDefaultPath(s_ssh, OROS_SFTP_JAIL_ROOT)
                != WS_SUCCESS) {
            printf("[ssh] WARNING: sftp default path (%s) refused\n",
                   OROS_SFTP_JAIL_ROOT);
        }
    }
#endif

    tcp_arg(newpcb, &s_sess);
    tcp_recv(newpcb, ssh_tcp_recv);
    tcp_err(newpcb, ssh_tcp_err);
    tcp_poll(newpcb, ssh_tcp_poll, 1);   /* poll ~500 ms (1 * 500ms)         */
    tcp_nagle_disable(newpcb);

    s_sessions++;
    printf("[ssh] new connection (session #%lu) — handshake...\n",
           (unsigned long)s_sessions);
    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */
int ssh_server_start(void)
{
    if (wolfSSH_Init() != WS_SUCCESS) {
        printf("[ssh] wolfSSH_Init failed\n");
        return -1;
    }

    s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (s_ctx == NULL) {
        printf("[ssh] wolfSSH_CTX_new failed\n");
        return -2;
    }

    /* I/O callbacks (lwIP raw transport) + password auth. */
    wolfSSH_SetIORecv(s_ctx, ssh_io_recv);
    wolfSSH_SetIOSend(s_ctx, ssh_io_send);
    wolfSSH_SetUserAuth(s_ctx, ssh_user_auth);

    /* ed25519 host key (embedded DER). */
    if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx,
            ssh_hostkey_ed25519_der, ssh_hostkey_ed25519_der_len,
            WOLFSSH_FORMAT_ASN1) < 0) {
        printf("[ssh] loading ed25519 host key failed\n");
        return -3;
    }

    /* TCP server: bind + listen on the SSH port. */
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL)
        return -4;
    if (tcp_bind(pcb, IP_ANY_TYPE, SSH_SERVER_PORT) != ERR_OK) {
        tcp_close(pcb);
        return -5;
    }
    s_listen = tcp_listen(pcb);
    if (s_listen == NULL) {
        tcp_close(pcb);
        return -6;
    }
    tcp_accept(s_listen, ssh_tcp_accept);

    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.state = SSH_ST_IDLE;
    return 0;
}

void ssh_server_poll(void)
{
    if (ssh_server_session_active())
        ssh_advance();
}

int ssh_server_session_active(void)
{
    return (s_sess.state == SSH_ST_ACCEPT ||
            s_sess.state == SSH_ST_RUN ||
            s_sess.state == SSH_ST_SFTP);
}

uint32_t ssh_server_sessions(void) { return s_sessions; }
uint32_t ssh_server_auth_ok(void)  { return s_auth_ok;  }
uint32_t ssh_server_commands(void) { return s_commands; }

uint32_t ssh_server_sftp_sessions(void)
{
#ifdef WOLFSSH_SFTP
    return s_sftp_sessions;
#else
    return 0;
#endif
}
