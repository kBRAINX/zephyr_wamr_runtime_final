/*
 * src/host_api.c — Couche hote WAMR (transport abstrait + metriques + identite)
 *
 * Cette couche fait le pont entre le module WASM et le firmware natif. Elle
 * n'expose au WASM que des host functions GENERIQUES :
 *   - transport abstrait (host_transport_*) branche sur transport.h, donc
 *     sur le backend Wi-Fi OU BLE compile, sans que le WASM le sache ;
 *   - metriques BRUTES uniquement (aucun calcul derive ici : idle ratio et
 *     statut sont calcules dans le WASM) ;
 *   - identite (device/type/os/transport) resolue a l'execution.
 *
 * La table native_symbols est IDENTIQUE (noms + signatures) a celle attendue
 * par le module .wasm. Toute divergence casse la portabilite binaire.
 *
 * Licence : Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "wasm_export.h"
#include "host_api.h"
#include "transport.h"

/* ----------------------------------------------------------------
 * Identite du noeud — surchargeable via Kconfig / build flags.
 *
 * - DEVICE_NAME : identifiant unique du noeud.
 * - DEVICE_TYPE : famille materielle (informative).
 * - OS_NAME     : systeme hote.
 * transport : fourni dynamiquement par transport_name() (wifi/ble).
 * ---------------------------------------------------------------- */
#ifndef CONFIG_WAMR_NODE_DEVICE_NAME
#define CONFIG_WAMR_NODE_DEVICE_NAME "zephyr_node"
#endif
#ifndef CONFIG_WAMR_NODE_DEVICE_TYPE
#define CONFIG_WAMR_NODE_DEVICE_TYPE "generic"
#endif
#ifndef CONFIG_WAMR_NODE_OS_NAME
#define CONFIG_WAMR_NODE_OS_NAME "zephyr"
#endif

/* Pool WAMR, pour la resolution de pointeur en dernier recours. */
static void *g_pool_base;
static size_t g_pool_size;

/* Compteur de redemarrages (M10). Sans stockage persistant portable sur
 * toutes les cartes, on compte les executions depuis la mise sous tension.
 */
static uint32_t g_reset_count;

void host_set_pool(void *pool, size_t size)
{
	g_pool_base = pool;
	g_pool_size = size;
}

void host_reset_counter_init(void)
{
	g_reset_count++;
}

/* ----------------------------------------------------------------
 * Resolution de pointeur WASM -> natif
 * ---------------------------------------------------------------- */
static void *app_ptr(wasm_module_inst_t inst, uint32_t app_offset, uint32_t len)
{
	if (app_offset == 0 || len == 0) {
		return NULL;
	}
	if (wasm_runtime_validate_app_addr(inst, app_offset, len)) {
		return wasm_runtime_addr_app_to_native(inst, app_offset);
	}
	/* Repli : la valeur est peut-etre deja une adresse native dans le pool. */
	uintptr_t v = (uintptr_t)app_offset;
	if (g_pool_base &&
	    v >= (uintptr_t)g_pool_base &&
	    v + len <= (uintptr_t)g_pool_base + g_pool_size) {
		return (void *)v;
	}
	return NULL;
}

/* ================================================================
 * HOST FUNCTIONS — affichage
 * ================================================================ */
static void h_print(wasm_exec_env_t e, char *msg, uint32_t len)
{
	ARG_UNUSED(e);
	if (!msg || len == 0) {
		return;
	}
	char buf[192];
	uint32_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
	memcpy(buf, msg, n);
	buf[n] = '\0';
	printk("%s", buf);
}

/* ================================================================
 * HOST FUNCTIONS — transport abstrait
 *
 * Ces fonctions delèguent au backend transport (Wi-Fi ou BLE) sans que le
 * WASM sache lequel est actif.
 * ================================================================ */
