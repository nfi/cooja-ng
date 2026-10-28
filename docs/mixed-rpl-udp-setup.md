# A mixed-board RPL-UDP simulation in Cooja-NG

For someone who knows Contiki-NG but not Cooja-NG: `examples/rpl-udp` with a
Tmote Sky as the server (RPL root) and Sky, Z1, Zolertia Firefly and nRF52840
clients.

## 1. Build the images in Contiki-NG

```sh
cd contiki-ng/examples/rpl-udp
make TARGET=sky udp-server udp-client          # udp-server.sky, udp-client.sky
make TARGET=z1 udp-client                      # udp-client.z1
make TARGET=zoul BOARD=firefly udp-client      # udp-client.zoul
make TARGET=nrf BOARD=nrf52840/dk udp-client   # udp-client.nrf
```

## 2. Write the setup file

`cngsim new rpl-udp --board sky -n 4 -o mixed.yaml` writes a commented
template with two Sky mote types. `new` takes one board only; the mixed
part is edited in by hand:

```yaml
version: 2
title: "rpl-udp: Sky root, mixed clients"
timeout_ms: 120000
seed: 42
startup_delay_ms: 1000

medium:
  type: udgm              # unit-disk graph, as in Cooja: in range = received
  tx_range: 45            # metres
  interference_range: 90

mote_types:
  - { name: server,  firmware: ../contiki-ng/examples/rpl-udp/udp-server.sky }
  - { name: sky,     firmware: ../contiki-ng/examples/rpl-udp/udp-client.sky }
  - { name: z1,      firmware: ../contiki-ng/examples/rpl-udp/udp-client.z1 }
  - { name: firefly, firmware: ../contiki-ng/examples/rpl-udp/udp-client.zoul, board: zoul-firefly }
  - { name: nrf,     firmware: ../contiki-ng/examples/rpl-udp/udp-client.nrf,  board: nrf52840-dk }

nodes:
  - { type: server,  id: 1, x: 0,  y: 0 }
  - { type: sky,     id: 2, x: 30, y: 0 }
  - { type: z1,      id: 3, x: 0,  y: 30 }
  - { type: firefly, id: 4, x: 30, y: 30 }
  - { type: nrf,     id: 5, x: 60, y: 0 }

test:
  validators:
    - { pattern: "Received request",  node: 1, min_count: 4 }
    - { pattern: "Received response", node: 5, min_count: 1 }
```

Paths are relative to the YAML file. Each node's node id and link address
are patched from `id` at boot, per architecture, so the four client stacks
form one DAG under the Sky root.

## 3. Check, run, watch

```sh
cngsim check mixed.yaml      # loads and validates without running
cngsim run mixed.yaml        # consoles, checks, verdict; exit 0 or 1
cngsim run mixed.yaml --ui   # browser: DAG arrows, consoles, drag nodes, Save
```

## How the board is chosen

The firmware file's extension selects the board, from a static table
(`cngsim boards` lists it with the Contiki-NG `TARGET`/`BOARD` for each).
`.sky`, `.z1`, `.cc2538dk` and `.cooja` are what Contiki-NG writes.
Contiki-NG's `.nrf`, `.simplelink` and `.zoul` are not in the table, since
they cover several boards each, so those images need `board: <name>` on the
mote type, or `--board <name>` on the `cngsim run` command line. An unknown
extension without `board:` is an error; a `board:` that contradicts a known
extension is an error too. Nothing is read from the image itself beyond the
ELF machine type.

## Pitfalls

- **Two of the four images need `board:`.** The error appears only at
  `check` or `run`.
- **Firefly radio band.** A stock `BOARD=firefly` build uses the CC2538's
  2.4 GHz radio, which a Sky DAG needs. The Firefly images bundled with
  Cooja-NG are sub-GHz CC1200 builds; copied from an example, the Firefly
  sits on another band and never joins.
- **nRF52840 is generic.** `.nrf` can be any Nordic SoC, so
  `board: nrf52840-dk` is mandatory; the dongle is `nrf52840-dongle`.
  (`TARGET=nrf52840`, which wrote `.nrf52840`, is being removed from
  Contiki-NG.)
- **Range.** Every client needs a path to node 1 within `tx_range`. The
  `count` + `layout` shorthand places many nodes of one type; a mixed
  network is placed by hand or in the browser, then saved.
- **Existing Cooja `.csc`:** `cngsim convert sim.csc -o sim.yaml` instead
  of the template.
