/***************************************************************************************
 *  Genesis Plus GX
 *
 *  Memory access API implementation. See memory_api.h for usage.
 *
 *  This module never hands out raw pointers to callers outside the core: all
 *  access goes through memory_api_read()/memory_api_write() which copy into
 *  caller-supplied buffers after bounds-checking.
 *
 ****************************************************************************************/

#include <string.h>
#include "memory_api.h"
#include "osd.h"
#include "macros.h"
#include "genesis.h"
#include "vdp_ctrl.h"
#include "system.h"

/* Sega CD Word-RAM is exposed as a single linear 256K domain regardless of
   1M/2M mode: in 2M mode it maps directly onto word_ram_2M, in 1M mode the
   two 128K banks are concatenated in bank order (bank 0 then bank 1). This
   is a debugging convenience view, not a cycle-accurate view of what the
   68k/SUB-68k currently see mapped into their address spaces. */
#define SCD_WORD_RAM_SIZE 0x40000

static const memory_domain_info_t domain_table[MEM_DOMAIN_COUNT] =
{
  { MEM_DOMAIN_MAIN_68K_RAM, "main_68k_ram",  0x10000, 1, 1 },
  { MEM_DOMAIN_Z80_RAM,      "z80_ram",       0x2000,  1, 1 },
  { MEM_DOMAIN_VRAM,         "vram",          0x10000, 1, 1 },
  { MEM_DOMAIN_CRAM,         "cram",          0x80,    1, 1 },
  { MEM_DOMAIN_VSRAM,        "vsram",         0x80,    1, 1 },
  { MEM_DOMAIN_SCD_PRG_RAM,  "scd_prg_ram",   0x80000, 1, 1 },
  { MEM_DOMAIN_SCD_WORD_RAM, "scd_word_ram",  SCD_WORD_RAM_SIZE, 1, 1 },
  { MEM_DOMAIN_SCD_BRAM,     "scd_bram",      0x2000,  1, 1 }
};

int memory_api_list_domains(const memory_domain_info_t **domains, int *count)
{
  if (!domains || !count)
  {
    return MEMORY_API_ERR_UNKNOWN_DOMAIN;
  }

  *domains = domain_table;
  *count = MEM_DOMAIN_COUNT;
  return MEMORY_API_OK;
}

int memory_api_find_domain(const char *name, memory_domain_id_t *out_id)
{
  int i;

  if (!name || !out_id)
  {
    return MEMORY_API_ERR_UNKNOWN_DOMAIN;
  }

  for (i = 0; i < MEM_DOMAIN_COUNT; i++)
  {
    if (!strcmp(domain_table[i].name, name))
    {
      *out_id = domain_table[i].id;
      return MEMORY_API_OK;
    }
  }

  return MEMORY_API_ERR_UNKNOWN_DOMAIN;
}

/* Returns 1 if the Sega CD hardware is currently active and its memory
   regions (PRG-RAM, Word-RAM, BRAM) are valid to access. */
static int scd_active(void)
{
  return (system_hw == SYSTEM_MCD);
}

/* Resolves a domain to its backing host pointer and current size.
   Returns MEMORY_API_OK on success, or a negative MEMORY_API_ERR_* code if
   the domain is unknown or not currently available. Never returns a pointer
   to the caller's own response buffer -- this is strictly an internal helper. */
static int resolve_domain(memory_domain_id_t domain, uint8 **base, uint32 *size, int *readable, int *writable)
{
  if ((domain < 0) || (domain >= MEM_DOMAIN_COUNT))
  {
    return MEMORY_API_ERR_UNKNOWN_DOMAIN;
  }

  *readable = domain_table[domain].readable;
  *writable = domain_table[domain].writable;
  *size = domain_table[domain].size;

  switch (domain)
  {
    case MEM_DOMAIN_MAIN_68K_RAM:
      *base = work_ram;
      return MEMORY_API_OK;

    case MEM_DOMAIN_Z80_RAM:
      *base = zram;
      return MEMORY_API_OK;

    case MEM_DOMAIN_VRAM:
      *base = vram;
      return MEMORY_API_OK;

    case MEM_DOMAIN_CRAM:
      *base = cram;
      return MEMORY_API_OK;

    case MEM_DOMAIN_VSRAM:
      *base = vsram;
      return MEMORY_API_OK;

    case MEM_DOMAIN_SCD_PRG_RAM:
      if (!scd_active())
      {
        return MEMORY_API_ERR_UNSUPPORTED_DOMAIN;
      }
      *base = scd.prg_ram;
      return MEMORY_API_OK;

    case MEM_DOMAIN_SCD_WORD_RAM:
      /* handled specially in read/write since it may be backed by two
         separate banks (1M mode) instead of one linear buffer */
      if (!scd_active())
      {
        return MEMORY_API_ERR_UNSUPPORTED_DOMAIN;
      }
      *base = NULL;
      return MEMORY_API_OK;

    case MEM_DOMAIN_SCD_BRAM:
      if (!scd_active())
      {
        return MEMORY_API_ERR_UNSUPPORTED_DOMAIN;
      }
      *base = scd.bram;
      return MEMORY_API_OK;

    default:
      return MEMORY_API_ERR_UNKNOWN_DOMAIN;
  }
}

/* Word-RAM is physically split into two 128K banks in 1M mode. We present a
   single linear 256K view: [0x00000-0x1FFFF] = bank 0, [0x20000-0x3FFFF] =
   bank 1. In 2M mode it is already one linear 256K buffer. */
