/*
 * src/host_api.c — Couche hote WAMR, VARIANTE SIMULATION (Zephyr).
 *
 * OBJECTIF
 * --------
 * Faire tourner la VRAIE sonde .wasm (PADRE + CBOR, inchangee) sur un vrai
 * ESP32, mais en alimentant CPU et batterie avec des DONNEES SIMULEES
 * realistes, pour evaluer PADRE sans application reelle ni batterie physique :
 *
 *   - host_metric_cpu_usage  : REJOUE une trace CPU pre-generee (profil de
 *     l'equipement, calibree sur le dataset serre). C'est aussi ICI qu'on
 *     applique la decharge batterie du cycle (une lecture CPU = un cycle).
 *   - host_metric_battery_mv : BATTERIE SIMULEE en BOUCLE FERMEE. Elle NE LIT
 *     PAS l'ADC (toute logique ADC est outrepassee). Elle decroit selon
 *     E_base + E_cpu*cpu + E_tx*(octets REELLEMENT emis, lus dans
 *     g_tx_counters.bytes_tx). Le % est converti en mV pour que le cutoff PADRE
 *     (BATTERY_CUTOFF_MV) s'active et declenche le mode survie.
 *
 * Le reste (transport reseau, metriques reseau reelles : bytes_tx/rx, erreurs,
 * signal, durees) reste REEL et inchange. Le .wasm ignore que cpu/batterie sont
 * simules : la portabilite du contrat hote est preservee.
 *
 * PROFIL : choisi a la compilation par SIM_EQUIP_ID (0..4) :
 *   west build ... -- -DEXTRA_CONF_FILE=conf/wifi.conf -DSIM_EQUIP_ID=3
 * (necessite les 3 lignes CMake decrites dans le README de simulation).
 * Un firmware par equipement (flasher chaque carte avec un ID different).
 *
 * Licence : Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>

#if defined(CONFIG_WIFI) && defined(CONFIG_NET_STATISTICS)
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_stats.h>
#include <zephyr/net/net_mgmt.h>
#endif

#include "wasm_export.h"
#include "host_api.h"
#include "transport.h"

#include "traces_cpu.h"   /* tables de traces CPU (static const -> flash) */

/* ================================================================
 * SELECTION DU PROFIL D'EQUIPEMENT (a passer au build : -DSIM_EQUIP_ID=n)
 *   0 = stable          (batt0 90%)
 *   1 = montee_charge   (batt0 75%)
 *   2 = critique        (batt0 60%)
 *   3 = batterie_faible (batt0 30%)
 *   4 = nominal         (batt0 50%)
 * ================================================================ */
#ifndef SIM_EQUIP_ID
#define SIM_EQUIP_ID 0
#endif

/* Identite : type/os de l'equipement simule (fixes ici, non surchargeables). */
#define SIM_DEVICE_TYPE "esp32s3"
#define SIM_OS_NAME     "zephyr"

/* Nom d'equipement = "sim-<profil>" (ex. "sim-batterie_faible"). */
static char g_dev_name[40];

/* Pool WAMR (resolution de pointeur). */
static void *g_pool_base;
static size_t g_pool_size;
static uint32_t g_reset_count;

/* ================================================================
 * ETAT DE SIMULATION
 * ================================================================ */

/* Index courant dans la trace CPU (avance a chaque lecture CPU = chaque cycle). */
static uint32_t g_sim_idx;

/* Batterie simulee en 1e-5 % (0..10 000 000). Echelle fine -> arithmetique
 * entiere sans perte (consommations par cycle = quelques centaines d'unites). */
#define BATT_SCALE   100000               /* g_batt_hi = pct * 100000 */
static int32_t g_batt_hi;                 /* batterie, echelle 1e-5 %  */
static bool    g_batt_init;

/* Dernier compteur d'octets emis vu (pour le delta de transmission). */
static uint32_t g_last_bytes_tx;

/* --- Constantes energetiques CALIBREES (batt_constantes.json), en 1e-5 % ---
 * Modele : conso(cycle) = CONSO_BASE + CONSO_CPU*cpu + CONSO_TX_PER_OCTET*octets.
 *   base 0.0018 %/cycle           -> 180 (1e-5 %)
 *   cpu  0.00012 %/(cycle . %CPU)  -> 12  (1e-5 %) par % de CPU
 *   tx   derive de Heinzelman      -> 1   (1e-5 %) par octet emis
 * Le terme transmission est en BOUCLE FERMEE : octets REELLEMENT emis.
 * PADRE emettant moins -> batterie preservee. (Source : 3_calibrer_batterie.py) */
#define CONSO_BASE            180
#define CONSO_CPU             12
#define CONSO_TX_PER_OCTET    1

/* Conversion % simule -> mV. Mapping [3000..4200] : le cutoff PADRE
 * (BATTERY_CUTOFF_MV = 3300 mV) est franchi vers ~25 %, ce qui declenche le
 * mode survie AVANT que la batterie soit vide. */
#define SIM_MV_LOW   3000
#define SIM_MV_FULL  4200

void host_set_pool(void *pool, size_t size)
{
	g_pool_base = pool;
	g_pool_size = size;
}

