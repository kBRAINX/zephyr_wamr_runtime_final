/*
 * src/main.c — Firmware hote Zephyr generique, multi-transport (Wi-Fi / BLE)
 *
 * Ce fichier initialise WAMR, enregistre les fonctions hotes, RECOIT un module
 * .wasm puis l'execute.
 *
 * MISE A JOUR (upload reseau)
 * ---------------------------
 * En CONFIG WI-FI, le module .wasm peut desormais etre recu par DEUX voies :
 *   - l'UART (comme auparavant), ou
 *   - le RESEAU (TCP), la carte agissant en serveur d'upload (port 5555).
 * Le firmware ecoute les DEUX simultanement : la premiere voie qui livre un
 * module l'emporte. Pour rendre l'upload TCP possible, le Wi-Fi est monte ICI,
 * cote hote, AVANT l'attente (bootstrap reseau) ; la sonde WASM reutilise
 * ensuite cette connexion (cf. transport_wifi.c, drapeau g_wifi_up).
 *
 * En CONFIG BLE (pas de CONFIG_WIFI), rien ne change : seul le chemin UART est
 * compile, a l'identique de la version precedente.
 *
 * Licence : Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "wasm_export.h"
#include "host_api.h"

#if defined(CONFIG_WIFI)
#include "transport.h"
#include "net_upload.h"
#define UPLOAD_TCP_PORT      5555   /* port d'ecoute pour l'upload reseau */
#define WIFI_BRINGUP_SECS    20     /* delai max de montee du Wi-Fi au boot */
#endif

#define UART_NODE DT_CHOSEN(zephyr_console)

/* Tailles WAMR configurables par carte via Kconfig. */
#ifndef CONFIG_WAMR_APP_POOL_SIZE_KB
#define CONFIG_WAMR_APP_POOL_SIZE_KB 160
#endif
#ifndef CONFIG_WAMR_APP_WASM_MAX_KB
#define CONFIG_WAMR_APP_WASM_MAX_KB 40
#endif
#ifndef CONFIG_WAMR_APP_STACK_KB
#define CONFIG_WAMR_APP_STACK_KB 8
#endif
#ifndef CONFIG_WAMR_APP_HEAP_KB
#define CONFIG_WAMR_APP_HEAP_KB 16
#endif

#define WASM_MAX_SIZE  (CONFIG_WAMR_APP_WASM_MAX_KB  * 1024)
#define STACK_SIZE     (CONFIG_WAMR_APP_STACK_KB     * 1024)
#define HEAP_SIZE      (CONFIG_WAMR_APP_HEAP_KB      * 1024)
#define WAMR_POOL_SIZE (CONFIG_WAMR_APP_POOL_SIZE_KB * 1024)

static uint8_t wasm_buffer[WASM_MAX_SIZE];
static char wamr_pool[WAMR_POOL_SIZE] __aligned(8);

static const struct device *uart_dev;

/* --------------------------------------------------------------------------
 * Reception UART (inchangee) : protocole [taille(4 LE)][binaire].
 * -------------------------------------------------------------------------- */

static void uart_read_byte(const struct device *dev, uint8_t *out)
{
	while (uart_poll_in(dev, out) != 0) {
		k_yield();
	}
}

static void uart_drain_rx(const struct device *dev)
{
	uint8_t dummy;
	int rounds = 0;
	int drained;
	do {
		drained = 0;
		while (uart_poll_in(dev, &dummy) == 0) {
			drained++;
		}
		if (drained > 0) {
			k_msleep(300);
			rounds = 0;
		} else {
			rounds++;
			k_msleep(100);
		}
	} while (rounds < 5);
	printk("UART resync OK\n");
}

/* Recoit taille + corps a partir d'un premier octet DEJA lu (detection UART).
 * Retourne true si un module valide a ete recu dans wasm_buffer.
 */
static bool uart_receive_module(const struct device *dev, uint8_t first,
				uint32_t *size_out)
{
	uint32_t wasm_size = 0;
	((uint8_t *)&wasm_size)[0] = first;
	for (int i = 1; i < 4; i++) {
		uint8_t b;
		uart_read_byte(dev, &b);
		((uint8_t *)&wasm_size)[i] = b;
	}
	printk("Incoming size = %u bytes\n", wasm_size);

	if (wasm_size == 0 || wasm_size > WASM_MAX_SIZE) {
		printk("ERROR: invalid size (0 < size <= %d)\n", WASM_MAX_SIZE);
		uart_drain_rx(dev);
		return false;
	}

	for (uint32_t i = 0; i < wasm_size; i++) {
		uart_read_byte(dev, &wasm_buffer[i]);
	}
	printk("Upload complete (%u bytes)\n", wasm_size);
	*size_out = wasm_size;
	return true;
}

/* --------------------------------------------------------------------------
 * Execution du module WASM (inchangee).
 *
 * NB : wasm_data est un uint8_t* NON const, car wasm_runtime_load() attend un
 * pointeur non const (sinon avertissement -Wdiscarded-qualifiers).
 * -------------------------------------------------------------------------- */

