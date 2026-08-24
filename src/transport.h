/*
 * src/transport.h — Interface de transport abstrait pour le firmware WAMR
 *
 * OBJECTIF
 * --------
 * Definir un contrat de transport UNIQUE, implemente par deux backends
 * interchangeables :
 *   - transport_wifi.c  (Wi-Fi + TCP)   compile si CONFIG_WIFI est actif
 *   - transport_ble.c   (BLE + NUS)     compile si CONFIG_BT  est actif
 *
 * Le reste du firmware (main.c, couche hote WAMR) ne connait QUE cette
 * interface : il ignore totalement quel backend est derriere.
 *
 * MISE A JOUR (upload reseau)
 * ---------------------------
 * Ajout de deux fonctions PROPRES AU BACKEND Wi-Fi, utilisees uniquement pour
 * la phase d'upload reseau du module .wasm (voir net_upload.c / main.c) :
 *   - transport_wifi_bring_up() : monte le Wi-Fi (association + IP) AVANT
 *     l'execution de la sonde, afin de pouvoir recevoir le .wasm par TCP ;
 *   - transport_wifi_is_up()    : indique si le Wi-Fi est deja monte.
 * Ces symboles ne sont definis que par transport_wifi.c ; main.c ne les
 * appelle que sous #if defined(CONFIG_WIFI), donc aucun lien n'est requis en
 * build BLE.
 *
 * Licence : Apache-2.0
 */

#ifndef WAMR_TRANSPORT_H
#define WAMR_TRANSPORT_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Nom du transport actif, expose au module WASM via host_get_transport_name. */
extern const char *transport_name(void);

/* Compteurs de transmission cumules, communs a tous les backends. */
struct transport_counters {
	uint32_t bytes_tx;        /* M4 */
	uint32_t bytes_rx;        /* M5 */
	uint32_t errors;          /* M6 */
};
extern struct transport_counters g_tx_counters;

/* Etablit le transport. Retourne un descripteur logique >= 0, ou -1. */
int transport_connect(const char *ip, size_t ip_len,
		      uint32_t port, uint32_t timeout_secs);

/* Attend que le transport soit pret a transmettre. 0 si pret, -1 sur timeout. */
int transport_wait_ready(uint32_t timeout_secs);

/* Emet un bloc de donnees. Retourne le nombre d'octets emis, ou < 0. */
int transport_send(int handle, const uint8_t *buf, uint32_t len);

/* Recoit un bloc (ACK applicatif). >=0 octets, < 0 en cas d'erreur. */
int transport_recv(int handle, uint8_t *buf, uint32_t len);

/* Ferme le transport. */
void transport_close(int handle);

/* RSSI courant en dBm. 0 si indisponible. */
int32_t transport_signal_dbm(void);

/* ---- Specifique Wi-Fi : bring-up pour l'upload reseau ---------------------
 * Definis uniquement dans transport_wifi.c (CONFIG_WIFI). En build BLE, main.c
 * ne les appelle pas (gardes #if defined(CONFIG_WIFI)).
 */
int transport_wifi_bring_up(uint32_t timeout_secs);
bool transport_wifi_is_up(void);

#endif /* WAMR_TRANSPORT_H */