/*
 * Contrat de mise a jour a chaud (hot-update).
 *
 * Ces fonctions sont DEFINIES dans main.c (qui possede les deux buffers et le
 * compteur) et APPELEES par host_api.c (qui les expose au module WASM sous
 * forme de host functions). Cette indirection garde main.c proprietaire de
 * l'etat de deploiement, et host_api.c simple relais.
 *
 * Modele : point de sortie COOPERATIF. La sonde interroge periodiquement le
 * reseau via host_poll_update ; si un nouveau .wasm est recu et mis en attente
 * (staging), la sonde rend la main proprement (return) et le firmware bascule
 * sur le nouveau module. Jamais deux INSTANCES WASM simultanees : l'ancienne
 * est detruite avant le chargement de la nouvelle.
 *
 * En config BLE (pas de CONFIG_WIFI), deploy_try_stage renvoie toujours 0 :
 * la mise a jour a chaud n'existe pas, on reste sur l'upload UART + reset.
 *
 * Licence : Apache-2.0
 */

#ifndef WAMR_DEPLOY_H
#define WAMR_DEPLOY_H

#include <stdint.h>

/* Sonde le reseau (non bloquant) et, si un module complet arrive, le place
 * dans le buffer de staging et leve le drapeau d'attente.
 * Retourne 1 si un module est desormais en attente, 0 sinon.
 */
int deploy_try_stage(void);

/* Retourne 1 si un module recu attend d'etre charge, 0 sinon. */
int deploy_pending(void);

/* Nombre de mises a jour a chaud effectuees depuis le demarrage (metrique,
 * distincte de reset_count qui, lui, ne compte que les redemarrages materiels).
 */
uint32_t deploy_update_count(void);

#endif /* WAMR_DEPLOY_H */