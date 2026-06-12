# Guide de compilation de Zephyr + WAMR et deploiement sur un equipement IoT

## Préréquis

- Installer `Zephyr Os`

## Compilation du projet et deploiement sur un equipement IoT

```bash
# Activer l'environnement Zephyr
source ~/zephyrproject/.venv/bin/activate
source ~/zephyrproject/zephyr/zephyr-env.sh

# Se placer dans le projet zephyr
cd ~/zephyrproject

# Cloner le depot wamr
git clone https://github.com/bytecodealliance/wasm-micro-runtime.git

# cloner le repository
git clone https://github.com/BrayanneImt/zephyr_wamr_runtime_final.git

# Structure attendue
zephyrproject/
├── zephyr/
├── modules/
├── zephyr_wamr_runtime_final/
└── wasm-micro-runtime/

# Acceder au repertoire du projet
cd zephyr_wamr_runtime

# Compiler pour l'equipement IoT (heltec_wifi_lora32_v3/esp32s3/procpu est le support de notre equipement iot sur zephyr)
west build -p always -b heltec_wifi_lora32_v3/esp32s3/procpu .

# Flasher (brancher le Heltec en USB d'abord)
west flash

# Surveiller les logs série
west espressif monitor
```
