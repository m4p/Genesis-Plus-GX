/***************************************************************************************
 *  Genesis Plus GX / SDL2
 *
 *  Local-only JSON HTTP API server. See api_server.h for the public
 *  interface used by sdl2/main.c.
 *
 *  Threading model: the HTTP server runs on a background SDL thread and
 *  accepts/handles one connection at a time. Requests that touch emulator
 *  state (memory access, pause/resume, frame-step, reset) are never
 *  executed on the server thread -- they are placed in a single-slot
 *  mailbox and the server thread blocks on a condition variable until the
 *  main/emulation thread drains it (once per frame, via
 *  api_server_process_pending()) and signals completion. This avoids data
 *  races against the emulator core.
 *
 *  Scope is intentionally narrow: no TLS, no auth, no routing framework, no
 *  third-party HTTP/JSON library. This is a small local debugging tool.
 *
 ****************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "SDL.h"
#include "SDL_thread.h"

#include "shared.h"
#include "memory_api.h"
#include "api_server.h"

#define API_SERVER_BACKLOG       8
#define API_HEADER_BUF_SIZE      8192
#define API_MAX_BODY_SIZE        (3 * 1024 * 1024 / 10) /* ~300KB: enough for hex-encoded 64K + JSON overhead */
#define API_ACCEPT_POLL_MS       200

/* ------------------------------------------------------------------------ */
/* Mailbox: single in-flight request handed from the server thread to the   */
/* main/emulation thread.                                                   */
/* ------------------------------------------------------------------------ */

typedef enum
{
  API_REQ_NONE = 0,
  API_REQ_PEEK,
  API_REQ_POKE,
  API_REQ_SEARCH,
  API_REQ_INPUT,
  API_REQ_SCREENSHOT,
  API_REQ_STATE_SAVE,
  API_REQ_STATE_LOAD,
  API_REQ_REGISTERS,
  API_REQ_SNAPSHOT,
  API_REQ_DIFF,
  API_REQ_PAUSE,
  API_REQ_RESUME,
  API_REQ_FRAME,
  API_REQ_RESET
} api_request_type_t;

typedef struct
{
  api_request_type_t type;
  memory_domain_id_t domain;
  uint32 address;     /* also used as search range 'start' */
  uint32 length;      /* also used as search 'pattern_length' */
  uint32 search_end;        /* search only: range 'end' (0 = domain size) */
  uint32 search_max_results; /* search only */
  uint8 buffer[MEMORY_API_MAX_TRANSFER]; /* poke input, peek output, or search pattern */
  uint32 search_offsets[MEMORY_API_MAX_SEARCH_RESULTS]; /* search only */
  uint32 search_count;       /* search only */
  uint16 input_press_mask;   /* input only: buttons to start holding */
  uint16 input_release_mask; /* input only: buttons to stop holding */
  uint16 input_held_mask;    /* input only: resulting held-button state */
  char path[512];            /* screenshot / state save / state load */
  int width, height;         /* screenshot only */
  uint32 state_bytes;        /* state save only */
  char reg_cpu[8];                 /* registers only: "m68k" / "s68k" / "z80" */
  char reg_set_names[20][8];       /* registers only: register names to write before reading back */
  uint32 reg_set_values[20];       /* registers only */
  int reg_set_count;               /* registers only */
  char reg_json[768];              /* registers only: full register-state response body, filled by process_pending */
  char snap_slot[32];                                    /* snapshot/diff only: slot name */
  char snap_filter[16];                                   /* diff only: "changed"/"unchanged"/"increased"/"decreased" */
  uint32 snap_value_size;                                 /* diff only: 1, 2 or 4 */
  uint32 diff_offsets[MEMORY_API_MAX_SEARCH_RESULTS];      /* diff only */
  uint32 diff_old_values[MEMORY_API_MAX_SEARCH_RESULTS];   /* diff only */
  uint32 diff_new_values[MEMORY_API_MAX_SEARCH_RESULTS];   /* diff only */
  uint32 diff_count;                                       /* diff only */
  int result; /* MEMORY_API_OK / MEMORY_API_ERR_* on completion for memory
                 requests; 0 on success / -1 on failure for everything else */
} api_mailbox_t;

/* Custom result codes used only by API_REQ_SNAPSHOT/API_REQ_DIFF, chosen to
   never collide with the negative MEMORY_API_ERR_* range. */
#define API_ERR_SNAPSHOT_SLOTS_FULL  (-100)
#define API_ERR_SNAPSHOT_NOT_FOUND   (-101)

#define API_MAX_SNAPSHOT_SLOTS 4

typedef struct
{
  int in_use;
  char name[32];
  memory_domain_id_t domain;
  uint32 start;
  uint32 length;
  uint8 data[MEMORY_API_MAX_TRANSFER];
} api_snapshot_t;

static api_mailbox_t mailbox;
static int request_pending  = 0; /* server thread -> main thread */
static int response_ready   = 0; /* main thread -> server thread */
static SDL_mutex *mailbox_mutex = NULL;
static SDL_cond  *mailbox_cond  = NULL;

static volatile int server_running = 0;
static volatile int api_paused     = 0;

/* Buttons currently held via POST /input. Only ever mutated from the
   main/emulation thread (inside api_server_process_pending(), itself only
   reachable via the mailbox), and only ever read from the main/emulation
   thread (sdl2/main.c's sdl_input_update(), called once per frame from the
   same loop that calls api_server_process_pending()) -- no locking needed. */
static uint16 api_held_buttons = 0;

/* Named snapshot slots for POST /snapshot and POST /diff. Only ever touched
   from the main/emulation thread, one request at a time (see threading note
   above api_held_buttons). */
static api_snapshot_t snapshots[API_MAX_SNAPSHOT_SLOTS];

static api_screenshot_fn screenshot_handler = NULL;

void api_server_set_screenshot_handler(api_screenshot_fn fn)
{
  screenshot_handler = fn;
}

/* Scratch buffer for savestate I/O. Only ever touched from the main thread,
   one request at a time (see threading note above api_held_buttons). */
static uint8 state_scratch[STATE_SIZE];
static int listen_fd = -1;
static SDL_Thread *server_thread = NULL;

/* Submits a request to the main thread and blocks until it has been
   processed. Called only from the server thread. Returns the result code
   left in req->result. */
static int submit_request(api_mailbox_t *req)
{
  SDL_LockMutex(mailbox_mutex);

  while (request_pending)
  {
    SDL_CondWait(mailbox_cond, mailbox_mutex);
  }

  mailbox = *req;
  request_pending = 1;
  response_ready = 0;
  SDL_CondBroadcast(mailbox_cond);

  while (!response_ready)
  {
    SDL_CondWait(mailbox_cond, mailbox_mutex);
  }

  *req = mailbox;
  response_ready = 0;
  SDL_CondBroadcast(mailbox_cond);

  SDL_UnlockMutex(mailbox_mutex);

  return req->result;
}

