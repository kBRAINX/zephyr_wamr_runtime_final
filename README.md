# zephyr_wamr_runtime — Firmware WAMR multi-transport

Firmware Zephyr générique qui héberge un runtime WebAssembly (WAMR) et
transmet les métriques du module WASM via **Wi-Fi** ou **Bluetooth LE**, le
transport étant choisi à la compilation.

## Prérequis

Installer zephyr

```bash
# Environnement Zephyr (si pas déjà fait)
source ~/zephyrproject/.venv/bin/activate
source ~/zephyrproject/zephyr/zephyr-env.sh
```

## Récupération des sources

```bash
cd ~/zephyrproject

# Runtime WAMR
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git

# Ce projet
git clone https://github.com/kBRAINX/zephyr_wamr_runtime_final.git

# Structure attendue
zephyrproject/
├── zephyr/
├── modules/
├── zephyr_wamr_runtime_final/
└── wasm-micro-runtime/

cd zephyr_wamr_runtime_final
```

## Trouver la cible de build exacte pour VOTRE carte

Le nom exact d'une cible de build (« board target ») dépend de la version
de Zephyr installée, en particulier pour les cartes à plusieurs cœurs ou
plusieurs SoC (comme l'ESP32-C6, qui a un cœur haute performance et un
cœur basse consommation). **Ne recopiez pas une cible trouvée sur internet
sans la vérifier** : interrogez toujours votre propre installation.

```bash
west boards | grep -i heltec      # -> heltec_wifi_lora32_v3/esp32s3/procpu
west boards | grep -i esp32c6     # -> esp32c6_devkitc/esp32c6/hpcore (versions recentes)
                                   #    ou esp32c6_devkitc (versions plus anciennes)
```

Si `west boards` ne liste rien pour votre carte, elle n'est pas encore
supportée par votre version de Zephyr : consultez la
[liste officielle des cartes Espressif](https://docs.zephyrproject.org/latest/boards/espressif/index.html).

## Arborescence du projet

```text

zephyr_wamr_runtime_final/
├── src/
│   ├── main.c
│   │   └── Point d’entrée : réception et exécution du firmware WASM avec WAMR.
│   ├── host_api.c
│   │   └── Implémentation de l’API hôte : métriques et identité de l’équipement.
│   ├── host_api.h
│   │   └── Déclaration de l’API accessible depuis le module WASM.
│   ├── transport.h
│   │   └── Interface abstraite du transport.
│   ├── transport_wifi.c
│   │   └── Transport Wi-Fi/TCP, activé avec CONFIG_WIFI.
│   └── transport_ble.c
│       └── Transport BLE/NUS, activé avec CONFIG_BT.
│
├── conf/
│   ├── wifi.conf
│   │   └── Configuration du transport Wi-Fi.
│   └── ble.conf
│       └── Configuration du transport BLE.
│
├── boards/
│   └── *.conf
│       └── Configuration spécifique à chaque carte.
│
├── optional-overlays/
│   └── ...
│       └── Configurations optionnelles, notamment pour la console USB.
│
├── Kconfig
│   └── Définit les options de configuration du projet.
│
├── prj.conf
│   └── Configuration générale, sans transport imposé.
│
└── CMakeLists.txt
    └── Gère la compilation, l’architecture et la sélection du transport.
```

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

## Rappels

- Runtime WAMR cloné à côté (`../wasm-micro-runtime`).
- Cartes Espressif : `west blobs fetch hal_espressif` (une fois par workspace).
- NUCLEO-WB55RG : coprocesseur radio « HCI Only » flashé au préalable
  (voir `docs/NUCLEO_WB55RG.md`).