static void scd_word_ram_copy(uint32 address, uint32 length, uint8 *dst, const uint8 *src, int writing)
{
  int mode_1m = (scd.regs[0x03 >> 1].byte.l & 0x04) ? 1 : 0;

  if (!mode_1m)
  {
    uint8 *bank = scd.word_ram_2M + address;
    if (writing) memcpy(bank, src, length);
    else memcpy(dst, bank, length);
    return;
  }

  while (length > 0)
  {
    int bank_index = (address >> 17) & 1;
    uint32 bank_offset = address & 0x1FFFF;
    uint32 chunk = 0x20000 - bank_offset;
    if (chunk > length)
    {
      chunk = length;
    }

    if (writing)
    {
      memcpy(scd.word_ram[bank_index] + bank_offset, src, chunk);
      src += chunk;
    }
    else
    {
      memcpy(dst, scd.word_ram[bank_index] + bank_offset, chunk);
      dst += chunk;
    }

    address += chunk;
    length -= chunk;
  }
}

int memory_api_read(memory_domain_id_t domain, uint32 address, uint8 *out, uint32 length)
{
  uint8 *base = NULL;
  uint32 size = 0;
  int readable = 0, writable = 0;
  int err;

  if (!out)
  {
    return MEMORY_API_ERR_INVALID_RANGE;
  }

  if ((length == 0) || (length > MEMORY_API_MAX_TRANSFER))
  {
    return MEMORY_API_ERR_INVALID_LENGTH;
  }

  err = resolve_domain(domain, &base, &size, &readable, &writable);
  if (err != MEMORY_API_OK)
  {
    return err;
  }

  if (!readable)
  {
    return MEMORY_API_ERR_NOT_READABLE;
  }

  /* reject overflow and out-of-range accesses */
  if ((address > size) || (length > size - address))
  {
    return MEMORY_API_ERR_INVALID_RANGE;
  }

  if (domain == MEM_DOMAIN_SCD_WORD_RAM)
  {
    scd_word_ram_copy(address, length, out, NULL, 0);
  }
  else
  {
    memcpy(out, base + address, length);
  }

  return MEMORY_API_OK;
}

/* Scratch buffer used to present Sega CD Word-RAM as a linear 256K view for
   searching (see scd_word_ram_copy() above). Only ever touched from the
   main/emulation thread, one request at a time. */
static uint8 word_ram_search_scratch[SCD_WORD_RAM_SIZE];

int memory_api_search(memory_domain_id_t domain, uint32 start, uint32 end,
                       const uint8 *pattern, uint32 pattern_length,
                       uint32 *out_offsets, uint32 max_results, uint32 *out_count)
{
  uint8 *base = NULL;
  uint32 size = 0;
  int readable = 0, writable = 0;
  const uint8 *haystack;
  uint32 count = 0;
  uint32 i;
  int err;

  if (!pattern || !out_offsets || !out_count)
  {
    return MEMORY_API_ERR_INVALID_RANGE;
  }

  if ((pattern_length == 0) || (pattern_length > MEMORY_API_MAX_SEARCH_PATTERN))
  {
    return MEMORY_API_ERR_INVALID_PATTERN;
  }

  err = resolve_domain(domain, &base, &size, &readable, &writable);
  if (err != MEMORY_API_OK)
  {
    return err;
  }

  if (!readable)
  {
    return MEMORY_API_ERR_NOT_READABLE;
  }

  if ((end == 0) || (end > size))
  {
    end = size;
  }

  if (start > end)
  {
    return MEMORY_API_ERR_INVALID_RANGE;
  }

  if (max_results > MEMORY_API_MAX_SEARCH_RESULTS)
  {
    max_results = MEMORY_API_MAX_SEARCH_RESULTS;
  }

  *out_count = 0;

  if (pattern_length > (end - start))
  {
    return MEMORY_API_OK;
  }

  if (domain == MEM_DOMAIN_SCD_WORD_RAM)
  {
    scd_word_ram_copy(0, size, word_ram_search_scratch, NULL, 0);
    haystack = word_ram_search_scratch;
  }
  else
  {
    haystack = base;
  }

  for (i = start; (i + pattern_length <= end) && (count < max_results); i++)
  {
    if (!memcmp(haystack + i, pattern, pattern_length))
    {
      out_offsets[count++] = i;
    }
  }

  *out_count = count;
  return MEMORY_API_OK;
}

int memory_api_write(memory_domain_id_t domain, uint32 address, const uint8 *data, uint32 length)
{
  uint8 *base = NULL;
  uint32 size = 0;
  int readable = 0, writable = 0;
  int err;

  if (!data)
  {
    return MEMORY_API_ERR_INVALID_RANGE;
  }

  if ((length == 0) || (length > MEMORY_API_MAX_TRANSFER))
  {
    return MEMORY_API_ERR_INVALID_LENGTH;
  }

  err = resolve_domain(domain, &base, &size, &readable, &writable);
  if (err != MEMORY_API_OK)
  {
    return err;
  }

  if (!writable)
  {
    return MEMORY_API_ERR_NOT_WRITABLE;
  }

  if ((address > size) || (length > size - address))
  {
    return MEMORY_API_ERR_INVALID_RANGE;
  }

  if (domain == MEM_DOMAIN_SCD_WORD_RAM)
  {
    scd_word_ram_copy(address, length, NULL, data, 1);
  }
  else
  {
    memcpy(base + address, data, length);
  }

  return MEMORY_API_OK;
}