int api_server_process_pending(void)
{
  if (!mailbox_mutex)
  {
    return 0;
  }

  SDL_LockMutex(mailbox_mutex);

  if (!request_pending)
  {
    SDL_UnlockMutex(mailbox_mutex);
    return 0;
  }

  switch (mailbox.type)
  {
    case API_REQ_PEEK:
      mailbox.result = memory_api_read(mailbox.domain, mailbox.address, mailbox.buffer, mailbox.length);
      break;

    case API_REQ_POKE:
      mailbox.result = memory_api_write(mailbox.domain, mailbox.address, mailbox.buffer, mailbox.length);
      break;

    case API_REQ_SEARCH:
      mailbox.result = memory_api_search(mailbox.domain, mailbox.address, mailbox.search_end,
                                          mailbox.buffer, mailbox.length,
                                          mailbox.search_offsets, mailbox.search_max_results,
                                          &mailbox.search_count);
      break;

    case API_REQ_INPUT:
      api_held_buttons = (api_held_buttons | mailbox.input_press_mask) & (uint16)~mailbox.input_release_mask;
      mailbox.input_held_mask = api_held_buttons;
      mailbox.result = MEMORY_API_OK;
      break;

    case API_REQ_SCREENSHOT:
      if (!screenshot_handler)
      {
        mailbox.result = -1;
      }
      else
      {
        mailbox.result = screenshot_handler(mailbox.path, &mailbox.width, &mailbox.height);
      }
      break;

    case API_REQ_STATE_SAVE:
    {
      FILE *f = fopen(mailbox.path, "wb");
      if (!f)
      {
        mailbox.result = -1;
        break;
      }
      mailbox.state_bytes = (uint32)state_save(state_scratch);
      mailbox.result = (fwrite(state_scratch, mailbox.state_bytes, 1, f) == 1) ? 0 : -1;
      fclose(f);
      break;
    }

    case API_REQ_STATE_LOAD:
    {
      FILE *f = fopen(mailbox.path, "rb");
      size_t n;
      if (!f)
      {
        mailbox.result = -1;
        break;
      }
      n = fread(state_scratch, 1, STATE_SIZE, f);
      fclose(f);
      mailbox.result = ((n > 0) && state_load(state_scratch)) ? 0 : -1;
      break;
    }

    case API_REQ_REGISTERS:
    {
      int i;
      size_t off;

      if (!strcmp(mailbox.reg_cpu, "z80"))
      {
        for (i = 0; i < mailbox.reg_set_count; i++)
        {
          const char *name = mailbox.reg_set_names[i];
          uint32 value = mailbox.reg_set_values[i] & 0xFFFF;

          if (!strcmp(name, "pc")) Z80.pc.d = value;
          else if (!strcmp(name, "sp")) Z80.sp.d = value;
          else if (!strcmp(name, "af")) Z80.af.d = value;
          else if (!strcmp(name, "bc")) Z80.bc.d = value;
          else if (!strcmp(name, "de")) Z80.de.d = value;
          else if (!strcmp(name, "hl")) Z80.hl.d = value;
          else if (!strcmp(name, "ix")) Z80.ix.d = value;
          else if (!strcmp(name, "iy")) Z80.iy.d = value;
        }

        snprintf(mailbox.reg_json, sizeof(mailbox.reg_json),
                 "{\"cpu\":\"z80\",\"pc\":%u,\"sp\":%u,\"af\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u,\"ix\":%u,\"iy\":%u,"
                 "\"af2\":%u,\"bc2\":%u,\"de2\":%u,\"hl2\":%u,\"halted\":%s}",
                 Z80.pc.d & 0xFFFF, Z80.sp.d & 0xFFFF, Z80.af.d & 0xFFFF, Z80.bc.d & 0xFFFF,
                 Z80.de.d & 0xFFFF, Z80.hl.d & 0xFFFF, Z80.ix.d & 0xFFFF, Z80.iy.d & 0xFFFF,
                 Z80.af2.d & 0xFFFF, Z80.bc2.d & 0xFFFF, Z80.de2.d & 0xFFFF, Z80.hl2.d & 0xFFFF,
                 Z80.halt ? "true" : "false");
      }
      else
      {
        m68ki_cpu_core *cpu = !strcmp(mailbox.reg_cpu, "s68k") ? &s68k : &m68k;
        static const char *dnames[8] = { "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7" };
        static const char *anames[8] = { "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7" };
        uint32 ccr, sr, usp, ssp;
        int j;

        for (i = 0; i < mailbox.reg_set_count; i++)
        {
          const char *name = mailbox.reg_set_names[i];
          uint32 value = mailbox.reg_set_values[i];
          int matched = 0;

          for (j = 0; j < 8; j++)
          {
            if (!strcmp(name, dnames[j])) { cpu->dar[j] = value; matched = 1; break; }
            if (!strcmp(name, anames[j])) { cpu->dar[8 + j] = value; matched = 1; break; }
          }
          if (!matched && !strcmp(name, "pc"))
          {
            cpu->pc = value;
          }
        }

        ccr = ((cpu->x_flag & 0x100) >> 4)
            | ((cpu->n_flag & 0x80) >> 4)
            | (cpu->not_z_flag ? 0 : 4)
            | ((cpu->v_flag & 0x80) >> 6)
            | ((cpu->c_flag & 0x100) >> 8);
        sr = cpu->t1_flag | (cpu->s_flag << 11) | cpu->int_mask | ccr;
        usp = cpu->s_flag ? cpu->sp[0] : cpu->dar[15];
        ssp = cpu->s_flag ? cpu->dar[15] : cpu->sp[4];

        off = (size_t)snprintf(mailbox.reg_json, sizeof(mailbox.reg_json),
                                "{\"cpu\":\"%s\",\"pc\":%u,\"sr\":%u,\"usp\":%u,\"ssp\":%u,\"halted\":%s,\"d\":[",
                                mailbox.reg_cpu, cpu->pc, sr, usp, ssp, cpu->stopped ? "true" : "false");
        for (i = 0; i < 8; i++)
        {
          off += (size_t)snprintf(mailbox.reg_json + off, sizeof(mailbox.reg_json) - off, "%s%u", i ? "," : "", cpu->dar[i]);
        }
        off += (size_t)snprintf(mailbox.reg_json + off, sizeof(mailbox.reg_json) - off, "],\"a\":[");
        for (i = 0; i < 8; i++)
        {
          off += (size_t)snprintf(mailbox.reg_json + off, sizeof(mailbox.reg_json) - off, "%s%u", i ? "," : "", cpu->dar[8 + i]);
        }
        snprintf(mailbox.reg_json + off, sizeof(mailbox.reg_json) - off, "]}");
      }

      mailbox.result = 0;
      break;
    }

    case API_REQ_SNAPSHOT:
    {
      api_snapshot_t *slot = NULL;
      int i;

      for (i = 0; i < API_MAX_SNAPSHOT_SLOTS; i++)
      {
        if (snapshots[i].in_use && !strcmp(snapshots[i].name, mailbox.snap_slot))
        {
          slot = &snapshots[i];
          break;
        }
      }
      if (!slot)
      {
        for (i = 0; i < API_MAX_SNAPSHOT_SLOTS; i++)
        {
          if (!snapshots[i].in_use)
          {
            slot = &snapshots[i];
            break;
          }
        }
      }
      if (!slot)
      {
        mailbox.result = API_ERR_SNAPSHOT_SLOTS_FULL;
        break;
      }

      {
        const memory_domain_info_t *domains;
        int count;
        uint32 dom_size = 0;
        uint32 start = mailbox.address;
        uint32 end = mailbox.search_end;

        memory_api_list_domains(&domains, &count);
        if ((mailbox.domain >= 0) && (mailbox.domain < count))
        {
          dom_size = domains[mailbox.domain].size;
        }

        if ((end == 0) || (end > dom_size))
        {
          end = dom_size;
        }
        if ((start > end) || ((end - start) > MEMORY_API_MAX_TRANSFER))
        {
          mailbox.result = MEMORY_API_ERR_INVALID_RANGE;
          break;
        }

        mailbox.result = memory_api_read(mailbox.domain, start, slot->data, end - start);
        if (mailbox.result == MEMORY_API_OK)
        {
          strncpy(slot->name, mailbox.snap_slot, sizeof(slot->name) - 1);
          slot->name[sizeof(slot->name) - 1] = '\0';
          slot->domain = mailbox.domain;
          slot->start = start;
          slot->length = end - start;
          slot->in_use = 1;
          mailbox.address = start;
          mailbox.length = slot->length;
        }
      }
      break;
    }

    case API_REQ_DIFF:
    {
      api_snapshot_t *slot = NULL;
      int i;
      static uint8 diff_live[MEMORY_API_MAX_TRANSFER];

      for (i = 0; i < API_MAX_SNAPSHOT_SLOTS; i++)
      {
        if (snapshots[i].in_use && !strcmp(snapshots[i].name, mailbox.snap_slot))
        {
          slot = &snapshots[i];
          break;
        }
      }
      if (!slot)
      {
        mailbox.result = API_ERR_SNAPSHOT_NOT_FOUND;
        break;
      }

      mailbox.domain = slot->domain;
      mailbox.address = slot->start;
      mailbox.length = slot->length;

      mailbox.result = memory_api_read(slot->domain, slot->start, diff_live, slot->length);
      if (mailbox.result != MEMORY_API_OK)
      {
        break;
      }

      {
        uint32 vs = mailbox.snap_value_size ? mailbox.snap_value_size : 1;
        uint32 max_results = mailbox.search_max_results ? mailbox.search_max_results : 64;
        uint32 count = 0;
        uint32 off;

        if (max_results > MEMORY_API_MAX_SEARCH_RESULTS)
        {
          max_results = MEMORY_API_MAX_SEARCH_RESULTS;
        }

        for (off = 0; ((off + vs) <= slot->length) && (count < max_results); off += vs)
        {
          uint32 oldv = 0, newv = 0;
          uint32 k;
          int keep;

          for (k = 0; k < vs; k++)
          {
            oldv = (oldv << 8) | slot->data[off + k];
            newv = (newv << 8) | diff_live[off + k];
          }

          if (!strcmp(mailbox.snap_filter, "changed")) keep = (oldv != newv);
          else if (!strcmp(mailbox.snap_filter, "unchanged")) keep = (oldv == newv);
          else if (!strcmp(mailbox.snap_filter, "increased")) keep = (newv > oldv);
          else if (!strcmp(mailbox.snap_filter, "decreased")) keep = (newv < oldv);
          else keep = 0;

          if (keep)
          {
            mailbox.diff_offsets[count] = slot->start + off;
            mailbox.diff_old_values[count] = oldv;
            mailbox.diff_new_values[count] = newv;
            count++;
          }
        }

        mailbox.diff_count = count;
      }

      mailbox.result = MEMORY_API_OK;
      break;
    }

    case API_REQ_PAUSE:
      api_paused = 1;
      mailbox.result = MEMORY_API_OK;
      break;

    case API_REQ_RESUME:
      api_paused = 0;
      mailbox.result = MEMORY_API_OK;
      break;

    case API_REQ_FRAME:
      if (system_hw == SYSTEM_MCD)
      {
        system_frame_scd(0);
      }
      else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
      {
        system_frame_gen(0);
      }
      else
      {
        system_frame_sms(0);
      }
      mailbox.result = MEMORY_API_OK;
      break;

    case API_REQ_RESET:
      system_reset();
      mailbox.result = MEMORY_API_OK;
      break;

    default:
      mailbox.result = MEMORY_API_ERR_UNKNOWN_DOMAIN;
      break;
  }

  request_pending = 0;
  response_ready = 1;
  SDL_CondBroadcast(mailbox_cond);
  SDL_UnlockMutex(mailbox_mutex);

  return 1;
}

