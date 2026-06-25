#!/usr/bin/env node

/*
 * MCP server that proxies the Genesis Plus GX SDL2 peek/poke JSON API
 * (sdl/api_server.c) as MCP tools. The emulator must already be running
 * with --api-port/--api-bind enabled.
 */

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const API_BASE = process.env.GPGX_API_BASE ?? "http://127.0.0.1:8765";

async function apiGet(path) {
  const res = await fetch(`${API_BASE}${path}`);
  return { status: res.status, body: await res.json() };
}

async function apiPost(path, payload) {
  const res = await fetch(`${API_BASE}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload ?? {}),
  });
  return { status: res.status, body: await res.json() };
}

function toolResult(result) {
  return {
    content: [{ type: "text", text: JSON.stringify(result.body, null, 2) }],
    isError: result.status >= 400 || result.body?.ok === false,
  };
}

const server = new McpServer({
  name: "genesis-plus-gx-memory",
  version: "1.0.0",
});

server.registerTool(
  "health",
  {
    title: "Health check",
    description: "Check whether the Genesis Plus GX peek/poke API is reachable.",
    inputSchema: {},
  },
  async () => toolResult(await apiGet("/health"))
);

server.registerTool(
  "list_domains",
  {
    title: "List memory domains",
    description:
      "List the memory domains exposed by the emulator (e.g. main_68k_ram, z80_ram, vram, cram, vsram, and Sega CD scd_prg_ram/scd_word_ram/scd_bram when a Mega-CD title is loaded), with their size and read/write capability.",
    inputSchema: {},
  },
  async () => toolResult(await apiGet("/domains"))
);

server.registerTool(
  "peek",
  {
    title: "Read emulated memory",
    description:
      "Read a bounds-checked range of bytes from an emulated memory domain. Returns the data hex- or base64-encoded.",
    inputSchema: {
      domain: z.string().describe("Domain name, e.g. main_68k_ram, z80_ram, vram, cram, vsram, scd_prg_ram, scd_word_ram, scd_bram"),
      address: z.number().int().nonnegative().describe("Byte offset within the domain"),
      length: z.number().int().positive().describe("Number of bytes to read"),
      encoding: z.enum(["hex", "base64"]).default("hex"),
    },
  },
  async ({ domain, address, length, encoding }) =>
    toolResult(await apiPost("/peek", { domain, address, length, encoding }))
);

server.registerTool(
  "poke",
  {
    title: "Write emulated memory",
    description:
      "Write a bounds-checked range of bytes into an emulated memory domain. Data must be hex- or base64-encoded matching the given encoding.",
    inputSchema: {
      domain: z.string().describe("Domain name, e.g. main_68k_ram, z80_ram, vram, cram, vsram, scd_prg_ram, scd_word_ram, scd_bram"),
      address: z.number().int().nonnegative().describe("Byte offset within the domain"),
      data: z.string().describe("Bytes to write, encoded per 'encoding'"),
      encoding: z.enum(["hex", "base64"]).default("hex"),
    },
  },
  async ({ domain, address, data, encoding }) =>
    toolResult(await apiPost("/poke", { domain, address, data, encoding }))
);

server.registerTool(
  "search",
  {
    title: "Search emulated memory",
    description:
      "Search a memory domain for every occurrence of a byte pattern, returning matching offsets. Optionally restrict to a [start, end) byte range; end defaults to the domain's size.",
    inputSchema: {
      domain: z.string().describe("Domain name, e.g. main_68k_ram, z80_ram, vram, cram, vsram, scd_prg_ram, scd_word_ram, scd_bram"),
      pattern: z.string().describe("Byte pattern to search for, encoded per 'encoding'"),
      encoding: z.enum(["hex", "base64"]).default("hex"),
      start: z.number().int().nonnegative().optional().describe("Start offset of the search range (default 0)"),
      end: z.number().int().nonnegative().optional().describe("End offset of the search range, exclusive (default: domain size)"),
      max_results: z.number().int().positive().max(256).optional().describe("Maximum number of offsets to return (default 64, capped at 256)"),
    },
  },
  async ({ domain, pattern, encoding, start, end, max_results }) =>
    toolResult(await apiPost("/search", { domain, pattern, encoding, start, end, max_results }))
);

const BUTTON_NAMES = ["up", "down", "left", "right", "a", "b", "c", "x", "y", "z", "start", "mode"];

server.registerTool(
  "press_buttons",
  {
    title: "Press/release gamepad buttons",
    description:
      "Hold and/or release buttons on player 1's gamepad. Supports the full Mega Drive / Mega CD 6-button pad: up, down, left, right, a, b, c, x, y, z, start, mode. Buttons stay held across frames until released; call again with 'release' to let go, or pair with frame_step while paused for precise single-frame input.",
    inputSchema: {
      press: z.array(z.enum(BUTTON_NAMES)).optional().describe("Buttons to start holding"),
      release: z.array(z.enum(BUTTON_NAMES)).optional().describe("Buttons to stop holding"),
    },
  },
  async ({ press, release }) => toolResult(await apiPost("/input", { press, release }))
);

server.registerTool(
  "pause",
  {
    title: "Pause emulation",
    description: "Pause the emulator's main loop. Memory peek/poke and frame-step remain usable while paused.",
    inputSchema: {},
  },
  async () => toolResult(await apiPost("/pause"))
);

server.registerTool(
  "resume",
  {
    title: "Resume emulation",
    description: "Resume the emulator's main loop after a pause.",
    inputSchema: {},
  },
  async () => toolResult(await apiPost("/resume"))
);

server.registerTool(
  "frame_step",
  {
    title: "Step one frame",
    description: "Advance emulation by exactly one frame. Most useful while paused.",
    inputSchema: {},
  },
  async () => toolResult(await apiPost("/frame"))
);

server.registerTool(
  "registers",
  {
    title: "Read/write CPU registers",
    description:
      "Read the registers of a CPU (m68k = main 68000, s68k = Mega CD SubCPU, z80 = sound CPU), optionally writing one or more registers first. For m68k/s68k: d0-d7, a0-a7, pc (writable), plus sr/usp/ssp/halted (read-only). For z80: pc, sp, af, bc, de, hl, ix, iy (writable), plus af2/bc2/de2/hl2/halted (read-only).",
    inputSchema: {
      cpu: z.enum(["m68k", "s68k", "z80"]),
      set: z.record(z.string(), z.number().int()).optional().describe("Register name -> new value, e.g. {\"pc\": 4096, \"d0\": 0}"),
    },
  },
  async ({ cpu, set }) => toolResult(await apiPost("/registers", { cpu, ...set }))
);

server.registerTool(
  "screenshot",
  {
    title: "Take a screenshot",
    description:
      "Save a BMP screenshot of the currently displayed emulator frame into the given folder (which must already exist). The filename is generated automatically; the returned path points to the saved file.",
    inputSchema: {
      folder: z.string().describe("Absolute or relative path to an existing, writable folder"),
    },
  },
  async ({ folder }) => toolResult(await apiPost("/screenshot", { folder }))
);

server.registerTool(
  "save_state",
  {
    title: "Save state",
    description: "Save the emulator's full state to the given file path (creates or overwrites it).",
    inputSchema: {
      path: z.string().describe("File path to write the savestate to"),
    },
  },
  async ({ path }) => toolResult(await apiPost("/state/save", { path }))
);

server.registerTool(
  "load_state",
  {
    title: "Load state",
    description: "Restore the emulator's full state from a previously saved savestate file.",
    inputSchema: {
      path: z.string().describe("File path to a savestate previously written by save_state"),
    },
  },
  async ({ path }) => toolResult(await apiPost("/state/load", { path }))
);

server.registerTool(
  "reset",
  {
    title: "Reset emulator",
    description: "Reset the emulated system (equivalent to pressing the console's reset button).",
    inputSchema: {},
  },
  async () => toolResult(await apiPost("/reset"))
);

const transport = new StdioServerTransport();
await server.connect(transport);
