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
 * interface : il ignore totalement quel backend est derriere. C'est ce qui
 * permet au meme binaire .wasm de fonctionner sur une carte Wi-Fi (Heltec,
 * ESP32-C6) comme sur une carte BLE (NUCLEO-WB55RG), sans recompilation du
 * WASM.
 *
 * SELECTION A LA COMPILATION
 * --------------------------
 * Exactement UN backend est compile, selon la configuration Kconfig de la
 * carte + l'overlay de conf choisi (conf/wifi.conf ou conf/ble.conf) :
 *
 *   #if defined(CONFIG_WIFI)      -> transport_wifi.c fournit l'implementation
 *   #elif defined(CONFIG_BT)      -> transport_ble.c  fournit l'implementation
 *   #else                         -> erreur de compilation explicite
 *
 * Le Wi-Fi n'est donc JAMAIS une dependance stricte : une carte sans Wi-Fi
 * (comme le NUCLEO-WB55RG) compile parfaitement en mode BLE.
 *
 * Licence : Apache-2.0
 */

#ifndef WAMR_TRANSPORT_H
#define WAMR_TRANSPORT_H

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stddef.h>

/* Nom du transport actif, expose au module WASM via host_get_transport_name.
 * "wifi" ou "ble". Defini par le backend compile.
 */
extern const char *transport_name(void);

/* Compteurs de transmission cumules, communs a tous les backends.
 * Mis a jour par le backend actif dans transport_send()/transport_recv().
 * Lus par les host functions M4/M5/M6.
 */
struct transport_counters {
	uint32_t bytes_tx;        /* M4 */
	uint32_t bytes_rx;        /* M5 */
	uint32_t errors;          /* M6 */
};
extern struct transport_counters g_tx_counters;

/* Etablit le transport.
 *   - Wi-Fi : connecte le Wi-Fi puis ouvre un socket TCP vers (ip, port).
 *   - BLE   : demarre l'advertising ; ip/port ignores.
 * Retourne un descripteur logique >= 0, ou -1 en cas d'echec.
 */
int transport_connect(const char *ip, size_t ip_len,
		      uint32_t port, uint32_t timeout_secs);

/* Attend que le transport soit pret a transmettre.
 *   - Wi-Fi : adresse IPv4 obtenue (DHCP).
 *   - BLE   : un central s'est connecte ET a active les notifications NUS.
 * Retourne 0 si pret, -1 sur timeout.
 */
int transport_wait_ready(uint32_t timeout_secs);

/* Emet un bloc de donnees. Retourne le nombre d'octets emis, ou < 0.
 * Met a jour g_tx_counters.bytes_tx / errors.
 */
int transport_send(int handle, const uint8_t *buf, uint32_t len);

/* Recoit un bloc (ACK applicatif). Retourne le nombre d'octets recus,
 * 0 si rien dans le delai, < 0 en cas d'erreur.
 * Met a jour g_tx_counters.bytes_rx / errors.
 */
int transport_recv(int handle, uint8_t *buf, uint32_t len);

/* Ferme le transport (Wi-Fi : ferme le socket ; BLE : arrete l'advertising). */
void transport_close(int handle);

/* RSSI courant en dBm (Wi-Fi ou BLE selon le backend). 0 si indisponible.
 * Valeur signee negative.
 */
int32_t transport_signal_dbm(void);

#endif /* WAMR_TRANSPORT_H */
