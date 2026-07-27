/*
 * src/transport_wifi.c — Backend de transport Wi-Fi + TCP
 *
 * Compile UNIQUEMENT lorsque CONFIG_WIFI est actif. Sur une carte sans
 * Wi-Fi (ex. NUCLEO-WB55RG), ce fichier se reduit a du vide et n'introduit
 * aucune dependance : le Wi-Fi n'est donc jamais une exigence stricte.
 *
 * Licence : Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_WIFI)

#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "transport.h"

/* SSID / mot de passe du point d'acces.
 * Ce sont des parametres d'INFRASTRUCTURE (pas de l'equipement) : ils sont
 * identiques pour tous les noeuds Wi-Fi d'une campagne. Ils sont ici, cote
 * firmware, plutot que dans le WASM, pour que le .wasm reste agnostique du
 * transport. Adaptez-les a votre reseau, ou surchargez-les via Kconfig.
 */
#ifndef CONFIG_WAMR_WIFI_SSID
#define CONFIG_WAMR_WIFI_SSID "a26nguep-hotspot"
#endif
#ifndef CONFIG_WAMR_WIFI_PSK
#define CONFIG_WAMR_WIFI_PSK "123456789"
#endif

struct transport_counters g_tx_counters = {0, 0, 0};

static K_SEM_DEFINE(wifi_ip_ready, 0, 1);
static struct net_mgmt_event_callback dhcp_cb;

/* Cible TCP memorisee entre transport_connect() et le premier envoi. */
static char g_ip[32];
static uint32_t g_port;
static uint32_t g_sock_timeout;
static int g_fd = -1;

const char *transport_name(void)
{
	return "wifi";
}

static void on_dhcp(struct net_mgmt_event_callback *cb,
		    uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (event == NET_EVENT_IPV4_DHCP_BOUND) {
		k_sem_give(&wifi_ip_ready);
	}
}

int transport_connect(const char *ip, size_t ip_len,
		      uint32_t port, uint32_t timeout_secs)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		printk("[wifi] aucune interface reseau\n");
		return -1;
	}

	net_mgmt_init_event_callback(&dhcp_cb, on_dhcp,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&dhcp_cb);

	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_WAMR_WIFI_SSID,
		.ssid_length = strlen(CONFIG_WAMR_WIFI_SSID),
		.psk = (const uint8_t *)CONFIG_WAMR_WIFI_PSK,
		.psk_length = strlen(CONFIG_WAMR_WIFI_PSK),
		.channel = WIFI_CHANNEL_ANY,
		.security = WIFI_SECURITY_TYPE_PSK,
		.mfp = WIFI_MFP_OPTIONAL,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.timeout = SYS_FOREVER_MS,
	};

	/* La cible TCP est memorisee ici ; le socket reel est ouvert au premier
	 * envoi (ensure_socket), une fois l'adresse IP obtenue.
	 */
	uint32_t n = ip_len < sizeof(g_ip) - 1 ? ip_len : sizeof(g_ip) - 1;
	memcpy(g_ip, ip, n);
	g_ip[n] = '\0';
	g_port = port;
	g_sock_timeout = timeout_secs;

	printk("[wifi] connexion a \"%s\"...\n", CONFIG_WAMR_WIFI_SSID);
	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface,
			   &params, sizeof(params));
	if (ret != 0) {
		printk("[wifi] echec de connexion Wi-Fi (%d)\n", ret);
		return -1;
	}
	net_dhcpv4_start(iface);

	/* handle 0 = connexion logique Wi-Fi etablie, socket a la demande. */
	return 0;
}

int transport_wait_ready(uint32_t timeout_secs)
{
	if (k_sem_take(&wifi_ip_ready, K_SECONDS(timeout_secs)) != 0) {
		printk("[wifi] pas d'adresse IP (timeout)\n");
		return -1;
	}
	printk("[wifi] adresse IP obtenue\n");
	return 0;
}

/* Ouvre reellement le socket TCP au premier envoi. Le handle expose au WASM
 * reste stable (0) ; g_fd porte le vrai descripteur.
 */
static int ensure_socket(void)
{
	if (g_fd >= 0) {
		return g_fd;
	}
	int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		g_tx_counters.errors++;
		return -1;
	}
	struct zsock_timeval tv = {.tv_sec = g_sock_timeout, .tv_usec = 0};
	zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)g_port);
	if (zsock_inet_pton(AF_INET, g_ip, &addr.sin_addr) != 1) {
		zsock_close(sock);
		g_tx_counters.errors++;
		return -1;
	}
	if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		zsock_close(sock);
		g_tx_counters.errors++;
		return -1;
	}
	g_fd = sock;
	return g_fd;
}

int transport_send(int handle, const uint8_t *buf, uint32_t len)
{
	ARG_UNUSED(handle);
	/* HTTP/1.0 + Connection: close : un socket neuf par envoi. */
	int fd = ensure_socket();
	if (fd < 0) {
		return -1;
	}

	/* IMPORTANT : zsock_send() peut faire un envoi PARTIEL (renvoyer moins
	 * que 'len'), surtout pour des charges utiles de plusieurs centaines
	 * d'octets. Si on n'envoie pas la totalite, la requete HTTP arrive
	 * tronquee cote serveur, qui attend alors le reste du corps annonce par
	 * Content-Length et finit par timeouter. On boucle donc jusqu'a avoir
	 * tout emis.
	 */
	uint32_t total_sent = 0;
	while (total_sent < len) {
		int n = zsock_send(fd, buf + total_sent, len - total_sent, 0);
		if (n <= 0) {
			g_tx_counters.errors++;
			/* Socket casse : on le ferme pour repartir proprement. */
			zsock_close(g_fd);
			g_fd = -1;
			return (total_sent > 0) ? (int)total_sent : -1;
		}
		total_sent += (uint32_t)n;
	}
	g_tx_counters.bytes_tx += total_sent;
	return (int)total_sent;
}

int transport_recv(int handle, uint8_t *buf, uint32_t len)
{
	ARG_UNUSED(handle);
	if (g_fd < 0) {
		return -1;
	}
	int received = zsock_recv(g_fd, buf, len, 0);
	if (received > 0) {
		g_tx_counters.bytes_rx += (uint32_t)received;
	} else if (received < 0) {
		g_tx_counters.errors++;
	}
	/* Connection: close -> on ferme apres l'ACK pour le prochain cycle. */
	zsock_close(g_fd);
	g_fd = -1;
	return received;
}

void transport_close(int handle)
{
	ARG_UNUSED(handle);
	if (g_fd >= 0) {
		zsock_close(g_fd);
		g_fd = -1;
	}
}

int32_t transport_signal_dbm(void)
{
	struct net_if *iface = net_if_get_default();
	if (!iface) {
		return 0;
	}
	struct wifi_iface_status status = {0};
	int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
			   &status, sizeof(status));
	if (ret != 0 || status.state < WIFI_STATE_ASSOCIATED) {
		return 0;
	}
	return (int32_t)status.rssi;
}

#endif /* CONFIG_WIFI */