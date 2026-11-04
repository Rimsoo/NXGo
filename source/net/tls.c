#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "log.h"
#include "tls.h"

bool tls_init(void)
{
    // Le service est à compteur de références : sans effet si curl l'a déjà ouvert
    Result rc = sslInitialize(3);
    if (R_FAILED(rc))
        log_printf("sslInitialize : 0x%x\n", rc);
    return R_SUCCEEDED(rc);
}

void tls_exit(void)
{
    sslExit();
}

static int tcp_connect(const char *host, int port, int timeout_sec)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_printf("DNS : impossible de résoudre %s\n", host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0) {
        // Les lectures TLS passent par cette socket : le timeout évite de bloquer indéfiniment
        struct timeval tv = {.tv_sec = timeout_sec};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            log_printf("TCP : connexion à %s:%d impossible\n", host, port);
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    return fd;
}

bool tls_connect(TlsConn *t, const char *host, int port, int timeout_sec)
{
    memset(t, 0, sizeof(*t));
    t->tcp_fd = t->ssl_fd = -1;

    int fd = tcp_connect(host, port, timeout_sec);
    if (fd < 0)
        return false;

    Result rc = sslCreateContext(&t->ctx, SslVersion_Auto);
    if (R_FAILED(rc)) {
        log_printf("sslCreateContext : 0x%x\n", rc);
        close(fd);
        return false;
    }
    rc = sslContextCreateConnection(&t->ctx, &t->conn);
    if (R_FAILED(rc)) {
        log_printf("sslContextCreateConnection : 0x%x\n", rc);
        sslContextClose(&t->ctx);
        close(fd);
        return false;
    }

    // Le module SSL utilise la socket ; il peut en rendre une copie qu'il faudra fermer
    t->tcp_fd = fd;
    t->ssl_fd = socketSslConnectionSetSocketDescriptor(&t->conn, fd);
    t->open = true;

    rc = sslConnectionSetHostName(&t->conn, host, strlen(host));
    if (R_SUCCEEDED(rc))
        rc = sslConnectionSetVerifyOption(&t->conn, SslVerifyOption_PeerCa | SslVerifyOption_HostName |
                                                        SslVerifyOption_DateCheck);
    if (R_SUCCEEDED(rc))
        rc = sslConnectionDoHandshake(&t->conn, NULL, NULL, NULL, 0);
    if (R_FAILED(rc)) {
        log_printf("TLS : échec de la poignée de main avec %s (0x%x)\n", host, rc);
        tls_close(t);
        return false;
    }
    t->timeout_ms = timeout_sec * 1000;
    rc = sslConnectionSetIoMode(&t->conn, SslIoMode_NonBlocking);
    if (R_FAILED(rc))
        log_printf("TLS : mode non bloquant refusé (0x%x)\n", rc);
    return true;
}

void tls_close(TlsConn *t)
{
    if (!t->open)
        return;
    if (t->ssl_fd >= 0)
        close(t->ssl_fd);
    sslConnectionClose(&t->conn);
    sslContextClose(&t->ctx);
    close(t->tcp_fd);
    t->open = false;
}

// Mode non bloquant : le module SSL lit la socket en avance, donc on ne peut pas se fier
// à poll() sur la socket seule pour savoir s'il reste des données à lire. On tente une
// lecture, et on n'attend sur la socket que si le module n'a vraiment rien.
static bool would_block(Result rc)
{
    return R_MODULE(rc) == 123 && R_DESCRIPTION(rc) == 204;  // nn::ssl ResultWouldBlock
}

static void wait_socket(TlsConn *t, short events, int timeout_ms)
{
    struct pollfd pfd = {.fd = t->tcp_fd, .events = events};
    poll(&pfd, 1, timeout_ms);
}

// 1 : données en tampon, 0 : rien pour l'instant, -1 : connexion perdue
static int tls_fill(TlsConn *t)
{
    if (t->buf_pos < t->buf_len)
        return 1;
    if (t->dead)
        return -1;
    u32 got = 0;
    Result rc = sslConnectionRead(&t->conn, t->buf, sizeof(t->buf), &got);
    if (R_SUCCEEDED(rc) && got > 0) {
        t->buf_pos = 0;
        t->buf_len = got;
        return 1;
    }
    if (R_FAILED(rc) && would_block(rc))
        return 0;
    log_printf("TLS : lecture impossible (0x%x)\n", rc);
    t->dead = true;
    return -1;
}

bool tls_write_all(TlsConn *t, const void *data, size_t len)
{
    const u8 *p = data;
    u64 deadline = armTicksToNs(armGetSystemTick()) / 1000000ULL + t->timeout_ms;
    while (len > 0) {
        u32 written = 0;
        Result rc = sslConnectionWrite(&t->conn, p, len, &written);
        if (R_FAILED(rc) && would_block(rc) && armTicksToNs(armGetSystemTick()) / 1000000ULL < deadline) {
            wait_socket(t, POLLOUT, 100);
            continue;
        }
        if (R_FAILED(rc) || written == 0) {
            log_printf("TLS : erreur d'écriture (0x%x)\n", rc);
            t->dead = true;
            return false;
        }
        p += written;
        len -= written;
    }
    return true;
}

bool tls_read_exact(TlsConn *t, void *out, size_t len)
{
    u8 *dst = out;
    u64 deadline = armTicksToNs(armGetSystemTick()) / 1000000ULL + t->timeout_ms;
    while (len > 0) {
        int r = tls_fill(t);
        if (r < 0)
            return false;
        if (r == 0) {
            if (armTicksToNs(armGetSystemTick()) / 1000000ULL >= deadline) {
                log_printf("TLS : délai de lecture dépassé\n");
                t->dead = true;
                return false;
            }
            wait_socket(t, POLLIN, 100);
            continue;
        }
        u32 n = t->buf_len - t->buf_pos;
        if (n > len)
            n = len;
        memcpy(dst, t->buf + t->buf_pos, n);
        t->buf_pos += n;
        dst += n;
        len -= n;
    }
    return true;
}

bool tls_readable(TlsConn *t, int timeout_ms)
{
    int r = tls_fill(t);
    if (r == 0) {
        wait_socket(t, POLLIN, timeout_ms);
        r = tls_fill(t);
    }
    return r != 0;  // une erreur aussi : la lecture la constatera
}