int api_server_is_paused(void)
{
  return api_paused;
}

uint16 api_server_get_input_overlay(void)
{
  return api_held_buttons;
}

/* ------------------------------------------------------------------------ */
/* Minimal JSON helpers (flat objects only: string and integer values)      */
/* ------------------------------------------------------------------------ */

/* Finds `"key"` followed by optional whitespace and ':' anywhere in 'json'
   and returns a pointer just past the ':'. Good enough for the small flat
   request objects this API accepts; not a general JSON parser. */
static const char *json_find_key(const char *json, const char *key)
{
  size_t keylen = strlen(key);
  const char *p = json;

  while ((p = strchr(p, '"')) != NULL)
  {
    if (!strncmp(p + 1, key, keylen) && p[1 + keylen] == '"')
    {
      const char *after = p + 2 + keylen;
      while ((*after == ' ') || (*after == '\t')) after++;
      if (*after == ':')
      {
        return after + 1;
      }
    }
    p++;
  }

  return NULL;
}

static int json_get_string(const char *json, const char *key, char *out, size_t outsz)
{
  const char *v = json_find_key(json, key);
  size_t i = 0;

  if (!v) return 0;
  while ((*v == ' ') || (*v == '\t')) v++;
  if (*v != '"') return 0;
  v++;

  while (*v && (*v != '"') && (i + 1 < outsz))
  {
    if ((*v == '\\') && v[1])
    {
      v++;
    }
    out[i++] = *v++;
  }

  if (*v != '"') return 0;
  out[i] = '\0';
  return 1;
}

static int json_get_uint(const char *json, const char *key, uint32 *out)
{
  const char *v = json_find_key(json, key);
  char *end;
  unsigned long val;

  if (!v) return 0;
  while ((*v == ' ') || (*v == '\t')) v++;
  if (!isdigit((unsigned char)*v)) return 0;

  val = strtoul(v, &end, 10);
  if (end == v) return 0;

  *out = (uint32)val;
  return 1;
}

#define JSON_ARRAY_NAME_CAP 32

/* Parses a JSON array of strings for 'key' (e.g. ["a","start"]), writing up
   to 'max_names' NUL-terminated entries (each truncated to
   JSON_ARRAY_NAME_CAP-1 chars) into out_names. Returns the number of names
   parsed, 0 if 'key' is absent (treated as an empty list), or -1 if 'key'
   is present but is not a well-formed array of strings. */
static int json_get_string_array(const char *json, const char *key,
                                  char out_names[][JSON_ARRAY_NAME_CAP], int max_names)
{
  const char *v = json_find_key(json, key);
  int count = 0;

  if (!v) return 0;

  while ((*v == ' ') || (*v == '\t')) v++;
  if (*v != '[') return -1;
  v++;

  while ((*v == ' ') || (*v == '\t') || (*v == '\n') || (*v == '\r')) v++;
  if (*v == ']') return 0;

  for (;;)
  {
    size_t i = 0;

    while ((*v == ' ') || (*v == '\t')) v++;
    if (*v != '"') return -1;
    v++;

    if (count >= max_names) return -1;

    while (*v && (*v != '"') && (i + 1 < JSON_ARRAY_NAME_CAP))
    {
      out_names[count][i++] = *v++;
    }
    if (*v != '"') return -1;
    v++;
    out_names[count][i] = '\0';
    count++;

    while ((*v == ' ') || (*v == '\t')) v++;
    if (*v == ',') { v++; continue; }
    if (*v == ']') break;
    return -1;
  }

  return count;
}

