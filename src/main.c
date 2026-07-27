/*
 * src/main.c — Firmware hote Zephyr generique, multi-transport (Wi-Fi / BLE)
 *
 * Ce fichier ne contient AUCUN code specifique a un transport ni a une carte.
 * Il se contente de :
 *   1. initialiser le runtime WAMR (pool memoire) ;
 *   2. enregistrer les fonctions hotes (couche host_api, qui delegue au
 *      backend transport compile : Wi-Fi OU BLE) ;
 *   3. recevoir un module .wasm par UART (4 octets de taille little-endian
 *      suivis du binaire) ;
 *   4. l'executer.
 *
 * Le choix du transport est fait a la COMPILATION via la configuration
 * (voir conf/wifi.conf et conf/ble.conf) : ce fichier est identique quel que
 * soit le transport.
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

#define UART_NODE DT_CHOSEN(zephyr_console)

#define WASM_MAX_SIZE  (40  * 1024)
#define STACK_SIZE     (8   * 1024)
#define HEAP_SIZE      (16  * 1024)
#define WAMR_POOL_SIZE (160 * 1024)

static uint8_t wasm_buffer[WASM_MAX_SIZE];
static char wamr_pool[WAMR_POOL_SIZE] __aligned(8);

static const struct device *uart_dev;

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

	printk("\n===== UART DEPLOYMENT — Metrics Edition =====\n");
	printk("Protocol : 4 bytes size (LE) + wasm binary\n");
	printk("Max size : %d bytes\n", WASM_MAX_SIZE);

	while (1) {
		uint32_t wasm_size = 0;
		printk("\nWaiting upload...\n");

		for (int i = 0; i < 4; i++) {
			uint8_t b;
			uart_read_byte(uart_dev, &b);
			((uint8_t *)&wasm_size)[i] = b;
		}
		printk("Incoming size = %u bytes\n", wasm_size);

		if (wasm_size == 0 || wasm_size > WASM_MAX_SIZE) {
			printk("ERROR: invalid size (0 < size <= %d)\n",
			       WASM_MAX_SIZE);
			uart_drain_rx(uart_dev);
			continue;
		}

		for (uint32_t i = 0; i < wasm_size; i++) {
			uart_read_byte(uart_dev, &wasm_buffer[i]);
		}
		printk("Upload complete (%u bytes)\n", wasm_size);

		execute_wasm(wasm_buffer, wasm_size);
	}

	return 0;
}
