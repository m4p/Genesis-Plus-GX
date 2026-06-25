/***************************************************************************************
 *  Genesis Plus GX / SDL2
 *
 *  Local-only JSON HTTP API exposing core/memory_api.h to external tooling
 *  for debugging and automation (peek/poke into emulated memory, plus basic
 *  pause/resume/frame-step/reset controls).
 *
 ****************************************************************************************/

#ifndef _API_SERVER_H_
#define _API_SERVER_H_

/* Starts the API server on a background thread, bound to 'bind_addr'
   (dotted IPv4, e.g. "127.0.0.1") and 'port'. Returns 0 on success, -1 on
   failure (an error is printed to stderr). */
int api_server_start(const char *bind_addr, int port);

/* Stops the background thread and closes the listening socket. Safe to call
   even if the server was never started. */
void api_server_stop(void);

/* Must be called once per emulated frame from the main/emulation thread.
   Executes at most one request that touches emulator state (memory
   peek/poke, pause/resume, single frame-step, reset) and wakes up the HTTP
   server thread that is waiting on the result. Returns 1 if a request was
   processed, 0 if nothing was pending. Cheap to call when idle. */
int api_server_process_pending(void);

/* Returns non-zero if a client has paused the emulator via POST /pause and
   it has not since been resumed via POST /resume. The main loop should skip
   stepping emulation while this is set, but must keep calling
   api_server_process_pending() every iteration so /resume, /frame and
   memory peek/poke requests are still serviced. */
int api_server_is_paused(void);

#endif /* _API_SERVER_H_ */
