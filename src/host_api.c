/*
 * src/host_api.c — Couche hote WAMR (transport abstrait + metriques + identite)
 *
 * Licence : Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_STATISTICS)
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_stats.h>
#include <zephyr/net/net_mgmt.h>
#endif

#ifdef CONFIG_WAMR_BATTERY_ADC
#include <zephyr/drivers/adc.h>
#endif

#include "wasm_export.h"
#include "host_api.h"
#include "transport.h"

/* ----------------------------------------------------------------
 * Identite du noeud — surchargeable via Kconfig / build flags.
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

/* Compteur de redemarrages (M10). */
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
 * ================================================================ */
static int32_t h_transport_connect(wasm_exec_env_t e,
	uint32_t ip_ptr, uint32_t ip_len, uint32_t port, uint32_t timeout)
{
	wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
	const char *ip = (const char *)app_ptr(inst, ip_ptr, ip_len);
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

/* M7 — occupation reelle de la pile du thread courant (%) */
static uint32_t h_stack_usage_pct(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	struct k_thread *self = k_current_get();
	size_t unused = 0;
	if (k_thread_stack_space_get(self, &unused) != 0) {
		return 0;
	}
	size_t total = self->stack_info.size;
	if (total == 0 || unused > total) {
		return 0;
	}
	size_t used = total - unused;
	return (uint32_t)((used * 100U) / total);
#else
	return 0;
#endif
}

/* M9 — signal (dBm) */
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

/* M11 — active threads */
#ifdef CONFIG_THREAD_MONITOR
static void count_thread_cb(const struct k_thread *thread, void *user_data)
{
	ARG_UNUSED(thread);
	uint32_t *n = (uint32_t *)user_data;
	(*n)++;
}
#endif

static uint32_t h_active_threads(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
#ifdef CONFIG_THREAD_MONITOR
	uint32_t n = 0;
	k_thread_foreach(count_thread_cb, &n);
	return n;
#else
	return 1;
#endif
}

/* M12 — retransmissions du lien (source de "coap_retransmissions" cote WASM).
 * Sous Wi-Fi/TCP : compteur de segments retransmis. 0 en BLE (pas de TCP).
 */
static uint32_t h_tcp_retransmissions(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
#if defined(CONFIG_NET_STATISTICS) && defined(CONFIG_NET_STATISTICS_TCP) && defined(CONFIG_WIFI)
	struct net_stats stats;
	struct net_if *iface = net_if_get_default();
	if (iface && net_mgmt(NET_REQUEST_STATS_GET_ALL, iface,
			      &stats, sizeof(stats)) == 0) {
		return (uint32_t)stats.tcp.rexmit;
	}
#endif
	return 0;
}

/* M13 — tension batterie (mV). NOUVEAU.
 * Renvoie 0 si la mesure batterie n'est pas configuree (ex. alimentation USB).
 * Cote WASM, 0 desactive la regulation energetique (pas de mode survie force).
 *
 * Pour activer : CONFIG_WAMR_BATTERY_ADC=y ET fournir un overlay carte
 * definissant le canal ADC dans le noeud "zephyr,user", par exemple :
 *   / { zephyr,user { io-channels = <&adc 0>; }; };
 * Adapter le facteur de pont diviseur si necessaire (ex. x2 sur Heltec).
 */
#ifdef CONFIG_WAMR_BATTERY_ADC
static const struct adc_dt_spec batt_adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
#endif

static uint32_t h_battery_mv(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
#ifdef CONFIG_WAMR_BATTERY_ADC
	if (!adc_is_ready_dt(&batt_adc)) {
		return 0;
	}
	(void)adc_channel_setup_dt(&batt_adc);

	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	if (adc_sequence_init_dt(&batt_adc, &seq) != 0) {
		return 0;
	}
	if (adc_read_dt(&batt_adc, &seq) != 0) {
		return 0;
	}
	int32_t mv = raw;
	if (adc_raw_to_millivolts_dt(&batt_adc, &mv) != 0) {
		return 0;
	}
	/* Si un pont diviseur divise la tension batterie, multiplier ici. */
	return (uint32_t)(mv < 0 ? 0 : mv);
#else
	return 0;
#endif
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
 * IDENTIQUE (noms + signatures) au contrat du module .wasm (ffi.rs).
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
	{ "host_metric_active_threads",       h_active_threads,       "()i", NULL },
	{ "host_metric_tcp_retransmissions",  h_tcp_retransmissions,  "()i", NULL },
	{ "host_metric_battery_mv",           h_battery_mv,           "()i", NULL },

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