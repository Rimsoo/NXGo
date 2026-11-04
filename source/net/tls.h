#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

// Connexion TCP + TLS via le module SSL système de la Switch.
typedef struct {
    SslContext ctx;
    SslConnection conn;
    int tcp_fd;              // notre socket TCP, gardée ouverte pendant la connexion
    int ssl_fd;              // copie rendue par le module SSL (à fermer aussi), -1 sinon
    bool open;
    bool dead;               // erreur de lecture constatée (connexion perdue)
    int timeout_ms;          // délai max d'une lecture/écriture complète
    u8 buf[16384];           // tampon de lecture
    u32 buf_pos, buf_len;
} TlsConn;

bool tls_init(void);
void tls_exit(void);

bool tls_connect(TlsConn *t, const char *host, int port, int timeout_sec);
void tls_close(TlsConn *t);

bool tls_write_all(TlsConn *t, const void *data, size_t len);
// Lit exactement `len` octets (bloquant, limité par le timeout de la connexion).
bool tls_read_exact(TlsConn *t, void *out, size_t len);
// Vrai si des données sont prêtes à lire (attend au plus `timeout_ms`).
bool tls_readable(TlsConn *t, int timeout_ms);

#ifdef __cplusplus
}
#endif