/* ------------------------------------------------------------------------ */
/* hex / base64 encoding helpers                                            */
/* ------------------------------------------------------------------------ */

static void hex_encode(char *out, const uint8 *data, uint32 len)
{
  static const char digits[] = "0123456789abcdef";
  uint32 i;

  for (i = 0; i < len; i++)
  {
    out[i * 2]     = digits[(data[i] >> 4) & 0xF];
    out[i * 2 + 1] = digits[data[i] & 0xF];
  }
  out[len * 2] = '\0';
}

static int hex_nibble(char c)
{
  if ((c >= '0') && (c <= '9')) return c - '0';
  if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
  if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
  return -1;
}

/* Returns decoded length, or -1 on malformed input / overflow. */
static int hex_decode(uint8 *out, uint32 out_max, const char *hex)
{
  size_t len = strlen(hex);
  uint32 i;

  if ((len % 2) != 0) return -1;
  if ((len / 2) > out_max) return -1;

  for (i = 0; i < len / 2; i++)
  {
    int hi = hex_nibble(hex[i * 2]);
    int lo = hex_nibble(hex[i * 2 + 1]);
    if ((hi < 0) || (lo < 0)) return -1;
    out[i] = (uint8)((hi << 4) | lo);
  }

  return (int)(len / 2);
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(char *out, const uint8 *data, uint32 len)
{
  uint32 i = 0, o = 0;

  while (i + 3 <= len)
  {
    uint32 v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out[o++] = b64_table[(v >> 18) & 0x3F];
    out[o++] = b64_table[(v >> 12) & 0x3F];
    out[o++] = b64_table[(v >> 6) & 0x3F];
    out[o++] = b64_table[v & 0x3F];
    i += 3;
  }

  if (len - i == 1)
  {
    uint32 v = data[i] << 16;
    out[o++] = b64_table[(v >> 18) & 0x3F];
    out[o++] = b64_table[(v >> 12) & 0x3F];
    out[o++] = '=';
    out[o++] = '=';
  }
  else if (len - i == 2)
  {
    uint32 v = (data[i] << 16) | (data[i + 1] << 8);
    out[o++] = b64_table[(v >> 18) & 0x3F];
    out[o++] = b64_table[(v >> 12) & 0x3F];
    out[o++] = b64_table[(v >> 6) & 0x3F];
    out[o++] = '=';
  }

  out[o] = '\0';
}

static int base64_val(char c)
{
  const char *p = strchr(b64_table, c);
  if (!p || (c == '\0')) return -1;
  return (int)(p - b64_table);
}

/* Returns decoded length, or -1 on malformed input / overflow. */
static int base64_decode(uint8 *out, uint32 out_max, const char *b64)
{
  size_t len = strlen(b64);
  uint32 o = 0;
  size_t i = 0;

  if ((len % 4) != 0) return -1;

  while (i < len)
  {
    int v0, v1, v2, v3;
    int pad = 0;

    v0 = base64_val(b64[i]);
    v1 = base64_val(b64[i + 1]);
    if ((v0 < 0) || (v1 < 0)) return -1;

    if (b64[i + 2] == '=')
    {
      pad = 2;
      v2 = v3 = 0;
    }
    else
    {
      v2 = base64_val(b64[i + 2]);
      if (v2 < 0) return -1;

      if (b64[i + 3] == '=')
      {
        pad = 1;
        v3 = 0;
      }
      else
      {
        v3 = base64_val(b64[i + 3]);
        if (v3 < 0) return -1;
      }
    }

    if (o >= out_max) return -1;
    out[o++] = (uint8)((v0 << 2) | (v1 >> 4));

    if (pad < 2)
    {
      if (o >= out_max) return -1;
      out[o++] = (uint8)(((v1 & 0xF) << 4) | (v2 >> 2));
    }
    if (pad < 1)
    {
      if (o >= out_max) return -1;
      out[o++] = (uint8)(((v2 & 0x3) << 6) | v3);
    }

    i += 4;
  }

  return (int)o;
}

/* ------------------------------------------------------------------------ */
/* HTTP plumbing                                                            */
/* ------------------------------------------------------------------------ */

static int send_all(int fd, const char *data, size_t len)
{
  size_t sent = 0;
  while (sent < len)
  {
    ssize_t n = send(fd, data + sent, len - sent, 0);
    if (n <= 0) return -1;
    sent += (size_t)n;
  }
  return 0;
}

static void send_http_response(int fd, int status, const char *status_text, const char *json_body)
{
  char header[256];
  size_t body_len = strlen(json_body);

  snprintf(header, sizeof(header),
           "HTTP/1.1 %d %s\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: %zu\r\n"
           "Connection: close\r\n"
           "\r\n",
           status, status_text, body_len);

  if (send_all(fd, header, strlen(header)) == 0)
  {
    send_all(fd, json_body, body_len);
  }
}

static void send_ok(int fd, const char *json_body)
{
  send_http_response(fd, 200, "OK", json_body);
}

static void send_error(int fd, int status, const char *code, const char *message)
{
  char body[512];
  snprintf(body, sizeof(body),
           "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
           code, message);
  send_http_response(fd, status, (status == 404) ? "Not Found" : "Bad Request", body);
}

/* Translates a memory_api error code into an HTTP error response. */
static void send_memory_api_error(int fd, int err)
{
  switch (err)
  {
    case MEMORY_API_ERR_UNKNOWN_DOMAIN:
      send_error(fd, 400, "unknown_domain", "Unknown memory domain");
      break;
    case MEMORY_API_ERR_UNSUPPORTED_DOMAIN:
      send_error(fd, 400, "unknown_domain", "Memory domain is not available for the currently running system");
      break;
    case MEMORY_API_ERR_INVALID_RANGE:
      send_error(fd, 400, "invalid_range", "Address range exceeds memory domain size");
      break;
    case MEMORY_API_ERR_INVALID_LENGTH:
      send_error(fd, 400, "invalid_length", "Length must be non-zero and within the per-request limit");
      break;
    case MEMORY_API_ERR_NOT_READABLE:
      send_error(fd, 400, "domain_not_readable", "Memory domain is not readable");
      break;
    case MEMORY_API_ERR_NOT_WRITABLE:
      send_error(fd, 400, "domain_not_writable", "Memory domain is not writable");
      break;
    case MEMORY_API_ERR_INVALID_PATTERN:
      send_error(fd, 400, "invalid_pattern", "Pattern must be non-empty and within the per-request limit");
      break;
    default:
      send_error(fd, 500, "internal_error", "Internal error");
      break;
  }
}

static void handle_health(int fd)
{
  send_ok(fd, "{\"ok\":true,\"emulator\":\"Genesis Plus GX\",\"api\":\"peekpoke-v1\"}");
}

static void handle_domains(int fd)
{
  const memory_domain_info_t *domains;
  int count, i;
  char body[2048];
  size_t off;

  memory_api_list_domains(&domains, &count);

  off = (size_t)snprintf(body, sizeof(body), "{\"domains\":[");
  for (i = 0; i < count; i++)
  {
    off += (size_t)snprintf(body + off, sizeof(body) - off,
                             "%s{\"id\":\"%s\",\"size\":%u,\"readable\":%s,\"writable\":%s}",
                             (i > 0) ? "," : "",
                             domains[i].name,
                             domains[i].size,
                             domains[i].readable ? "true" : "false",
                             domains[i].writable ? "true" : "false");
  }
  snprintf(body + off, sizeof(body) - off, "]}");

  send_ok(fd, body);
}

static void handle_peek(int fd, const char *json)
{
  char domain_name[64];
  char encoding[16];
  uint32 address = 0, length = 0;
  memory_domain_id_t domain_id;
  api_mailbox_t req;
  char *out_text;
  size_t out_cap;

  if (!json_get_string(json, "domain", domain_name, sizeof(domain_name)))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'domain' field");
    return;
  }
  if (!json_get_uint(json, "address", &address))
  {
    send_error(fd, 400, "invalid_address", "Missing or invalid 'address' field");
    return;
  }
  if (!json_get_uint(json, "length", &length))
  {
    send_error(fd, 400, "invalid_length", "Missing or invalid 'length' field");
    return;
  }
  if (!json_get_string(json, "encoding", encoding, sizeof(encoding)))
  {
    send_error(fd, 400, "invalid_encoding", "Missing or invalid 'encoding' field");
    return;
  }
  if (strcmp(encoding, "hex") && strcmp(encoding, "base64"))
  {
    send_error(fd, 400, "invalid_encoding", "Supported encodings are 'hex' and 'base64'");
    return;
  }
  if (memory_api_find_domain(domain_name, &domain_id) != MEMORY_API_OK)
  {
    send_error(fd, 400, "unknown_domain", "Unknown memory domain");
    return;
  }
  if ((length == 0) || (length > MEMORY_API_MAX_TRANSFER))
  {
    send_error(fd, 400, "invalid_length", "Length must be non-zero and within the per-request limit");
    return;
  }

  memset(&req, 0, sizeof(req));
  req.type = API_REQ_PEEK;
  req.domain = domain_id;
  req.address = address;
  req.length = length;

  if (submit_request(&req) != MEMORY_API_OK)
  {
    send_memory_api_error(fd, req.result);
    return;
  }

  out_cap = ((size_t)length * 2) + 64; /* base64 needs less, hex needs len*2 */
  out_text = (char *)malloc(out_cap + 512);
  if (!out_text)
  {
    send_error(fd, 500, "internal_error", "Out of memory");
    return;
  }

  {
    char data_field[2 * MEMORY_API_MAX_TRANSFER + 4];
    if (!strcmp(encoding, "hex"))
    {
      hex_encode(data_field, req.buffer, length);
    }
    else
    {
      base64_encode(data_field, req.buffer, length);
    }

    snprintf(out_text, out_cap + 512,
             "{\"domain\":\"%s\",\"address\":%u,\"length\":%u,\"encoding\":\"%s\",\"data\":\"%s\"}",
             domain_name, address, length, encoding, data_field);
  }

  send_ok(fd, out_text);
  free(out_text);
}