static int32_t h_transport_connect(wasm_exec_env_t e,
	uint32_t ip_ptr, uint32_t ip_len, uint32_t port, uint32_t timeout)
{
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
	const char *ip = (const char *)app_ptr(inst, ip_ptr, ip_len);
	/* En BLE, ip peut etre NULL/ignore : on tolere. */
	return (int32_t)transport_connect(ip ? ip : "", ip_len, port, timeout);
}

static int32_t h_transport_wait_ready(wasm_exec_env_t e, uint32_t timeout)
{
	ARG_UNUSED(e);
	return (int32_t)transport_wait_ready(timeout);
}

static int32_t h_transport_send(wasm_exec_env_t e, int32_t handle,
				uint32_t buf_ptr, uint32_t buf_len)
{
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
	const uint8_t *buf = (const uint8_t *)app_ptr(inst, buf_ptr, buf_len);
	if (!buf) {
		return -1;
	}
	return (int32_t)transport_send(handle, buf, buf_len);
}

static int32_t h_transport_recv(wasm_exec_env_t e, int32_t handle,
				uint32_t buf_ptr, uint32_t buf_len)
{
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
	uint8_t *buf = (uint8_t *)app_ptr(inst, buf_ptr, buf_len);
	if (!buf) {
		return -1;
	}
	return (int32_t)transport_recv(handle, buf, buf_len);
}

static void h_transport_close(wasm_exec_env_t e, int32_t handle)
{
	ARG_UNUSED(e);
	transport_close(handle);
}

static void h_sleep(wasm_exec_env_t e, uint32_t secs)
{
	ARG_UNUSED(e);
	if (secs > 0) {
		k_sleep(K_SECONDS(secs));
	}
}

/* ================================================================
 * HOST FUNCTIONS — metriques BRUTES (aucun calcul derive ici)
 * ================================================================ */

/* M1 — CPU usage (%) */
static uint32_t h_cpu_usage(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
#ifdef CONFIG_THREAD_RUNTIME_STATS
	struct k_thread_runtime_stats a = {0}, b = {0};
	k_thread_runtime_stats_all_get(&a);
	k_msleep(100);
	k_thread_runtime_stats_all_get(&b);
	uint64_t total = b.execution_cycles - a.execution_cycles;
	uint64_t idle = b.idle_cycles - a.idle_cycles;
	if (total == 0) {
		return 0;
	}
	uint64_t active = total > idle ? total - idle : 0;
	return (uint32_t)((active * 100ULL) / total);
#else
	return 0;
#endif
}

/* M2 — Free heap (octets) */
static uint32_t h_free_heap(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return (uint32_t)CONFIG_HEAP_MEM_POOL_SIZE;
}

/* M3 — Uptime (ms) */
static uint32_t h_uptime_ms(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return (uint32_t)(k_uptime_get() & 0xFFFFFFFFULL);
}

/* M4/M5/M6 — compteurs transport (communs Wi-Fi/BLE) */
static uint32_t h_bytes_tx(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return g_tx_counters.bytes_tx;
}
static uint32_t h_bytes_rx(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return g_tx_counters.bytes_rx;
}
static uint32_t h_transport_errors(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return g_tx_counters.errors;
}

/* M7 — Stack usage (%) : proxy via charge du thread courant */
static uint32_t h_stack_usage_pct(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
#ifdef CONFIG_THREAD_RUNTIME_STATS
	struct k_thread_runtime_stats t = {0}, a = {0}, b = {0};
	k_thread_runtime_stats_all_get(&a);
	k_msleep(100);
	k_thread_runtime_stats_all_get(&b);
	k_thread_runtime_stats_get(k_current_get(), &t);
	uint64_t total = b.execution_cycles - a.execution_cycles;
	if (total == 0) {
		return 0;
	}
	uint64_t tc = t.execution_cycles;
	if (tc > total) {
		tc = total;
	}
	return (uint32_t)((tc * 100ULL) / total);
#else
	return 0;
#endif
}