static void execute_wasm(uint8_t *wasm_data, uint32_t wasm_size)
{
	char error_buf[128];
	wasm_module_t module = NULL;
	wasm_module_inst_t inst = NULL;
	wasm_exec_env_t exec_env = NULL;
	wasm_function_inst_t func = NULL;

	module = wasm_runtime_load(wasm_data, wasm_size,
				   error_buf, sizeof(error_buf));
	if (!module) {
		printk("LOAD ERROR: %s\n", error_buf);
		return;
	}
	printk("Module charge OK\n");

	inst = wasm_runtime_instantiate(module, STACK_SIZE, HEAP_SIZE,
					error_buf, sizeof(error_buf));
	if (!inst) {
		printk("INSTANTIATE ERROR: %s\n", error_buf);
		goto unload;
	}
	printk("Instance creee OK\n");

	exec_env = wasm_runtime_create_exec_env(inst, STACK_SIZE);
	if (!exec_env) {
		printk("EXEC ENV FAILED\n");
		goto deinstantiate;
	}

	func = wasm_runtime_lookup_function(inst, "main");
	if (!func) {
		func = wasm_runtime_lookup_function(inst, "_start");
	}
	if (!func) {
		printk("point d'entree introuvable (main/_start)\n");
		goto destroy_env;
	}

	printk("Execution du module WASM...\n");
	if (!wasm_runtime_call_wasm(exec_env, func, 0, NULL)) {
		printk("EXCEPTION: %s\n", wasm_runtime_get_exception(inst));
	} else {
		printk("Execution terminee\n");
	}

destroy_env:
	wasm_runtime_destroy_exec_env(exec_env);
deinstantiate:
	wasm_runtime_deinstantiate(inst);
unload:
	wasm_runtime_unload(module);
}

/* --------------------------------------------------------------------------
 * Programme principal.
 * -------------------------------------------------------------------------- */

int main(void)
{
	printk("\n");
	printk("========================================================\n");
	printk(" Firmware WAMR multi-transport (Wi-Fi / BLE)\n");
	printk("========================================================\n");

	RuntimeInitArgs init_args;
	memset(&init_args, 0, sizeof(init_args));
	init_args.mem_alloc_type = Alloc_With_Pool;
	init_args.mem_alloc_option.pool.heap_buf = wamr_pool;
	init_args.mem_alloc_option.pool.heap_size = sizeof(wamr_pool);

	if (!wasm_runtime_full_init(&init_args)) {
		printk("WAMR init failed\n");
		return -1;
	}
	printk("WAMR init OK (pool=%d KB)\n", WAMR_POOL_SIZE / 1024);

	host_set_pool(wamr_pool, sizeof(wamr_pool));
	host_reset_counter_init();

	if (!host_register_natives()) {
		return -1;
	}

	uart_dev = DEVICE_DT_GET(UART_NODE);
	if (!device_is_ready(uart_dev)) {
		printk("UART not ready\n");
		return -1;
	}

	bool net_ready = false;

#if defined(CONFIG_WIFI)
	/* Bootstrap reseau : on tente de monter le Wi-Fi pour autoriser l'upload
	 * TCP. En cas d'echec (pas d'AP), on retombe sur l'UART uniquement. */
	printk("[boot] config Wi-Fi : montee du reseau pour l'upload...\n");
	if (transport_wifi_bring_up(WIFI_BRINGUP_SECS) == 0) {
		if (net_upload_init(UPLOAD_TCP_PORT) == 0) {
			net_ready = true;
			printk("[boot] upload reseau actif (port TCP %d)\n",
			       UPLOAD_TCP_PORT);
		}
	} else {
		printk("[boot] Wi-Fi indisponible : upload par UART uniquement\n");
	}
#endif

	printk("\n===== DEPLOYMENT — Metrics Edition =====\n");
	printk("Protocol : 4 bytes size (LE) + wasm binary\n");
	printk("Max size : %d bytes\n", WASM_MAX_SIZE);
#if defined(CONFIG_WIFI)
	printk("Voies    : %s\n", net_ready ? "UART + reseau (TCP)" : "UART");
#else
	printk("Voies    : UART\n");
#endif

	/* Boucle d'attente : on sonde l'UART puis, en Wi-Fi, le reseau.
	 * La premiere voie qui livre un module complet l'emporte. */
	while (1) {
		uint32_t wasm_size = 0;
		bool got = false;

		/* 1) UART : sondage non bloquant du premier octet. */
		uint8_t first;
		if (uart_poll_in(uart_dev, &first) == 0) {
			if (uart_receive_module(uart_dev, first, &wasm_size)) {
				got = true;
			}
		}

#if defined(CONFIG_WIFI)
		/* 2) Reseau : sondage court d'un client TCP. */
		if (!got && net_ready) {
			int r = net_upload_try(wasm_buffer, WASM_MAX_SIZE,
					       &wasm_size);
			if (r == 1) {
				got = true;
			}
		}
#endif

		if (got) {
			execute_wasm(wasm_buffer, wasm_size);
			/* La sonde boucle indefiniment : en pratique on ne
			 * revient pas ici. Si le module s'arrete (erreur), on
			 * se remet en attente d'un nouvel upload. */
			printk("\nWaiting upload...\n");
		} else {
#if defined(CONFIG_WIFI)
			if (!net_ready) {
				k_msleep(20);
			}
			/* Si net_ready, net_upload_try() a deja temporise (poll). */
#else
			k_msleep(20);
#endif
		}
	}

	return 0;
}