static void handle_poke(int fd, const char *json)
{
  char domain_name[64];
  char encoding[16];
  char data_field[2 * MEMORY_API_MAX_TRANSFER + 4];
  uint32 address = 0;
  memory_domain_id_t domain_id;
  api_mailbox_t req;
  int decoded_len;
  char body[256];

  if (!json_get_string(json, "domain", domain_name, sizeof(domain_name)))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'domain' field");
    return;
  }
  if (!json_get_uint(json, "address", &address))
  {
    send_error(fd, 400, "invalid_address", "Missing or invalid 'address' field");
    return;
  }
  if (!json_get_string(json, "encoding", encoding, sizeof(encoding)))
  {
    send_error(fd, 400, "invalid_encoding", "Missing or invalid 'encoding' field");
    return;
  }
  if (strcmp(encoding, "hex") && strcmp(encoding, "base64"))
  {
    send_error(fd, 400, "invalid_encoding", "Supported encodings are 'hex' and 'base64'");
    return;
  }
  if (!json_get_string(json, "data", data_field, sizeof(data_field)))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'data' field");
    return;
  }
  if (memory_api_find_domain(domain_name, &domain_id) != MEMORY_API_OK)
  {
    send_error(fd, 400, "unknown_domain", "Unknown memory domain");
    return;
  }

  memset(&req, 0, sizeof(req));

  if (!strcmp(encoding, "hex"))
  {
    decoded_len = hex_decode(req.buffer, sizeof(req.buffer), data_field);
  }
  else
  {
    decoded_len = base64_decode(req.buffer, sizeof(req.buffer), data_field);
  }

  if (decoded_len < 0)
  {
    send_error(fd, 400, "decode_failed", "Could not decode 'data' using the given encoding");
    return;
  }
  if (decoded_len == 0)
  {
    send_error(fd, 400, "invalid_length", "Length must be non-zero and within the per-request limit");
    return;
  }

  req.type = API_REQ_POKE;
  req.domain = domain_id;
  req.address = address;
  req.length = (uint32)decoded_len;

  if (submit_request(&req) != MEMORY_API_OK)
  {
    send_memory_api_error(fd, req.result);
    return;
  }

  snprintf(body, sizeof(body),
           "{\"ok\":true,\"domain\":\"%s\",\"address\":%u,\"bytes_written\":%d}",
           domain_name, address, decoded_len);
  send_ok(fd, body);
}

static void handle_search(int fd, const char *json)
{
  char domain_name[64];
  char encoding[16];
  char pattern_field[2 * MEMORY_API_MAX_SEARCH_PATTERN + 4];
  uint32 start = 0, end = 0, max_results = 64;
  memory_domain_id_t domain_id;
  api_mailbox_t req;
  int decoded_len;
  char *out_text;
  size_t out_cap;
  size_t off;
  uint32 i;

  if (!json_get_string(json, "domain", domain_name, sizeof(domain_name)))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'domain' field");
    return;
  }
  if (!json_get_string(json, "encoding", encoding, sizeof(encoding)))
  {
    send_error(fd, 400, "invalid_encoding", "Missing or invalid 'encoding' field");
    return;
  }
  if (strcmp(encoding, "hex") && strcmp(encoding, "base64"))
  {
    send_error(fd, 400, "invalid_encoding", "Supported encodings are 'hex' and 'base64'");
    return;
  }
  if (!json_get_string(json, "pattern", pattern_field, sizeof(pattern_field)))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'pattern' field");
    return;
  }
  if (memory_api_find_domain(domain_name, &domain_id) != MEMORY_API_OK)
  {
    send_error(fd, 400, "unknown_domain", "Unknown memory domain");
    return;
  }

  /* 'start', 'end' and 'max_results' are all optional */
  json_get_uint(json, "start", &start);
  json_get_uint(json, "end", &end);
  if (json_get_uint(json, "max_results", &max_results))
  {
    if (max_results == 0)
    {
      max_results = 1;
    }
    if (max_results > MEMORY_API_MAX_SEARCH_RESULTS)
    {
      max_results = MEMORY_API_MAX_SEARCH_RESULTS;
    }
  }

  memset(&req, 0, sizeof(req));

  if (!strcmp(encoding, "hex"))
  {
    decoded_len = hex_decode(req.buffer, sizeof(req.buffer), pattern_field);
  }
  else
  {
    decoded_len = base64_decode(req.buffer, sizeof(req.buffer), pattern_field);
  }

  if (decoded_len < 0)
  {
    send_error(fd, 400, "decode_failed", "Could not decode 'pattern' using the given encoding");
    return;
  }
  if ((decoded_len == 0) || (decoded_len > MEMORY_API_MAX_SEARCH_PATTERN))
  {
    send_error(fd, 400, "invalid_pattern", "Pattern must be non-empty and within the per-request limit");
    return;
  }

  req.type = API_REQ_SEARCH;
  req.domain = domain_id;
  req.address = start;
  req.search_end = end;
  req.length = (uint32)decoded_len;
  req.search_max_results = max_results;

  if (submit_request(&req) != MEMORY_API_OK)
  {
    send_memory_api_error(fd, req.result);
    return;
  }

  out_cap = ((size_t)req.search_count * 12) + 256;
  out_text = (char *)malloc(out_cap);
  if (!out_text)
  {
    send_error(fd, 500, "internal_error", "Out of memory");
    return;
  }

  off = (size_t)snprintf(out_text, out_cap,
                          "{\"domain\":\"%s\",\"pattern_length\":%d,\"count\":%u,\"offsets\":[",
                          domain_name, decoded_len, req.search_count);
  for (i = 0; i < req.search_count; i++)
  {
    off += (size_t)snprintf(out_text + off, out_cap - off, "%s%u", (i > 0) ? "," : "", req.search_offsets[i]);
  }
  snprintf(out_text + off, out_cap - off, "]}");

  send_ok(fd, out_text);
  free(out_text);
}

