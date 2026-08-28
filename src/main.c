/*
 * src/main.c — Firmware hote Zephyr generique, multi-transport (Wi-Fi / BLE)
 *
 * Reception d'un module .wasm puis execution, avec MISE A JOUR A CHAUD en
 * config Wi-Fi.
 *
 * MISE A JOUR A CHAUD (hot-update)
 * --------------------------------
 * Modele COOPERATIF a un seul fil d'execution :
 *   - la sonde WASM interroge periodiquement le reseau (host_poll_update) ;
 *   - si un nouveau .wasm arrive, il est recu dans un SECOND buffer (staging)
 *     et un drapeau est leve ;
 *   - la sonde, informee, sort proprement de sa boucle (return), sans etre
 *     interrompue en plein appel hote ;
 *   - le firmware reprend la main, DETRUIT l'instance WASM courante, bascule
 *     le buffer de staging en buffer courant, incremente update_count, puis
 *     charge et execute le nouveau module.
 *
 * Repli : si le nouveau module est invalide (chargement echoue), on ne fait
 * pas de rollback ; la carte retourne en attente d'un autre upload (elle
 * reste joignable). Le module fautif est simplement ignore.
 *
 * En CONFIG BLE (pas de CONFIG_WIFI), la mise a jour a chaud n'existe pas :
 * upload par UART, avec redemarrage, exactement comme avant.
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
#include "deploy.h"

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

/* --- Deux buffers d'octets (module courant + module en attente) -------------
 * On alterne les pointeurs g_cur / g_stage a chaque mise a jour : pas de copie,
 * juste un echange de pointeurs. Seuls des OCTETS coexistent, jamais deux
 * instances WASM.
 */
static uint8_t wasm_buf_a[WASM_MAX_SIZE];
static uint8_t wasm_buf_b[WASM_MAX_SIZE];
static uint8_t *g_cur   = wasm_buf_a;   /* module en cours d'execution */
static uint8_t *g_stage = wasm_buf_b;   /* module recu, en attente     */
static uint32_t g_cur_size;
static uint32_t g_stage_size;
static volatile bool g_pending;         /* un module attend d'etre charge */
static uint32_t g_update_count;         /* nb de mises a jour a chaud     */

static char wamr_pool[WAMR_POOL_SIZE] __aligned(8);

static const struct device *uart_dev;

/* ==========================================================================
 * Contrat de deploiement (declare dans deploy.h, appele par host_api.c).
 * ========================================================================== */

int deploy_try_stage(void)
{
#if defined(CONFIG_WIFI)
	if (g_pending) {
		return 1;   /* deja un module en attente : on n'en recoit pas un 2e */
	}
	uint32_t sz = 0;
	int r = net_upload_try(g_stage, WASM_MAX_SIZE, &sz);
	if (r == 1) {
		g_stage_size = sz;
		g_pending = true;
		return 1;
	}
#endif
	return 0;
}

int deploy_pending(void)
{
	return g_pending ? 1 : 0;
}

uint32_t deploy_update_count(void)
{
	return g_update_count;
}

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

/* Recoit taille + corps a partir d'un premier octet DEJA lu, dans `dst`.
 * Retourne true si un module valide a ete recu.
 */
static bool uart_receive_module(const struct device *dev, uint8_t first,
				uint8_t *dst, uint32_t *size_out)
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
		uart_read_byte(dev, &dst[i]);
	}
	printk("Upload complete (%u bytes)\n", wasm_size);
	*size_out = wasm_size;
	return true;
}

/* --------------------------------------------------------------------------
 * Execution d'un module WASM. Retourne true si le module a ete charge et
 * execute (fin normale ou exception), false si le CHARGEMENT a echoue
 * (module invalide/corrompu) -> repli.
 *
 * NB : wasm_data est un uint8_t* NON const (wasm_runtime_load l'exige).
 * -------------------------------------------------------------------------- */

