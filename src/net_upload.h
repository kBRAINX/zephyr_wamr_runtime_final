/*
 * Reception d'un module .wasm par le reseau (TCP).
 *
 * Uniquement pertinent en config Wi-Fi. Les fonctions ne sont definies que
 * dans net_upload.c sous #if defined(CONFIG_WIFI) ; main.c ne les appelle que
 * sous la meme garde, donc aucun lien n'est requis en build BLE.
 *
 * Protocole (identique a l'UART) : 4 octets de taille (uint32 little-endian)
 * suivis du binaire .wasm. Le serveur repond "OK" a la fin.
 *
 * Licence : Apache-2.0
 */

#ifndef WAMR_NET_UPLOAD_H
#define WAMR_NET_UPLOAD_H

#include <stdint.h>

/* Ouvre le socket TCP d'ecoute sur le port donne. Retourne 0 si OK, -1 sinon. */
int net_upload_init(uint16_t port);

/* Verifie (sans bloquer longuement) si un client pousse un module.
 *   - retourne 1 si un module complet a ete recu dans `buf` (*out_size rempli) ;
 *   - retourne 0 si aucun client ne s'est presente ;
 *   - retourne -1 en cas de transfert errone (a ignorer, on reessaie).
 */
int net_upload_try(uint8_t *buf, uint32_t max_size, uint32_t *out_size);

/* Ferme le socket d'ecoute (optionnel). */
void net_upload_stop(void);

#endif /* WAMR_NET_UPLOAD_H */