static void handle_snapshot(int fd, const char *json)
{
  char slot[32];
  char domain_name[64];
  memory_domain_id_t domain_id;
  uint32 start = 0, end = 0;
  api_mailbox_t req;
  char body[256];

  if (!json_get_string(json, "slot", slot, sizeof(slot)))
  {
    strncpy(slot, "default", sizeof(slot) - 1);
    slot[sizeof(slot) - 1] = '\0';
  }
  if (!json_get_string(json, "domain", domain_name, sizeof(domain_name)))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'domain' field");
    return;
  }
  if (memory_api_find_domain(domain_name, &domain_id) != MEMORY_API_OK)
  {
    send_error(fd, 400, "unknown_domain", "Unknown memory domain");
    return;
  }

  /* 'start' and 'end' are optional; end defaults to the domain's size */
  json_get_uint(json, "start", &start);
  json_get_uint(json, "end", &end);

  memset(&req, 0, sizeof(req));
  req.type = API_REQ_SNAPSHOT;
  req.domain = domain_id;
  req.address = start;
  req.search_end = end;
  strncpy(req.snap_slot, slot, sizeof(req.snap_slot) - 1);

  submit_request(&req);

  if (req.result == API_ERR_SNAPSHOT_SLOTS_FULL)
  {
    send_error(fd, 400, "too_many_snapshots",
               "Maximum number of snapshot slots (4) already in use; diff or reuse an existing slot name");
    return;
  }
  if (req.result != MEMORY_API_OK)
  {
    send_memory_api_error(fd, req.result);
    return;
  }

  snprintf(body, sizeof(body),
           "{\"ok\":true,\"slot\":\"%s\",\"domain\":\"%s\",\"start\":%u,\"length\":%u}",
           slot, domain_name, req.address, req.length);
  send_ok(fd, body);
}

static void handle_diff(int fd, const char *json)
{
  char slot[32];
  char filter[16];
  uint32 value_size = 1, max_results = 64;
  api_mailbox_t req;
  char *out_text;
  size_t out_cap, off;
  uint32 i;
  const memory_domain_info_t *domains;
  int count;
  const char *domain_name = "";

  if (!json_get_string(json, "slot", slot, sizeof(slot)))
  {
    strncpy(slot, "default", sizeof(slot) - 1);
    slot[sizeof(slot) - 1] = '\0';
  }
  if (!json_get_string(json, "filter", filter, sizeof(filter)))
  {
    strncpy(filter, "changed", sizeof(filter) - 1);
    filter[sizeof(filter) - 1] = '\0';
  }
  if (strcmp(filter, "changed") && strcmp(filter, "unchanged") && strcmp(filter, "increased") && strcmp(filter, "decreased"))
  {
    send_error(fd, 400, "invalid_filter", "Supported filters are 'changed', 'unchanged', 'increased', 'decreased'");
    return;
  }
  if (json_get_uint(json, "value_size", &value_size))
  {
    if ((value_size != 1) && (value_size != 2) && (value_size != 4))
    {
      send_error(fd, 400, "invalid_value_size", "'value_size' must be 1, 2 or 4");
      return;
    }
  }
  if (json_get_uint(json, "max_results", &max_results))
  {
    if (max_results == 0)
    {
      max_results = 1;
    }
    if (max_results > MEMORY_API_MAX_SEARCH_RESULTS)
    {
      max_results = MEMORY_API_MAX_SEARCH_RESULTS;
    }
  }

  memset(&req, 0, sizeof(req));
  req.type = API_REQ_DIFF;
  strncpy(req.snap_slot, slot, sizeof(req.snap_slot) - 1);
  strncpy(req.snap_filter, filter, sizeof(req.snap_filter) - 1);
  req.snap_value_size = value_size;
  req.search_max_results = max_results;

  submit_request(&req);

  if (req.result == API_ERR_SNAPSHOT_NOT_FOUND)
  {
    send_error(fd, 404, "snapshot_not_found", "No snapshot exists for this slot; call POST /snapshot first");
    return;
  }
  if (req.result != MEMORY_API_OK)
  {
    send_memory_api_error(fd, req.result);
    return;
  }

  memory_api_list_domains(&domains, &count);
  if ((req.domain >= 0) && (req.domain < count))
  {
    domain_name = domains[req.domain].name;
  }

  out_cap = ((size_t)req.diff_count * 40) + 256;
  out_text = (char *)malloc(out_cap);
  if (!out_text)
  {
    send_error(fd, 500, "internal_error", "Out of memory");
    return;
  }

  off = (size_t)snprintf(out_text, out_cap,
                          "{\"slot\":\"%s\",\"domain\":\"%s\",\"filter\":\"%s\",\"value_size\":%u,\"count\":%u,\"matches\":[",
                          slot, domain_name, filter, value_size, req.diff_count);
  for (i = 0; i < req.diff_count; i++)
  {
    off += (size_t)snprintf(out_text + off, out_cap - off, "%s{\"offset\":%u,\"old\":%u,\"new\":%u}",
                             (i > 0) ? "," : "", req.diff_offsets[i], req.diff_old_values[i], req.diff_new_values[i]);
  }
  snprintf(out_text + off, out_cap - off, "]}");

  send_ok(fd, out_text);
  free(out_text);
}

/* Full Mega Drive / Mega CD 6-button pad layout. */
typedef struct
{
  const char *name;
  uint16 bit;
} button_def_t;

static const button_def_t button_table[] =
{
  { "up",    INPUT_UP },
  { "down",  INPUT_DOWN },
  { "left",  INPUT_LEFT },
  { "right", INPUT_RIGHT },
  { "a",     INPUT_A },
  { "b",     INPUT_B },
  { "c",     INPUT_C },
  { "x",     INPUT_X },
  { "y",     INPUT_Y },
  { "z",     INPUT_Z },
  { "start", INPUT_START },
  { "mode",  INPUT_MODE }
};