static bool execute_wasm(uint8_t *wasm_data, uint32_t wasm_size)
{
	char error_buf[128];
	wasm_module_t module = NULL;
	wasm_module_inst_t inst = NULL;
	wasm_exec_env_t exec_env = NULL;
	wasm_function_inst_t func = NULL;
	bool loaded = false;

	module = wasm_runtime_load(wasm_data, wasm_size,
				   error_buf, sizeof(error_buf));
	if (!module) {
		printk("LOAD ERROR: %s\n", error_buf);
		return false;   /* module invalide -> repli */
	}
	loaded = true;
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
	return loaded;
}

/* --------------------------------------------------------------------------
 * Cycle de vie d'execution avec mise a jour a chaud.
 *
 * Execute g_cur ; a la sortie de la sonde, si un module est en attente
 * (g_pending, leve par la sonde via host_poll_update), on bascule dessus et
 * on recommence. Sinon on rend la main (retour a l'attente d'un nouvel upload).
 * -------------------------------------------------------------------------- */

static void run_with_hot_update(void)
{
	bool keep = true;
	while (keep) {
		bool loaded = execute_wasm(g_cur, g_cur_size);

		if (g_pending) {
			/* Un module attend : on bascule (echange de pointeurs). */
			uint8_t *tmp = g_cur;
			g_cur = g_stage;
			g_stage = tmp;
			g_cur_size = g_stage_size;
			g_pending = false;
			g_update_count++;
			printk("\n[deploy] MISE A JOUR A CHAUD #%u (%u octets)\n",
			       g_update_count, g_cur_size);
			/* on boucle : execution du nouveau module */
		} else if (!loaded) {
			/* Module courant invalide et rien en attente : repli. */
			printk("[deploy] module invalide, retour en attente\n");
			keep = false;
		} else {
			/* Sonde terminee sans mise a jour (arret propre/erreur) :
			 * retour a l'attente d'un nouvel upload. */
			printk("\nWaiting upload...\n");
			keep = false;
		}
	}
}

/* --------------------------------------------------------------------------
 * Programme principal.
 * -------------------------------------------------------------------------- */

int main(void)
{
	printk("\n");
	printk("========================================================\n");
	printk(" Firmware WAMR multi-transport (Wi-Fi / BLE) + hot-update\n");
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
	/* Bootstrap reseau : on monte le Wi-Fi pour autoriser l'upload TCP. */
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

	printk("\n===== DEPLOYMENT — Metrics Edition (hot-update) =====\n");
	printk("Protocol : 4 bytes size (LE) + wasm binary\n");
	printk("Max size : %d bytes\n", WASM_MAX_SIZE);
#if defined(CONFIG_WIFI)
	printk("Voies    : %s\n", net_ready ? "UART + reseau (TCP)" : "UART");
	printk("Hot-update : %s\n", net_ready ? "actif (reseau)" : "inactif");
#else
	printk("Voies    : UART\n");
	printk("Hot-update : inactif (BLE)\n");
#endif

	/* Boucle d'attente du PREMIER module : UART puis, en Wi-Fi, reseau.
	 * Le module initial est recu dans le buffer courant g_cur. */
	while (1) {
		bool got = false;

		/* 1) UART : sondage non bloquant du premier octet. */
		uint8_t first;
		if (uart_poll_in(uart_dev, &first) == 0) {
			if (uart_receive_module(uart_dev, first,
						g_cur, &g_cur_size)) {
				got = true;
			}
		}

#if defined(CONFIG_WIFI)
		/* 2) Reseau : sondage court d'un client TCP (dans g_cur). */
		if (!got && net_ready) {
			uint32_t sz = 0;
			int r = net_upload_try(g_cur, WASM_MAX_SIZE, &sz);
			if (r == 1) {
				g_cur_size = sz;
				got = true;
			}
		}
#endif

		if (got) {
			/* Execution + mises a jour a chaud successives. */
			run_with_hot_update();
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