/* M9 — signal (dBm), Wi-Fi ou BLE selon le backend */
static int32_t h_signal_dbm(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return transport_signal_dbm();
}

/* M10 — reset count */
static uint32_t h_reset_count(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return g_reset_count;
}

/* ================================================================
 * HOST FUNCTIONS — identite (resolue a l'execution)
 * ================================================================ */
static int32_t copy_id(char *buf, uint32_t cap, const char *val)
{
	if (!buf) {
		return -1;
	}
	size_t len = strlen(val);
	if (cap < len) {
		return -1;
	}
	memcpy(buf, val, len);
	return (int32_t)len;
}

static int32_t h_get_device_name(wasm_exec_env_t e, char *buf, uint32_t cap)
{
	ARG_UNUSED(e);
	return copy_id(buf, cap, CONFIG_WAMR_NODE_DEVICE_NAME);
}
static int32_t h_get_device_type(wasm_exec_env_t e, char *buf, uint32_t cap)
{
	ARG_UNUSED(e);
	return copy_id(buf, cap, CONFIG_WAMR_NODE_DEVICE_TYPE);
}
static int32_t h_get_os_name(wasm_exec_env_t e, char *buf, uint32_t cap)
{
	ARG_UNUSED(e);
	return copy_id(buf, cap, CONFIG_WAMR_NODE_OS_NAME);
}
static int32_t h_get_transport_name(wasm_exec_env_t e, char *buf, uint32_t cap)
{
	ARG_UNUSED(e);
	return copy_id(buf, cap, transport_name());
}

/* ================================================================
 * TABLE DES SYMBOLES NATIFS
 *
 * IDENTIQUE (noms + signatures) au contrat du module .wasm.
 * ================================================================ */
static NativeSymbol native_symbols[] = {
	{ "host_print",                 h_print,                 "(*~)",    NULL },

	{ "host_transport_connect",     h_transport_connect,     "(iiii)i", NULL },
	{ "host_transport_wait_ready",  h_transport_wait_ready,  "(i)i",    NULL },
	{ "host_transport_send",        h_transport_send,        "(iii)i",  NULL },
	{ "host_transport_recv",        h_transport_recv,        "(iii)i",  NULL },
	{ "host_transport_close",       h_transport_close,       "(i)",     NULL },
	{ "host_sleep",                 h_sleep,                 "(i)",     NULL },

	{ "host_metric_cpu_usage",         h_cpu_usage,         "()i", NULL },
	{ "host_metric_free_heap",         h_free_heap,         "()i", NULL },
	{ "host_metric_uptime_ms",         h_uptime_ms,         "()i", NULL },
	{ "host_metric_bytes_tx",          h_bytes_tx,          "()i", NULL },
	{ "host_metric_bytes_rx",          h_bytes_rx,          "()i", NULL },
	{ "host_metric_transport_errors",  h_transport_errors,  "()i", NULL },
	{ "host_metric_stack_usage_pct",   h_stack_usage_pct,   "()i", NULL },
	{ "host_metric_signal_dbm",        h_signal_dbm,        "()i", NULL },
	{ "host_metric_reset_count",       h_reset_count,       "()i", NULL },

	{ "host_get_device_name",       h_get_device_name,       "(*~)i", NULL },
	{ "host_get_device_type",       h_get_device_type,       "(*~)i", NULL },
	{ "host_get_os_name",           h_get_os_name,           "(*~)i", NULL },
	{ "host_get_transport_name",    h_get_transport_name,    "(*~)i", NULL },
};

bool host_register_natives(void)
{
	uint32_t n = (uint32_t)(sizeof(native_symbols) / sizeof(native_symbols[0]));
	if (!wasm_runtime_register_natives("env", native_symbols, n)) {
		printk("[wamr] echec d'enregistrement des symboles natifs\n");
		return false;
	}
	printk("[wamr] %u symboles natifs enregistres\n", n);
	return true;
}
