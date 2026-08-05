/*
 * Transport to the phone: a raw byte stream carrying the EasyTether tunnel.
 *
 * We reach it through the local ADB server (the one `adb` itself talks to on
 * 127.0.0.1:5037) rather than driving USB ourselves.  That reuses adb's device
 * enumeration and RSA authorization, and it coexists with a running adb
 * instead of fighting it for exclusive access to the USB interface.
 *
 * The sequence is exactly what `adb forward tcp:N localabstract:easytetherx`
 * performs internally:
 *
 *     -> 0012 host:transport-any            (or host:transport:<serial>)
 *     <- OKAY
 *     -> 0019 localabstract:easytetherx
 *     <- OKAY
 *     ... raw bidirectional stream to the EasyTether service on the phone ...
 */
#ifndef ET_ADB_H
#define ET_ADB_H

#include <stddef.h>

/* Abstract socket published by the EasyTether app (v1.1.15 and later). */
#define ET_ADB_SERVICE "localabstract:easytetherx"

#define ET_ADB_DEFAULT_PORT 5037

/*
 * Opens a stream to the EasyTether service.  serial may be NULL to use
 * transport-any.  Returns a connected, blocking fd on success, or -1.  On
 * failure a human-readable reason is left in err (if non-NULL).
 */
int adb_open_stream(int server_port, const char *serial, char *err, size_t errlen);

/* True if an ADB server is answering on 127.0.0.1:port. */
int adb_server_alive(int server_port);

/*
 * Try to start an ADB server by running `adb start-server`.  If as_user is
 * non-NULL the command is run through `su` as that user so the server uses
 * that user's ~/.android keys (and any authorization the phone already
 * granted them).  Returns 0 if a server is reachable afterwards.
 */
int adb_start_server(int server_port, const char *as_user, const char *adb_path);

#endif /* ET_ADB_H */