#define BUTTON_TABLE_COUNT (sizeof(button_table) / sizeof(button_table[0]))

static int button_name_to_bit(const char *name, uint16 *out_bit)
{
  size_t i;

  for (i = 0; i < BUTTON_TABLE_COUNT; i++)
  {
    if (!strcmp(button_table[i].name, name))
    {
      *out_bit = button_table[i].bit;
      return 1;
    }
  }

  return 0;
}

static void handle_input(int fd, const char *json)
{
  char press_names[16][JSON_ARRAY_NAME_CAP];
  char release_names[16][JSON_ARRAY_NAME_CAP];
  int press_count, release_count, i;
  uint16 press_mask = 0, release_mask = 0;
  api_mailbox_t req;
  char body[512];
  size_t off;
  int first;

  press_count = json_get_string_array(json, "press", press_names, 16);
  release_count = json_get_string_array(json, "release", release_names, 16);

  if ((press_count < 0) || (release_count < 0))
  {
    send_error(fd, 400, "bad_json", "'press' and 'release' must be arrays of button name strings");
    return;
  }

  for (i = 0; i < press_count; i++)
  {
    uint16 bit;
    if (!button_name_to_bit(press_names[i], &bit))
    {
      send_error(fd, 400, "unknown_button", "Unknown button name");
      return;
    }
    press_mask |= bit;
  }

  for (i = 0; i < release_count; i++)
  {
    uint16 bit;
    if (!button_name_to_bit(release_names[i], &bit))
    {
      send_error(fd, 400, "unknown_button", "Unknown button name");
      return;
    }
    release_mask |= bit;
  }

  memset(&req, 0, sizeof(req));
  req.type = API_REQ_INPUT;
  req.input_press_mask = press_mask;
  req.input_release_mask = release_mask;

  submit_request(&req);

  off = (size_t)snprintf(body, sizeof(body), "{\"ok\":true,\"held\":[");
  first = 1;
  for (i = 0; i < (int)BUTTON_TABLE_COUNT; i++)
  {
    if (req.input_held_mask & button_table[i].bit)
    {
      off += (size_t)snprintf(body + off, sizeof(body) - off, "%s\"%s\"", first ? "" : ",", button_table[i].name);
      first = 0;
    }
  }
  snprintf(body + off, sizeof(body) - off, "]}");

  send_ok(fd, body);
}

/* Register names that POST /registers will accept as 'set' fields, per CPU.
   A field is treated as a write request if json_get_uint() finds it
   anywhere in the request body -- this minimal parser doesn't distinguish
   nesting, so {"cpu":"m68k","d0":1} and {"cpu":"m68k","set":{"d0":1}} are
   equivalent; the latter is just clearer to read in client code. */
static const char *m68k_settable_names[] = { "d0","d1","d2","d3","d4","d5","d6","d7",
                                              "a0","a1","a2","a3","a4","a5","a6","a7","pc" };
static const char *z80_settable_names[] = { "pc","sp","af","bc","de","hl","ix","iy" };

static void handle_registers(int fd, const char *json)
{
  char cpu_name[8];
  api_mailbox_t req;
  const char **names;
  size_t name_count, i;

  if (!json_get_string(json, "cpu", cpu_name, sizeof(cpu_name)))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'cpu' field");
    return;
  }
  if (strcmp(cpu_name, "m68k") && strcmp(cpu_name, "s68k") && strcmp(cpu_name, "z80"))
  {
    send_error(fd, 400, "unknown_cpu", "Supported values for 'cpu' are 'm68k', 's68k' and 'z80'");
    return;
  }

  memset(&req, 0, sizeof(req));
  req.type = API_REQ_REGISTERS;
  strncpy(req.reg_cpu, cpu_name, sizeof(req.reg_cpu) - 1);

  if (!strcmp(cpu_name, "z80"))
  {
    names = z80_settable_names;
    name_count = sizeof(z80_settable_names) / sizeof(z80_settable_names[0]);
  }
  else
  {
    names = m68k_settable_names;
    name_count = sizeof(m68k_settable_names) / sizeof(m68k_settable_names[0]);
  }

  for (i = 0; i < name_count; i++)
  {
    uint32 value;
    if (json_get_uint(json, names[i], &value) && (req.reg_set_count < (int)(sizeof(req.reg_set_names) / sizeof(req.reg_set_names[0]))))
    {
      strncpy(req.reg_set_names[req.reg_set_count], names[i], sizeof(req.reg_set_names[0]) - 1);
      req.reg_set_values[req.reg_set_count] = value;
      req.reg_set_count++;
    }
  }

  submit_request(&req);
  send_ok(fd, req.reg_json);
}

static void handle_screenshot(int fd, const char *json)
{
  char folder[400];
  char filename[64];
  static int screenshot_counter = 0;
  api_mailbox_t req;
  char body[512];

  if (!json_get_string(json, "folder", folder, sizeof(folder)) || (folder[0] == '\0'))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'folder' field");
    return;
  }

  snprintf(filename, sizeof(filename), "screenshot_%ld_%03d.bmp", (long)time(NULL), screenshot_counter++ % 1000);

  memset(&req, 0, sizeof(req));
  req.type = API_REQ_SCREENSHOT;
  snprintf(req.path, sizeof(req.path), "%s/%s", folder, filename);

  submit_request(&req);

  if (req.result != 0)
  {
    send_error(fd, 500, "screenshot_failed", "Failed to save screenshot (check that the folder exists and is writable)");
    return;
  }

  snprintf(body, sizeof(body), "{\"ok\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d}",
           req.path, req.width, req.height);
  send_ok(fd, body);
}

static void handle_state_save(int fd, const char *json)
{
  char path[sizeof(((api_mailbox_t *)0)->path)];
  api_mailbox_t req;
  char body[600];

  if (!json_get_string(json, "path", path, sizeof(path)) || (path[0] == '\0'))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'path' field");
    return;
  }

  memset(&req, 0, sizeof(req));
  req.type = API_REQ_STATE_SAVE;
  strncpy(req.path, path, sizeof(req.path) - 1);

  submit_request(&req);

  if (req.result != 0)
  {
    send_error(fd, 500, "state_save_failed", "Failed to write savestate file");
    return;
  }

  snprintf(body, sizeof(body), "{\"ok\":true,\"path\":\"%s\",\"bytes\":%u}", req.path, req.state_bytes);
  send_ok(fd, body);
}

static void handle_state_load(int fd, const char *json)
{
  char path[sizeof(((api_mailbox_t *)0)->path)];
  api_mailbox_t req;
  char body[600];

  if (!json_get_string(json, "path", path, sizeof(path)) || (path[0] == '\0'))
  {
    send_error(fd, 400, "bad_json", "Missing or invalid 'path' field");
    return;
  }

  memset(&req, 0, sizeof(req));
  req.type = API_REQ_STATE_LOAD;
  strncpy(req.path, path, sizeof(req.path) - 1);

  submit_request(&req);

  if (req.result != 0)
  {
    send_error(fd, 500, "state_load_failed", "Failed to load savestate file (missing, unreadable, or not a valid savestate)");
    return;
  }

  snprintf(body, sizeof(body), "{\"ok\":true,\"path\":\"%s\"}", req.path);
  send_ok(fd, body);
}

