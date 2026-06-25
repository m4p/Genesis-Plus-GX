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
