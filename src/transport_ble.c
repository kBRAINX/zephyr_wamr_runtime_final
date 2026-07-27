/*
 * src/transport_ble.c — Backend de transport Bluetooth LE + NUS
 *
 * Compile UNIQUEMENT lorsque CONFIG_BT est actif ET que CONFIG_WIFI ne l'est
 * pas (voir la selection dans transport.h / main.c). Utilise le service NUS
 * (Nordic UART Service) NATIF de Zephyr upstream :
 *     zephyr/bluetooth/services/nus.h   (CONFIG_BT_ZEPHYR_NUS)
 * Aucune dependance au SDK nRF.
 *
 * MODELE DE COMMUNICATION
 * -----------------------
 * L'equipement joue le role de PERIPHERIQUE BLE :
 *   1. il annonce (advertising) le service NUS ;
 *   2. une PASSERELLE (un PC, role central, voir gateway/ble_gateway.py) se
 *      connecte et souscrit aux notifications de la caracteristique TX ;
 *   3. l'equipement envoie les metriques par notifications NUS ;
 *   4. la passerelle relaie ces donnees vers le serveur HTTP.
 *
 * Un PC ne disposant pas de pile IP au-dessus du BLE, la passerelle est
 * indispensable : c'est elle qui transforme le flux NUS en requetes HTTP
 * comprehensibles par le serveur de collecte, exactement comme en Wi-Fi.
 *
 * Licence : Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_BT) && !defined(CONFIG_WIFI)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/services/nus.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "transport.h"

struct transport_counters g_tx_counters = {0, 0, 0};

/* Etat de la liaison BLE. */
static struct bt_conn *current_conn;
static volatile bool notif_enabled;     /* le central a souscrit a TX */
static int8_t last_rssi;                 /* RSSI de la connexion, si dispo */

static K_SEM_DEFINE(ble_ready, 0, 1);    /* leve quand notif_enabled devient vrai */

/* Nom d'annonce BLE. Repris de CONFIG_BT_DEVICE_NAME pour rester coherent
 * avec l'identite exposee au WASM.
 */
#define ADV_NAME CONFIG_BT_DEVICE_NAME

const char *transport_name(void)
{
	return "ble";
}

/* Donnees d'annonce : drapeaux + nom complet. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, ADV_NAME, sizeof(ADV_NAME) - 1),
};

/* Reponse au scan : UUID du service NUS, pour que la passerelle le repere. */
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static void start_advertising(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err) {
		printk("[ble] echec de demarrage de l'advertising (%d)\n", err);
	} else {
		printk("[ble] advertising demarre (nom \"%s\")\n", ADV_NAME);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("[ble] connexion echouee (0x%02x)\n", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	printk("[ble] central connecte\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	printk("[ble] central deconnecte (0x%02x)\n", reason);
	notif_enabled = false;
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	/* On redemarre l'advertising pour permettre une reconnexion. */
	start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Callback NUS : le central active/desactive les notifications. */
static void nus_notif_enabled(bool enabled, void *ctx)
{
	ARG_UNUSED(ctx);
	notif_enabled = enabled;
	printk("[ble] notifications NUS %s\n", enabled ? "activees" : "desactivees");
	if (enabled) {
		k_sem_give(&ble_ready);
	}
}

/* Callback NUS : donnees recues du central (utilise comme canal d'ACK). */
static uint8_t rx_ring[64];
static volatile uint16_t rx_len;

static void nus_received(struct bt_conn *conn, const void *data,
			 uint16_t len, void *ctx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(ctx);
	uint16_t n = len < sizeof(rx_ring) ? len : sizeof(rx_ring);
	memcpy(rx_ring, data, n);
	rx_len = n;
	g_tx_counters.bytes_rx += n;
}

static struct bt_nus_cb nus_listener = {
	.notif_enabled = nus_notif_enabled,
	.received = nus_received,
};

int transport_connect(const char *ip, size_t ip_len,
		      uint32_t port, uint32_t timeout_secs)
{
	ARG_UNUSED(ip);
	ARG_UNUSED(ip_len);
	ARG_UNUSED(port);
	ARG_UNUSED(timeout_secs);

	int err = bt_enable(NULL);
	if (err) {
		printk("[ble] bt_enable a echoue (%d)\n", err);
		return -1;
	}
	printk("[ble] pile Bluetooth initialisee\n");

	err = bt_nus_cb_register(&nus_listener, NULL);
	if (err) {
		printk("[ble] enregistrement du callback NUS echoue (%d)\n", err);
		return -1;
	}

	start_advertising();
	/* handle 0 = liaison BLE logique en attente de connexion. */
	return 0;
}

int transport_wait_ready(uint32_t timeout_secs)
{
	/* Pret = un central connecte ET les notifications activees. */
	if (k_sem_take(&ble_ready, K_SECONDS(timeout_secs)) != 0) {
		printk("[ble] aucun central abonne (timeout)\n");
		return -1;
	}
	printk("[ble] passerelle abonnee, transport pret\n");
	return 0;
}

int transport_send(int handle, const uint8_t *buf, uint32_t len)
{
	ARG_UNUSED(handle);

	if (!notif_enabled || current_conn == NULL) {
		g_tx_counters.errors++;
		return -1;
	}

	/* bt_nus_send fragmente automatiquement selon le MTU negocie.
	 * On envoie l'integralite du bloc (en-tete HTTP + JSON + '\n').
	 */
	int err = bt_nus_send(current_conn, buf, (uint16_t)len);
	if (err) {
		g_tx_counters.errors++;
		return -1;
	}
	g_tx_counters.bytes_tx += len;
	return (int)len;
}

int transport_recv(int handle, uint8_t *buf, uint32_t len)
{
	ARG_UNUSED(handle);
	/* ACK applicatif eventuel recu via nus_received(). Non bloquant. */
	if (rx_len == 0) {
		return 0;
	}
	uint16_t n = rx_len < len ? rx_len : (uint16_t)len;
	memcpy(buf, rx_ring, n);
	rx_len = 0;
	return n;
}

void transport_close(int handle)
{
	ARG_UNUSED(handle);
	if (current_conn) {
		bt_conn_disconnect(current_conn,
				   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

int32_t transport_signal_dbm(void)
{
	/* La lecture du RSSI de connexion via HCI (bt_conn_le_get_rssi ou
	 * commande HCI Read RSSI) n'est pas exposee de facon portable sur tous
	 * les controleurs. On retourne la derniere valeur connue si elle a ete
	 * captee, sinon 0. Sur STM32WB, la commande HCI Read RSSI peut etre
	 * cablee ici si besoin ; laisse a 0 par defaut pour rester generique.
	 */
	return (int32_t)last_rssi;
}

#endif /* CONFIG_BT && !CONFIG_WIFI */