void host_reset_counter_init(void)
{
	g_reset_count++;

	/* Init du nom d'equipement et de la batterie de depart. */
	const char *profil = sim_noms[SIM_EQUIP_ID];
	snprintf(g_dev_name, sizeof(g_dev_name), "sim-%s", profil);

	g_batt_hi = (int32_t)sim_batt0_pct[SIM_EQUIP_ID] * BATT_SCALE;
	g_batt_init = true;
	g_sim_idx = 0;
	g_last_bytes_tx = 0;

	printk("[sim] equipement %d = %s, batterie initiale %u%%\n",
	       SIM_EQUIP_ID, g_dev_name, sim_batt0_pct[SIM_EQUIP_ID]);
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
 * HOST FUNCTIONS — transport abstrait (REEL, inchange)
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
 * HOT-UPDATE : desactive en simulation (pas de mise a jour pendant un test).
 * ================================================================ */
static int32_t h_poll_update(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return 0;
}
static uint32_t h_update_count(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return 0;
}

/* ================================================================
 * HOST FUNCTIONS — METRIQUES SIMULEES (CPU + batterie)
 * ================================================================ */

/* M1 — CPU : REJOUE la trace du profil. Avance l'index a chaque appel, et
 * applique la decharge batterie du cycle (boucle fermee).
 * En fin de trace (simulation > duree generee), maintient la derniere valeur. */
static uint32_t h_cpu_usage(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	uint32_t idx = g_sim_idx;
	if (idx >= SIM_N_POINTS) {
		idx = SIM_N_POINTS - 1;
	}
	uint8_t cpu = sim_cpu_traces[SIM_EQUIP_ID][idx];

	/* Decharge batterie de CE cycle (h_cpu_usage appele 1x/cycle). */
	{
		uint32_t tx_now = g_tx_counters.bytes_tx;
		uint32_t d_octets = tx_now - g_last_bytes_tx;
		g_last_bytes_tx = tx_now;

		int32_t conso = CONSO_BASE
			      + (int32_t)CONSO_CPU * (int32_t)cpu
			      + (int32_t)CONSO_TX_PER_OCTET * (int32_t)d_octets;
		g_batt_hi -= conso;
		if (g_batt_hi < 0) {
			g_batt_hi = 0;
		}
	}

	g_sim_idx++;
	return cpu;
}

/* M13 — batterie SIMULEE (mV). Outrepasse l'ADC : renvoie la batterie du
 * modele en boucle fermee, convertie en mV pour le cutoff PADRE. */
static uint32_t h_battery_mv(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	if (!g_batt_init) {
		return SIM_MV_FULL;
	}
	int32_t hi = g_batt_hi;                          /* 0..10 000 000 (1e-5 %) */
	if (hi > 100 * BATT_SCALE) hi = 100 * BATT_SCALE;
	if (hi < 0) hi = 0;

	int32_t mv = SIM_MV_LOW +
		     (int32_t)((int64_t)(SIM_MV_FULL - SIM_MV_LOW) * hi
			       / (100 * BATT_SCALE));
	return (uint32_t)mv;
}

/* ================================================================
 * HOST FUNCTIONS — metriques REELLES (inchangees)
 * ================================================================ */

static uint32_t h_free_heap(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return (uint32_t)CONFIG_HEAP_MEM_POOL_SIZE;
}
static uint32_t h_uptime_ms(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return (uint32_t)(k_uptime_get() & 0xFFFFFFFFULL);
}
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
	return (uint32_t)(((total - unused) * 100U) / total);
#else
	return 0;
#endif
}
static int32_t h_signal_dbm(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return transport_signal_dbm();
}
static uint32_t h_reset_count(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return g_reset_count;
}

/* M11 — threads actifs.
 * CORRECTION : en simulation, on renvoie une valeur fixe. On N'APPELLE PAS
 * k_thread_foreach avec un callback nul (cela provoquait un saut vers l'adresse
 * 0 -> "Illegal instruction, mepc: 0"). Cette metrique n'influence pas PADRE. */
static uint32_t h_active_threads(wasm_exec_env_t e)
{
	ARG_UNUSED(e);
	return 1;
}

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

/* ================================================================
 * HOST FUNCTIONS — identite (equipement simule distinct)
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
	return copy_id(buf, cap, g_dev_name);
}
static int32_t h_get_device_type(wasm_exec_env_t e, char *buf, uint32_t cap)
{
	ARG_UNUSED(e);
	return copy_id(buf, cap, SIM_DEVICE_TYPE);
}
static int32_t h_get_os_name(wasm_exec_env_t e, char *buf, uint32_t cap)
{
	ARG_UNUSED(e);
	return copy_id(buf, cap, SIM_OS_NAME);
}
static int32_t h_get_transport_name(wasm_exec_env_t e, char *buf, uint32_t cap)
{
	ARG_UNUSED(e);
	return copy_id(buf, cap, transport_name());
}

/* ================================================================
 * TABLE DES SYMBOLES NATIFS
 * IDENTIQUE (noms + signatures) au contrat du module .wasm (ffi.rs).
 * L'ORDRE et les SIGNATURES doivent correspondre exactement, sinon un appel
 * part vers une adresse invalide (crash "Illegal instruction, mepc: 0").
 * ================================================================ */
static NativeSymbol native_symbols[] = {
	{ "host_print",                 h_print,                 "(*~)",    NULL },

	{ "host_transport_connect",     h_transport_connect,     "(iiii)i", NULL },
	{ "host_transport_wait_ready",  h_transport_wait_ready,  "(i)i",    NULL },
	{ "host_transport_send",        h_transport_send,        "(iii)i",  NULL },
	{ "host_transport_recv",        h_transport_recv,        "(iii)i",  NULL },
	{ "host_transport_close",       h_transport_close,       "(i)",     NULL },
	{ "host_sleep",                 h_sleep,                 "(i)",     NULL },

	{ "host_poll_update",           h_poll_update,           "()i", NULL },
	{ "host_metric_update_count",   h_update_count,          "()i", NULL },

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
	printk("[wamr] %u symboles natifs enregistres (SIMULATION equip %d)\n",
	       n, SIM_EQUIP_ID);
	return true;
}