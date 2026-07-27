# zephyr_wamr_runtime — Firmware WAMR multi-transport

Firmware Zephyr générique qui héberge un runtime WebAssembly (WAMR) et
transmet les métriques du module WASM via **Wi-Fi** ou **Bluetooth LE**, le
transport étant choisi à la compilation. Le Wi-Fi n'est **pas** une
dépendance obligatoire : le firmware compile sur n'importe quelle carte, y
compris sans Wi-Fi.

## Compilation

Le transport se choisit par un overlay de configuration :

```bash
# Wi-Fi (Heltec, ESP32-C6...)
west build -p always -b <carte> . -- -DEXTRA_CONF_FILE=conf/wifi.conf

# Bluetooth LE (NUCLEO-WB55RG...)
west build -p always -b <carte> . -- -DEXTRA_CONF_FILE=conf/ble.conf
```

Sans overlay de transport, le build s'arrête avec un message explicite.
L'architecture WAMR (`XTENSA`, `RISCV32_ILP32`, `THUMB`...) est **détectée
automatiquement** d'après la carte.

## Structure

```
src/main.c            point d'entrée, upload UART, exécution WAMR
src/host_api.{c,h}    couche hôte : métriques brutes + identité (20 symboles)
src/transport.h       contrat de transport abstrait
src/transport_wifi.c  backend Wi-Fi/TCP   (compilé si CONFIG_WIFI)
src/transport_ble.c   backend BLE/NUS     (compilé si CONFIG_BT)
conf/wifi.conf        overlay de sélection Wi-Fi
conf/ble.conf         overlay de sélection BLE
boards/*.conf         identité par carte
optional-overlays/    overlays facultatifs (console USB ESP32-C6)
Kconfig, prj.conf     configuration (prj.conf neutre : pas de transport imposé)
CMakeLists.txt        détection archi + sélection transport
```

## Documentation

Voir le dossier `docs/` à la racine : `PRISE_EN_MAIN.md`, `CONCEPTION.md`,
`RESEAU_BLE.md`, `NUCLEO_WB55RG.md`, `ESP32C6_PORTS.md`.

## Prérequis

- Runtime WAMR cloné à côté (`../wasm-micro-runtime`).
- Cartes Espressif : `west blobs fetch hal_espressif` (une fois par workspace).
- NUCLEO-WB55RG : coprocesseur radio « HCI Only » flashé au préalable
  (voir `docs/NUCLEO_WB55RG.md`).
