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
template with two Sky mote types. `new` takes one board, from those its
template has images for (the table under "What `cngsim new` can write").
The mixed part, and any path of your own, is edited in by hand:

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

A relative path is looked up next to the YAML file, then from the
directory `cngsim` runs in; an absolute or `~/` path is used as written,
so a setup meant to be moved on its own names its images that way. Each
node's node id and link address are patched from `id` at boot, per
architecture, so the four client stacks form one DAG under the Sky root.

## 3. Check, run, watch

```sh
cngsim check mixed.yaml      # loads and validates without running
cngsim run mixed.yaml        # consoles, checks, verdict; exit 0 or 1
cngsim run mixed.yaml --ui   # browser: DAG arrows, consoles, drag nodes, Save
```

## Your own application

The simulator knows nothing about rpl-udp. A mote type is an image and a
board; node id and link address are patched from `id` for any Contiki-NG
image, since the symbols that takes are in every one. For another
server/client application, swap the step 1 build lines and the `firmware:`
paths for yours. `new` cannot name your images: write the template for a
board whose bundled images exist (`--board nrf52840-dk`), then edit the
two paths, adding `board:` where the extension does not name the board.
The validators match console text: replace the two
patterns with lines your server and clients print, or leave `test:` out to
just watch the consoles (`run` then gives no verdict). The 16-node cap
`cngsim new rpl-udp` enforces is the bundled udp-server's routing table
(`NETSTACK_MAX_ROUTE_ENTRIES`), not a simulator limit.

## What `cngsim new` can write

`new` fills a template with the images bundled under `firmware/<board>/`,
so a template exists for a board only when those images do. This is the
state of this release; a failed `new` prints the same list.

- **rpl-udp**: node 1 runs `udp-server` as the RPL root, the others
  `udp-client`. 2 to 16 nodes. The check waits for "Received response" on
  the farthest client.
- **nullnet**: every node runs `nullnet-broadcast`, no IP. 2 or more nodes.
  The check waits for "Received".
- **hello**: every node runs `hello-world`, no radio. 1 or more nodes. The
  check waits for "Hello, world".

| Board | rpl-udp | nullnet | hello |
|---|---|---|---|
| sky | yes | yes | yes |
| z1 | yes | – | yes |
| cc2538dk | yes | yes | yes |
| nrf52840-dk | yes | – | yes |
| nrf52840-dongle | yes | – | yes |
| nrf54l15-dk | yes | – | yes |
| nrf54l15-xiao | yes | – | yes |
| cc1352r1-launchpad | yes | yes | – |
| cc1312r7-launchpad | yes | yes | – |
| zoul-firefly | – | – | – |
| fr5969 | – | – | – |

No template covers the Firefly: its bundled images are the sub-GHz CC1200
builds, under other names. The fr5969 ships no images.

## Variation: 32 nodes on three boards, the root chosen from the shell

One application on Zolertia Firefly, nRF52840-DK and nRF52840 Dongle, and
no server image: node 1 becomes the RPL root when the simulation types
`rpl-set-root 1` at its console.

Build it for each board with the shell in it (`MODULES += os/services/shell`).
A root serving 32 nodes keeps a source-routing link per node, so
`project-conf.h` needs `NETSTACK_MAX_ROUTE_ENTRIES` above 32 (the default is
16). Both Nordic builds are named `myapp.nrf`, and the link Contiki-NG puts
next to the sources is to the last one built, so the setup names the
copies under `build/`:

```sh
cd myapp
make TARGET=zoul BOARD=firefly            # build/zoul/firefly/myapp.zoul
make TARGET=nrf BOARD=nrf52840/dk         # build/nrf/nrf52840/dk/myapp.nrf
make TARGET=nrf BOARD=nrf52840/dongle     # build/nrf/nrf52840/dongle/myapp.nrf
```

```yaml
version: 2
title: "myapp: 32 nodes on three boards, root chosen from the shell"
timeout_ms: 300000
seed: 42
startup_delay_ms: 1000

medium: { type: udgm, tx_range: 50, interference_range: 100 }

mote_types:
  - { name: dk,      firmware: ../myapp/build/nrf/nrf52840/dk/myapp.nrf,     board: nrf52840-dk }
  - { name: firefly, firmware: ../myapp/build/zoul/firefly/myapp.zoul,       board: zoul-firefly }
  - { name: dongle,  firmware: ../myapp/build/nrf/nrf52840/dongle/myapp.nrf, board: nrf52840-dongle }

# One entry places many nodes of a type. Ids must not overlap between
# entries; a file holds up to 128 nodes. Three grids stacked 40 m apart:
# every node has a neighbour within tx_range.
nodes:
  - { type: dk,      id: 1,  count: 11, layout: grid, columns: 4, spacing: 40, x: 0, y: 0 }
  - { type: firefly, id: 12, count: 11, layout: grid, columns: 4, spacing: 40, x: 0, y: 120 }
  - { type: dongle,  id: 23, count: 10, layout: grid, columns: 4, spacing: 40, x: 0, y: 240 }

test:
  actions:
    - { at_ms: 2000, type: send, node: 1, data: "rpl-set-root 1\n" }
  steps:
    - { wait: "Setting as DAG root", node: 1, timeout_ms: 5000 }
  validators:
    - { pattern: "<a line your clients print once they reach the root>", min_count: 31 }
```

The `send` action delivers the bytes to the node's console UART receive
path, paced at the baud rate, on every board here (CC2538 UART0, nRF52840
UARTE). At 2 s the latest boot `startup_delay_ms` allows is over. The step
waits for the shell's reply, `Setting as DAG root with prefix ...`, which
shows the command landed.

The same from the keyboard, or as a script ([shell.md](shell.md)):

```sh
cngsim run myapp.yaml --shell            # a prompt; type the cmd line below
cngsim run myapp.yaml --script root.cnsh
```

```
# root.cnsh
wait-until 2s
cmd -e "Setting as DAG root" 1 rpl-set-root 1   # type it, wait for node 1's prompt
expect -n 31 any "<the clients' line>" 240s
pass
```

`cngsim new` writes one board, so the three mote types and three node
entries are typed by hand or saved from the browser.

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
