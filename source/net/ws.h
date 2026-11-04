#pragma once

#include "tls.h"

#ifdef __cplusplus
extern "C" {
#endif

// Client WebSocket minimal (RFC 6455) au-dessus de TLS.
typedef struct {
    TlsConn tls;
} WsConn;

bool ws_connect(WsConn *ws, const char *host, int port, const char *path);
void ws_close(WsConn *ws);

bool ws_send_binary(WsConn *ws, const void *data, size_t len);
bool ws_send_text(WsConn *ws, const char *text);
// Vrai si un message commence à arriver (attend au plus `timeout_ms`).
bool ws_readable(WsConn *ws, int timeout_ms);

// Reçoit le prochain message complet (réassemble les fragments, répond aux pings).
// *out est alloué avec malloc et doit être libéré par l'appelant ; il vaut NULL
// (longueur 0) quand seule une trame de contrôle (ping/pong) est arrivée.
bool ws_recv(WsConn *ws, u8 **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
