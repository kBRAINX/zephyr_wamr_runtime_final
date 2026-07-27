cmake_minimum_required(VERSION 3.20.0)

# ============================================================================
# CMakeLists.txt — Firmware WAMR multi-transport
#
# Deux responsabilites :
#  1. Detecter automatiquement l'architecture cible de WAMR d'apres la carte
#     (aucune valeur figee : fonctionne sur Xtensa, RISC-V, ARM...).
#  2. Compiler le bon backend de transport (Wi-Fi ou BLE) selon la config,
#     et refuser explicitement une config sans transport.
# ============================================================================

set(WAMR_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../wasm-micro-runtime)

# --- Reglages WAMR indispensables sur cible contrainte ---
set(WAMR_BUILD_PLATFORM "zephyr")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_THREAD_MGR 0)
# Memoire lineaire non "shrunk" : sinon ~1 Mo/module, fatal ici.
set(WAMR_BUILD_SHRUNK_MEMORY 0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wamr_multitransport)

# ----------------------------------------------------------------------------
# 1. DETECTION AUTOMATIQUE DE L'ARCHITECTURE WAMR
#
# Les symboles CONFIG_* sont disponibles APRES find_package(Zephyr).
# On respecte un choix explicite de l'utilisateur s'il en fournit un.
# ----------------------------------------------------------------------------
if(NOT DEFINED WAMR_BUILD_TARGET)
  if(CONFIG_XTENSA)
    set(_WAMR_TARGET "XTENSA")
  elseif(CONFIG_RISCV)
    if(CONFIG_64BIT)
      set(_WAMR_TARGET "RISCV64")
    elseif(CONFIG_FPU)
      set(_WAMR_TARGET "RISCV32_ILP32F")
    else()
      set(_WAMR_TARGET "RISCV32_ILP32")
    endif()
  elseif(CONFIG_ARM64)
    set(_WAMR_TARGET "AARCH64")
  elseif(CONFIG_ARM)
    set(_WAMR_TARGET "THUMB")
  elseif(CONFIG_X86_64)
    set(_WAMR_TARGET "X86_64")
  elseif(CONFIG_X86)
    set(_WAMR_TARGET "X86_32")
  else()
    message(FATAL_ERROR
      "Architecture non reconnue pour WAMR. Definissez-la explicitement : "
      "west build ... -- -DWAMR_BUILD_TARGET=<XTENSA|RISCV32_ILP32|THUMB|...>")
  endif()
  set(WAMR_BUILD_TARGET ${_WAMR_TARGET})
endif()
message(STATUS "WAMR_BUILD_TARGET detecte automatiquement : ${WAMR_BUILD_TARGET}")

# ----------------------------------------------------------------------------
# 2. SELECTION DU BACKEND DE TRANSPORT
#
# Exactement un backend doit etre actif. On le verifie ici, tot, avec un
# message clair, plutot que de laisser echouer l'edition de liens.
# ----------------------------------------------------------------------------
if(CONFIG_WIFI AND CONFIG_BT)
  message(FATAL_ERROR
    "CONFIG_WIFI et CONFIG_BT sont tous deux actifs. Choisissez UN seul "
    "transport (conf/wifi.conf OU conf/ble.conf).")
elseif(CONFIG_WIFI)
  message(STATUS "Transport selectionne : Wi-Fi (TCP)")
elseif(CONFIG_BT)
  message(STATUS "Transport selectionne : Bluetooth LE (NUS)")
else()
  message(FATAL_ERROR
    "Aucun transport selectionne. Ajoutez au build :\n"
    "  -DEXTRA_CONF_FILE=conf/wifi.conf   (Wi-Fi)\n"
    "  ou -DEXTRA_CONF_FILE=conf/ble.conf (Bluetooth LE)")
endif()

# ----------------------------------------------------------------------------
# 3. Runtime WAMR (bibliotheque)
# ----------------------------------------------------------------------------
set(WAMR_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../wasm-micro-runtime)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(vmlib PUBLIC
  ${WAMR_ROOT_DIR}/core/iwasm/include
  ${WAMR_ROOT_DIR}/core/shared/utils
)
target_link_libraries(vmlib PUBLIC zephyr_interface)

# IMPORTANT — dependance sur les en-tetes generes de Zephyr.
#
# Depuis Zephyr ~4.4, kernel.h inclut <zephyr/heap_constants.h>, un en-tete
# GENERE pendant le build (il n'existe pas dans l'arbre source). Une
# bibliotheque creee via add_library() comme vmlib est liee a
# zephyr_interface pour les chemins d'include, mais cela n'entraine PAS
# automatiquement une dependance d'ordonnancement sur la generation de ces
# en-tetes. Resultat : ninja peut compiler WAMR AVANT que heap_constants.h
# soit produit, d'ou l'erreur :
#   fatal error: zephyr/heap_constants.h: No such file or directory
#
# On force donc vmlib a attendre la cible qui genere ces en-tetes. Selon la
# version de Zephyr, cette cible s'appelle zephyr_generated_headers (recent)
# ou offsets_h (plus ancien). On ajoute celle(s) qui existe(nt).
if(TARGET zephyr_generated_headers)
  add_dependencies(vmlib zephyr_generated_headers)
endif()
if(TARGET offsets_h)
  add_dependencies(vmlib offsets_h)
endif()

# ----------------------------------------------------------------------------
# 4. Sources de l'application
#
# Les deux backends sont listes, mais chacun est neutralise en interne par ses
# propres gardes #if : seul le backend correspondant au transport actif produit
# du code. Les lister tous les deux inconditionnellement evite un piege CMake
# (un fichier compile "a vide" ne coute rien).
# ----------------------------------------------------------------------------
target_sources(app PRIVATE
  src/main.c
  src/host_api.c
  src/transport_wifi.c
  src/transport_ble.c
)

target_include_directories(app PRIVATE
  ${WAMR_ROOT_DIR}/core/iwasm/include
)
target_link_libraries(app PRIVATE vmlib)