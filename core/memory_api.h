/***************************************************************************************
 *  Genesis Plus GX
 *
 *  Memory access API used by external tooling (e.g. the SDL2 frontend's JSON
 *  peek/poke server) to inspect and modify emulated memory in a safe,
 *  bounds-checked way without exposing raw internal pointers.
 *
 ****************************************************************************************/

#ifndef _MEMORY_API_H_
#define _MEMORY_API_H_

#include "types.h"

/* maximum number of bytes that can be transferred in a single read/write request */
#define MEMORY_API_MAX_TRANSFER 0x10000

typedef enum
{
  MEM_DOMAIN_MAIN_68K_RAM = 0,
  MEM_DOMAIN_Z80_RAM,
  MEM_DOMAIN_VRAM,
  MEM_DOMAIN_CRAM,
  MEM_DOMAIN_VSRAM,
  MEM_DOMAIN_SCD_PRG_RAM,
  MEM_DOMAIN_SCD_WORD_RAM,
  MEM_DOMAIN_SCD_BRAM,
  MEM_DOMAIN_COUNT
} memory_domain_id_t;

typedef struct
{
  memory_domain_id_t id;
  const char *name;
  uint32 size;
  int readable;
  int writable;
} memory_domain_info_t;

enum
{
  MEMORY_API_OK                     =  0,
  MEMORY_API_ERR_UNKNOWN_DOMAIN     = -1, /* domain id/name does not exist */
  MEMORY_API_ERR_UNSUPPORTED_DOMAIN = -2, /* domain exists but is not available right now (e.g. Sega CD domain while running cartridge mode) */
  MEMORY_API_ERR_INVALID_RANGE      = -3, /* address + length exceeds domain size */
  MEMORY_API_ERR_INVALID_LENGTH     = -4, /* length is zero or exceeds MEMORY_API_MAX_TRANSFER */
  MEMORY_API_ERR_NOT_READABLE       = -5,
  MEMORY_API_ERR_NOT_WRITABLE       = -6
};

/* Returns the static table of all known memory domains and its length.
   Domains for hardware that is not currently active (e.g. Sega CD domains
   while running a plain cartridge) are still listed, but reads/writes to
   them will fail with MEMORY_API_ERR_UNSUPPORTED_DOMAIN. */
int memory_api_list_domains(const memory_domain_info_t **domains, int *count);

/* Looks up a domain by its stable string name (e.g. "main_68k_ram").
   Returns MEMORY_API_OK and fills *out_id on success. */
int memory_api_find_domain(const char *name, memory_domain_id_t *out_id);

/* Reads 'length' bytes from 'domain' starting at 'address' into 'out'.
   Returns MEMORY_API_OK on success, or a negative MEMORY_API_ERR_* code. */
int memory_api_read(memory_domain_id_t domain, uint32 address, uint8 *out, uint32 length);

/* Writes 'length' bytes from 'data' into 'domain' starting at 'address'.
   Returns MEMORY_API_OK on success, or a negative MEMORY_API_ERR_* code. */
int memory_api_write(memory_domain_id_t domain, uint32 address, const uint8 *data, uint32 length);

#endif /* _MEMORY_API_H_ */