static void handle_simple_control(int fd, api_request_type_t type)
{
  api_mailbox_t req;

  memset(&req, 0, sizeof(req));
  req.type = type;
  submit_request(&req);

  if (type == API_REQ_FRAME)
  {
    send_ok(fd, "{\"ok\":true,\"frames_advanced\":1}");
  }
  else
  {
    send_ok(fd, "{\"ok\":true}");
  }
}

/* Reads the full request (headers + body) off 'fd' into a heap buffer.
   Returns a malloc'd, NUL-terminated buffer the caller must free(), or NULL
   on error / oversized request. *body_out points into the same buffer. */
static char *read_request(int fd, char **body_out)
{
  char *buf = (char *)malloc(API_HEADER_BUF_SIZE);
  size_t used = 0;
  char *header_end = NULL;
  long content_length = 0;
  const char *cl_ptr;

  if (!buf) return NULL;

  /* read headers */
  while (used + 1 < API_HEADER_BUF_SIZE)
  {
    ssize_t n = recv(fd, buf + used, API_HEADER_BUF_SIZE - 1 - used, 0);
    if (n <= 0)
    {
      free(buf);
      return NULL;
    }
    used += (size_t)n;
    buf[used] = '\0';

    header_end = strstr(buf, "\r\n\r\n");
    if (header_end) break;
  }

  if (!header_end)
  {
    free(buf);
    return NULL;
  }

  header_end += 4;

  /* find Content-Length (case-insensitive) */
  cl_ptr = buf;
  content_length = 0;
  while ((cl_ptr = strcasestr(cl_ptr, "Content-Length:")) != NULL)
  {
    if (cl_ptr < header_end)
    {
      content_length = strtol(cl_ptr + 16, NULL, 10);
    }
    cl_ptr += 16;
  }

  if (content_length < 0) content_length = 0;
  if (content_length > API_MAX_BODY_SIZE)
  {
    free(buf);
    return NULL;
  }

  {
    size_t header_len = (size_t)(header_end - buf);
    size_t total_needed = header_len + (size_t)content_length;
    size_t body_have = used - header_len;

    if (total_needed + 1 > API_HEADER_BUF_SIZE)
    {
      char *grown = (char *)realloc(buf, total_needed + 1);
      if (!grown)
      {
        free(buf);
        return NULL;
      }
      buf = grown;
      header_end = buf + header_len;
    }

    while (body_have < (size_t)content_length)
    {
      ssize_t n = recv(fd, buf + header_len + body_have, (size_t)content_length - body_have, 0);
      if (n <= 0)
      {
        free(buf);
        return NULL;
      }
      body_have += (size_t)n;
    }

    buf[header_len + (size_t)content_length] = '\0';
    *body_out = header_end;
    return buf;
  }
}

static void handle_connection(int fd)
{
  char *body = NULL;
  char *request = read_request(fd, &body);
  char method[8];
  char path[256];

  if (!request)
  {
    send_error(fd, 400, "bad_json", "Malformed or oversized HTTP request");
    return;
  }

  if (sscanf(request, "%7s %255s", method, path) != 2)
  {
    send_error(fd, 400, "bad_json", "Malformed HTTP request line");
    free(request);
    return;
  }

  if (!strcmp(method, "GET") && !strcmp(path, "/health"))
  {
    handle_health(fd);
  }
  else if (!strcmp(method, "GET") && !strcmp(path, "/domains"))
  {
    handle_domains(fd);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/peek"))
  {
    handle_peek(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/search"))
  {
    handle_search(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/snapshot"))
  {
    handle_snapshot(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/diff"))
  {
    handle_diff(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/input"))
  {
    handle_input(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/registers"))
  {
    handle_registers(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/screenshot"))
  {
    handle_screenshot(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/state/save"))
  {
    handle_state_save(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/state/load"))
  {
    handle_state_load(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/poke"))
  {
    handle_poke(fd, body);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/pause"))
  {
    handle_simple_control(fd, API_REQ_PAUSE);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/resume"))
  {
    handle_simple_control(fd, API_REQ_RESUME);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/frame"))
  {
    handle_simple_control(fd, API_REQ_FRAME);
  }
  else if (!strcmp(method, "POST") && !strcmp(path, "/reset"))
  {
    handle_simple_control(fd, API_REQ_RESET);
  }
  else
  {
    send_error(fd, 404, "unknown_endpoint", "Unknown endpoint");
  }

  free(request);
}

static int server_thread_fn(void *data)
{
  (void)data;

  while (server_running)
  {
    struct timeval tv;
    fd_set readset;
    int sel;
    int fd;

    FD_ZERO(&readset);
    FD_SET(listen_fd, &readset);
    tv.tv_sec = 0;
    tv.tv_usec = API_ACCEPT_POLL_MS * 1000;

    sel = select(listen_fd + 1, &readset, NULL, NULL, &tv);
    if (sel <= 0)
    {
      continue;
    }

    fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
    {
      continue;
    }

    handle_connection(fd);
    close(fd);
  }

  return 0;
}

int api_server_start(const char *bind_addr, int port)
{
  struct sockaddr_in addr;
  int opt = 1;

  if (server_running)
  {
    return 0;
  }

  mailbox_mutex = SDL_CreateMutex();
  mailbox_cond = SDL_CreateCond();
  if (!mailbox_mutex || !mailbox_cond)
  {
    fprintf(stderr, "api_server: failed to create synchronization primitives\n");
    return -1;
  }

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0)
  {
    fprintf(stderr, "api_server: socket() failed: %s\n", strerror(errno));
    return -1;
  }

  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1)
  {
    fprintf(stderr, "api_server: invalid bind address '%s'\n", bind_addr);
    close(listen_fd);
    listen_fd = -1;
    return -1;
  }

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    fprintf(stderr, "api_server: bind() to %s:%d failed: %s\n", bind_addr, port, strerror(errno));
    close(listen_fd);
    listen_fd = -1;
    return -1;
  }

  if (listen(listen_fd, API_SERVER_BACKLOG) < 0)
  {
    fprintf(stderr, "api_server: listen() failed: %s\n", strerror(errno));
    close(listen_fd);
    listen_fd = -1;
    return -1;
  }

  if (strcmp(bind_addr, "127.0.0.1") && strcmp(bind_addr, "localhost"))
  {
    fprintf(stderr, "api_server: WARNING binding to non-localhost address '%s' -- the JSON memory API will be reachable from the network\n", bind_addr);
  }

  server_running = 1;
  server_thread = SDL_CreateThread(server_thread_fn, "api_server", NULL);
  if (!server_thread)
  {
    fprintf(stderr, "api_server: failed to start server thread\n");
    server_running = 0;
    close(listen_fd);
    listen_fd = -1;
    return -1;
  }

  printf("Peek/poke JSON API listening on http://%s:%d\n", bind_addr, port);
  return 0;
}

void api_server_stop(void)
{
  if (!server_running)
  {
    return;
  }

  server_running = 0;

  if (server_thread)
  {
    SDL_WaitThread(server_thread, NULL);
    server_thread = NULL;
  }

  if (listen_fd >= 0)
  {
    close(listen_fd);
    listen_fd = -1;
  }

  if (mailbox_mutex)
  {
    SDL_DestroyMutex(mailbox_mutex);
    mailbox_mutex = NULL;
  }
  if (mailbox_cond)
  {
    SDL_DestroyCond(mailbox_cond);
    mailbox_cond = NULL;
  }
}
