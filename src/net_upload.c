/*
 * Reception d'un module .wasm par le reseau (TCP).
 *
 * Compile UNIQUEMENT en config Wi-Fi (#if defined(CONFIG_WIFI)). En build BLE,
 * ce fichier se reduit a du vide et n'introduit aucune dependance : l'upload
 * reseau n'existe que sur les cartes Wi-Fi, l'UART restant la seule voie en BLE.
 *
 * Le socket d'ecoute reste BLOQUANT ; on teste la presence d'un client avec
 * zsock_poll() (timeout court), ce qui evite tout reglage non bloquant et rend
 * la boucle d'attente de main.c reactive tant a l'UART qu'au reseau.
 *
 * Licence : Apache-2.0
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_WIFI)

#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "net_upload.h"

/* Duree du sondage de presence d'un client (ms). Sert aussi de rythme a la
 * boucle d'attente cote main.c. */
#define POLL_MS      50
/* Delai max de reception d'un transfert une fois le client connecte (s). */
#define RECV_TIMEOUT 15

static int listen_fd = -1;

int net_upload_init(uint16_t port)
{
	int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		printk("[upload] creation du socket d'ecoute echouee\n");
		return -1;
	}

	int yes = 1;
	zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printk("[upload] bind sur le port %u echoue\n", port);
		zsock_close(fd);
		return -1;
	}
	if (zsock_listen(fd, 1) < 0) {
		printk("[upload] listen echoue\n");
		zsock_close(fd);
		return -1;
	}

	listen_fd = fd;
	return 0;
}

static int recv_all(int fd, uint8_t *buf, uint32_t n)
{
	uint32_t got = 0;
	while (got < n) {
		int r = zsock_recv(fd, buf + got, n - got, 0);
		if (r <= 0) {
			return -1;
		}
		got += (uint32_t)r;
	}
	return 0;
}

int net_upload_try(uint8_t *buf, uint32_t max_size, uint32_t *out_size)
{
	if (listen_fd < 0) {
		return 0;
	}

	/* Un client est-il en attente ? (sondage court, non bloquant en pratique) */
	struct zsock_pollfd pfd = {
		.fd = listen_fd,
		.events = ZSOCK_POLLIN,
	};
	int pr = zsock_poll(&pfd, 1, POLL_MS);
	if (pr <= 0 || !(pfd.revents & ZSOCK_POLLIN)) {
		return 0;   /* aucun client */
	}

	struct sockaddr_in cli;
	socklen_t clen = sizeof(cli);
	int cfd = zsock_accept(listen_fd, (struct sockaddr *)&cli, &clen);
	if (cfd < 0) {
		return 0;
	}
	printk("[upload] client reseau connecte\n");

	struct zsock_timeval tv = {.tv_sec = RECV_TIMEOUT, .tv_usec = 0};
	zsock_setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* 1) taille sur 4 octets (little-endian, identique a l'UART) */
	uint8_t hdr[4];
	if (recv_all(cfd, hdr, 4) != 0) {
		printk("[upload] taille non recue\n");
		zsock_close(cfd);
		return -1;
	}
	uint32_t size = (uint32_t)hdr[0] |
			((uint32_t)hdr[1] << 8) |
			((uint32_t)hdr[2] << 16) |
			((uint32_t)hdr[3] << 24);

	if (size == 0 || size > max_size) {
		printk("[upload] taille invalide : %u (max %u)\n", size, max_size);
		zsock_close(cfd);
		return -1;
	}
	printk("[upload] reception reseau de %u octets...\n", size);

	/* 2) binaire */
	if (recv_all(cfd, buf, size) != 0) {
		printk("[upload] transfert reseau incomplet\n");
		zsock_close(cfd);
		return -1;
	}

	(void)zsock_send(cfd, "OK", 2, 0);
	zsock_close(cfd);

	*out_size = size;
	printk("[upload] module recu par reseau (%u octets)\n", size);
	return 1;
}

void net_upload_stop(void)
{
	if (listen_fd >= 0) {
		zsock_close(listen_fd);
		listen_fd = -1;
	}
}

#endif /* CONFIG_WIFI */