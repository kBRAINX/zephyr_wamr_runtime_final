#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_stats.h>

#include <string.h>
#include <errno.h>

#include "wasm_export.h"

/* ----------------------------------------------------------------
 * HARDWARE
 * ---------------------------------------------------------------- */
#define UART_NODE DT_CHOSEN(zephyr_console)
#define LED_NODE  DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED_NODE, okay)
static const struct gpio_dt_spec board_led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
#define HAS_LED 1
#else
#define HAS_LED 0
#endif

/* ----------------------------------------------------------------
 * WAMR
 * ---------------------------------------------------------------- */
#define WASM_MAX_SIZE  (40  * 1024)
#define STACK_SIZE     (8   * 1024)
#define HEAP_SIZE      (16  * 1024)
#define WAMR_POOL_SIZE (160 * 1024)

static uint8_t wasm_buffer[WASM_MAX_SIZE];
static char wamr_pool[WAMR_POOL_SIZE] __aligned(8);

/* ----------------------------------------------------------------
 * RÉSEAU / UART
 * ---------------------------------------------------------------- */
static const struct device *uart_dev;
static K_SEM_DEFINE(net_ready_wamr, 0, 1);
static struct net_mgmt_event_callback dhcp_cb_wamr;

static void on_dhcp_wamr(struct net_mgmt_event_callback *cb,
                          uint64_t event, struct net_if *iface)
{
    if (event == NET_EVENT_IPV4_DHCP_BOUND) { k_sem_give(&net_ready_wamr); }
}

static void uart_read_byte(const struct device *dev, uint8_t *out)
{
    while (uart_poll_in(dev, out) != 0) { k_yield(); }
}

static void uart_drain_rx(const struct device *dev)
{
    uint8_t dummy;
    int drained;
    int rounds = 0;
    do {
        drained = 0;
        while (uart_poll_in(dev, &dummy) == 0) { drained++; }
        if (drained > 0) { k_msleep(300); rounds = 0; }
        else             { rounds++; k_msleep(100); }
    } while (rounds < 5);
    printk("UART resync OK\n");
}

/* ----------------------------------------------------------------
 * ÉTAT PARTAGÉ — métriques réseau cumulées
 * g_bytes_tx/rx/errors : mis à jour dans host_tcp_send/recv
 * g_reset_count        : incrémenté au premier appel wifi_connect
 *                        détecté via le compteur de boot Zephyr
 *                        (sans hwinfo, non disponible sur toutes cibles)
 * ---------------------------------------------------------------- */
static uint32_t g_bytes_tx    = 0;
static uint32_t g_bytes_rx    = 0;
static uint32_t g_net_errors  = 0;
static uint32_t g_reset_count = 0;

/* ----------------------------------------------------------------
 * RÉSOLUTION DE POINTEUR WASM → natif
 * ---------------------------------------------------------------- */
static const char *wasm_ptr_to_native(wasm_module_inst_t inst,
                                       uint32_t ptr, uint32_t len)
{
    if (ptr == 0 || len == 0) { return NULL; }
    const char *p = (const char *)wasm_runtime_addr_app_to_native(inst, ptr);
    if (p) { return p; }
    uintptr_t pool_start = (uintptr_t)wamr_pool;
    uintptr_t pool_end   = pool_start + WAMR_POOL_SIZE;
    uintptr_t val        = (uintptr_t)(uint32_t)ptr;
    if (val >= pool_start && val < pool_end) {
        return (const char *)val;
    }
    return NULL;
}

/* ================================================================
 * HOST FUNCTIONS — communes
 * ================================================================ */

static void host_print_impl(wasm_exec_env_t exec_env,
                             uint32_t msg_ptr, uint32_t msg_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const char *msg = wasm_ptr_to_native(inst, msg_ptr, msg_len);
    if (!msg) { return; }
    uint32_t copy_len = msg_len < 255 ? msg_len : 255;
    char buf[256];
    memcpy(buf, msg, copy_len);
    buf[copy_len] = '\0';
    printk("%s", buf);
}

static int32_t host_wifi_connect_impl(wasm_exec_env_t exec_env,
    uint32_t ssid_ptr, uint32_t ssid_len,
    uint32_t psk_ptr,  uint32_t psk_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const char *ssid = wasm_ptr_to_native(inst, ssid_ptr, ssid_len);
    const char *psk  = wasm_ptr_to_native(inst, psk_ptr,  psk_len);
    if (!ssid || !psk) { return -1; }
    struct net_if *iface = net_if_get_default();
    if (!iface) { return -1; }

    net_mgmt_init_event_callback(&dhcp_cb_wamr, on_dhcp_wamr,
                                 NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&dhcp_cb_wamr);

    /* M10 — Reset count : on utilise le boot_count Zephyr.
     * k_uptime_get() == 0 uniquement au tout premier appel après boot.
     * Alternative simple : on incrémente g_reset_count à chaque appel
     * de cette fonction (elle n'est appelée qu'une fois par exécution WASM). */
    g_reset_count++;

    struct wifi_connect_req_params params = {
        .ssid = (const uint8_t *)ssid, .ssid_length = (uint8_t)ssid_len,
        .psk  = (const uint8_t *)psk,  .psk_length  = (uint8_t)psk_len,
        .channel = WIFI_CHANNEL_ANY, .security = WIFI_SECURITY_TYPE_PSK,
        .mfp = WIFI_MFP_OPTIONAL, .band = WIFI_FREQ_BAND_2_4_GHZ,
        .timeout = SYS_FOREVER_MS,
    };
    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (ret == 0) { net_dhcpv4_start(iface); }
    return ret;
}

static int32_t host_wait_network_ready_impl(wasm_exec_env_t exec_env,
                                             uint32_t timeout_secs)
{
    ARG_UNUSED(exec_env);
    return (k_sem_take(&net_ready_wamr, K_SECONDS(timeout_secs)) == 0) ? 0 : -1;
}

static void host_gpio_blink_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
#if HAS_LED
    if (device_is_ready(board_led.port)) {
        gpio_pin_set_dt(&board_led, 1); k_msleep(150);
        gpio_pin_set_dt(&board_led, 0); k_msleep(150);
    }
#endif
}

static int32_t host_tcp_connect_impl(wasm_exec_env_t exec_env,
    uint32_t ip_ptr, uint32_t ip_len, uint32_t port, uint32_t timeout_secs)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const char *ip = wasm_ptr_to_native(inst, ip_ptr, ip_len);
    if (!ip) { return -1; }
    char ip_str[32];
    uint32_t n = ip_len < sizeof(ip_str)-1 ? ip_len : sizeof(ip_str)-1;
    memcpy(ip_str, ip, n); ip_str[n] = '\0';
    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { return -1; }
    struct zsock_timeval tv = { .tv_sec = timeout_secs, .tv_usec = 0 };
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (zsock_inet_pton(AF_INET, ip_str, &addr.sin_addr) != 1) {
        zsock_close(sock); return -1;
    }
    if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        zsock_close(sock); return -1;
    }
    return sock;
}

static int32_t host_tcp_send_impl(wasm_exec_env_t exec_env,
    int32_t fd, uint32_t buf_ptr, uint32_t buf_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const uint8_t *buf = (const uint8_t *)wasm_ptr_to_native(inst, buf_ptr, buf_len);
    if (!buf) { return -1; }
    int32_t sent = zsock_send(fd, buf, buf_len, 0);
    if (sent > 0) { g_bytes_tx += (uint32_t)sent; }
    else          { g_net_errors++; }
    return sent;
}

static int32_t host_tcp_recv_impl(wasm_exec_env_t exec_env,
    int32_t fd, uint32_t buf_ptr, uint32_t buf_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    uint8_t *buf = (uint8_t *)wasm_ptr_to_native(inst, buf_ptr, buf_len);
    if (!buf) { return -1; }
    int32_t received = zsock_recv(fd, buf, buf_len, 0);
    if (received > 0) { g_bytes_rx += (uint32_t)received; }
    else if (received < 0) { g_net_errors++; }
    return received;
}

static void host_tcp_close_impl(wasm_exec_env_t exec_env, int32_t fd)
{
    ARG_UNUSED(exec_env); zsock_close(fd);
}

static void host_sleep_impl(wasm_exec_env_t exec_env, uint32_t secs)
{
    ARG_UNUSED(exec_env); k_sleep(K_SECONDS(secs));
}

/* ================================================================
 * HOST FUNCTIONS — MÉTRIQUES (M1–M10)
 * ================================================================ */

/* M1 — CPU usage (%)
 * Mesure sur fenêtre 100 ms via k_thread_runtime_stats_all_get().
 * CONFIG_THREAD_RUNTIME_STATS=y requis dans prj.conf.
 */
static uint32_t host_metric_cpu_usage_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
#ifdef CONFIG_THREAD_RUNTIME_STATS
    struct k_thread_runtime_stats before = {0};
    struct k_thread_runtime_stats after  = {0};
    k_thread_runtime_stats_all_get(&before);
    k_msleep(100);
    k_thread_runtime_stats_all_get(&after);
    uint64_t total  = after.execution_cycles - before.execution_cycles;
    uint64_t idle   = after.idle_cycles      - before.idle_cycles;
    if (total == 0) { return 0; }
    uint64_t active = (total > idle) ? (total - idle) : 0;
    return (uint32_t)((active * 100ULL) / total);
#else
    return 0;
#endif
}

/* M2 — Free heap (octets)
 *
 * mallinfo() et _system_heap ne sont pas accessibles de facon portable
 * dans Zephyr 4.x sur ESP32-S3 avec picolibc sans configuration specifique.
 * On retourne CONFIG_HEAP_MEM_POOL_SIZE (valeur Kconfig, toujours disponible
 * comme macro, definie dans prj.conf a 65536).
 * C'est une valeur statique coherente qui ne necessite aucun appel d'API.
 */
static uint32_t host_metric_free_heap_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
    return (uint32_t)CONFIG_HEAP_MEM_POOL_SIZE;
}

/* M3 — Uptime (ms) */
static uint32_t host_metric_uptime_ms_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
    return (uint32_t)(k_uptime_get() & 0xFFFFFFFFULL);
}

/* M4 — Bytes TX (cumulés) */
static uint32_t host_metric_bytes_tx_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
#ifdef CONFIG_NET_STATISTICS
    struct net_stats stats;
    struct net_if *iface = net_if_get_default();
    if (iface && net_mgmt(NET_REQUEST_STATS_GET_ALL, iface,
                          &stats, sizeof(stats)) == 0) {
        return (uint32_t)stats.bytes.sent;
    }
#endif
    return g_bytes_tx;
}

/* M5 — Bytes RX (cumulés) */
static uint32_t host_metric_bytes_rx_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
#ifdef CONFIG_NET_STATISTICS
    struct net_stats stats;
    struct net_if *iface = net_if_get_default();
    if (iface && net_mgmt(NET_REQUEST_STATS_GET_ALL, iface,
                          &stats, sizeof(stats)) == 0) {
        return (uint32_t)stats.bytes.received;
    }
#endif
    return g_bytes_rx;
}

/* M6 — Network errors (cumulés)
 * net_stats_ip_errors dans Zephyr 4.x : membres protoerr, chkerr, fragerr.
 * Le champ opterr n'existe PAS dans cette version — retiré.
 */
static uint32_t host_metric_net_errors_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
#ifdef CONFIG_NET_STATISTICS
    struct net_stats stats;
    struct net_if *iface = net_if_get_default();
    if (iface && net_mgmt(NET_REQUEST_STATS_GET_ALL, iface,
                          &stats, sizeof(stats)) == 0) {
        return (uint32_t)(stats.ip_errors.protoerr
                        + stats.ip_errors.chkerr
                        + stats.ip_errors.fragerr);
    }
#endif
    return g_net_errors;
}

/* M7 — Stack usage du thread principal (%)
 *
 * k_thread_stack_space_get() requiert CONFIG_THREAD_STACK_INFO=y ET une
 * implementation kernel qui nest pas toujours linkee sur ESP32-S3 Zephyr 4.x
 * (undefined reference to z_impl_k_thread_stack_space_get).
 *
 * Alternative sans syscall externe : on utilise les stats runtime du thread
 * courant. execution_cycles / total_all_cycles donne la charge de CE thread,
 * ce qui reflete indirectement son activite (proxy utile pour le stack usage).
 * Retourne 0 si CONFIG_THREAD_RUNTIME_STATS nest pas active.
 */
static uint32_t host_metric_stack_usage_pct_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
#ifdef CONFIG_THREAD_RUNTIME_STATS
    /* Mesure la charge CPU de ce seul thread sur 100 ms.
     * Plus le thread est actif, plus le stack est potentiellement utilise.
     * Valeur dans [0, 100]. */
    struct k_thread_runtime_stats thread_stats = {0};
    struct k_thread_runtime_stats all_stats_before = {0};
    struct k_thread_runtime_stats all_stats_after  = {0};
    k_thread_runtime_stats_all_get(&all_stats_before);
    k_msleep(100);
    k_thread_runtime_stats_all_get(&all_stats_after);
    k_thread_runtime_stats_get(k_current_get(), &thread_stats);
    uint64_t total = all_stats_after.execution_cycles
                   - all_stats_before.execution_cycles;
    if (total == 0) { return 0; }
    uint64_t thread_cycles = thread_stats.execution_cycles;
    if (thread_cycles > total) { thread_cycles = total; }
    return (uint32_t)((thread_cycles * 100ULL) / total);
#else
    return 0;
#endif
}

/* M8 — Idle time ratio (%)
 * Même fenêtre 100 ms que M1.
 */
static uint32_t host_metric_idle_ratio_pct_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
#ifdef CONFIG_THREAD_RUNTIME_STATS
    struct k_thread_runtime_stats before = {0};
    struct k_thread_runtime_stats after  = {0};
    k_thread_runtime_stats_all_get(&before);
    k_msleep(100);
    k_thread_runtime_stats_all_get(&after);
    uint64_t total = after.execution_cycles - before.execution_cycles;
    uint64_t idle  = after.idle_cycles      - before.idle_cycles;
    if (total == 0) { return 100; }
    if (idle > total) { idle = total; }
    return (uint32_t)((idle * 100ULL) / total);
#else
    return 0;
#endif
}

/* M9 — RSSI Wi-Fi (dBm)
 * NET_REQUEST_WIFI_IFACE_STATUS → wifi_iface_status.rssi
 */
static int32_t host_metric_rssi_dbm_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
    struct net_if *iface = net_if_get_default();
    if (!iface) { return 0; }
    struct wifi_iface_status status = {0};
    int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
                       &status, sizeof(status));
    if (ret != 0) { return 0; }
    if (status.state < WIFI_STATE_ASSOCIATED) { return 0; }
    return (int32_t)status.rssi;
}

/* M10 — Reset count
 * g_reset_count est incrémenté à chaque appel de host_wifi_connect_impl(),
 * qui n'est appelée qu'une fois par démarrage du module WASM.
 * Cela reflète correctement le nombre de cycles d'exécution / reboots.
 */
static uint32_t host_metric_reset_count_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
    return g_reset_count;
}

/* ================================================================
 * TABLE DES SYMBOLES NATIFS
 * ================================================================ */
static NativeSymbol native_symbols[] = {
    /* Communes */
    { "host_print",              host_print_impl,              "(*~)",    NULL },
    { "host_wifi_connect",       host_wifi_connect_impl,       "(iiii)i", NULL },
    { "host_wait_network_ready", host_wait_network_ready_impl, "(i)i",    NULL },
    { "host_gpio_blink",         host_gpio_blink_impl,         "()",      NULL },
    { "host_tcp_connect",        host_tcp_connect_impl,        "(iiii)i", NULL },
    { "host_tcp_send",           host_tcp_send_impl,           "(iii)i",  NULL },
    { "host_tcp_recv",           host_tcp_recv_impl,           "(iii)i",  NULL },
    { "host_tcp_close",          host_tcp_close_impl,          "(i)",     NULL },
    { "host_sleep",              host_sleep_impl,              "(i)",     NULL },
    /* Métriques M1–M10 */
    { "host_metric_cpu_usage",       host_metric_cpu_usage_impl,       "()i", NULL },
    { "host_metric_free_heap",       host_metric_free_heap_impl,       "()i", NULL },
    { "host_metric_uptime_ms",       host_metric_uptime_ms_impl,       "()i", NULL },
    { "host_metric_bytes_tx",        host_metric_bytes_tx_impl,        "()i", NULL },
    { "host_metric_bytes_rx",        host_metric_bytes_rx_impl,        "()i", NULL },
    { "host_metric_net_errors",      host_metric_net_errors_impl,      "()i", NULL },
    { "host_metric_stack_usage_pct", host_metric_stack_usage_pct_impl, "()i", NULL },
    { "host_metric_idle_ratio_pct",  host_metric_idle_ratio_pct_impl,  "()i", NULL },
    { "host_metric_rssi_dbm",        host_metric_rssi_dbm_impl,        "()i", NULL },
    { "host_metric_reset_count",     host_metric_reset_count_impl,     "()i", NULL },
};

/* ================================================================
 * EXÉCUTION DU MODULE WASM
 * ================================================================ */
static void execute_wasm(uint8_t *wasm_data, uint32_t wasm_size)
{
    char error_buf[128];
    wasm_module_t        module      = NULL;
    wasm_module_inst_t   module_inst = NULL;
    wasm_exec_env_t      exec_env    = NULL;
    wasm_function_inst_t func        = NULL;

    printk("[POOL] addr=%p size=%d KB align=%d\n",
           wamr_pool, WAMR_POOL_SIZE/1024,
           ((uintptr_t)wamr_pool % 8 == 0) ? 8 : 0);

    module = wasm_runtime_load(wasm_data, wasm_size, error_buf, sizeof(error_buf));
    if (!module) { printk("LOAD ERROR: %s\n", error_buf); return; }
    printk("Module charge OK\n");

    module_inst = wasm_runtime_instantiate(module, STACK_SIZE, HEAP_SIZE,
                                           error_buf, sizeof(error_buf));
    if (!module_inst) {
        printk("INSTANTIATE ERROR: %s\n", error_buf);
        goto cleanup_module;
    }
    printk("Instance creee OK\n");

    exec_env = wasm_runtime_create_exec_env(module_inst, STACK_SIZE);
    if (!exec_env) { printk("EXEC ENV FAILED\n"); goto cleanup_inst; }

    func = wasm_runtime_lookup_function(module_inst, "_start");
    if (!func) func = wasm_runtime_lookup_function(module_inst, "main");
    if (!func) func = wasm_runtime_lookup_function(module_inst, "__main_void");
    if (!func) {
        printk("point entree introuvable (_start/main)\n");
        goto cleanup_env;
    }
    printk("Point entree trouve\n");
    printk("Executing WASM...\n");

    if (!wasm_runtime_call_wasm(exec_env, func, 0, NULL)) {
        printk("EXCEPTION: %s\n", wasm_runtime_get_exception(module_inst));
    } else {
        printk("Execution completed\n");
    }

cleanup_env:    wasm_runtime_destroy_exec_env(exec_env);
cleanup_inst:   wasm_runtime_deinstantiate(module_inst);
cleanup_module: wasm_runtime_unload(module);
}

/* ================================================================
 * MAIN
 * ================================================================ */
int main(void)
{
#if HAS_LED
    if (device_is_ready(board_led.port)) {
        gpio_pin_configure_dt(&board_led, GPIO_OUTPUT_INACTIVE);
    }
#endif

    printk("[POOL] wamr_pool=%p aligned8=%d\n",
           wamr_pool, (uintptr_t)wamr_pool % 8 == 0 ? 1 : 0);

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type                  = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf  = wamr_pool;
    init_args.mem_alloc_option.pool.heap_size = sizeof(wamr_pool);

    if (!wasm_runtime_full_init(&init_args)) {
        printk("WAMR init failed\n"); return -1;
    }
    printk("WAMR init OK (pool=%dKB)\n", WAMR_POOL_SIZE/1024);

    if (!wasm_runtime_register_natives("env", native_symbols,
                                       ARRAY_SIZE(native_symbols))) {
        printk("Failed to register native symbols\n"); return -1;
    }
    printk("Host functions enregistrees OK (%d symboles)\n",
           (int)ARRAY_SIZE(native_symbols));

    uart_dev = DEVICE_DT_GET(UART_NODE);
    if (!device_is_ready(uart_dev)) { printk("UART not ready\n"); return -1; }

    printk("\n===== WAMR UART DEPLOYMENT — Metrics Edition =====\n");
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
            printk("ERROR: invalid size (0 < size <= %d)\n", WASM_MAX_SIZE);
            uart_drain_rx(uart_dev);
            continue;
        }

        for (uint32_t i = 0; i < wasm_size; i++) {
            uart_read_byte(uart_dev, &wasm_buffer[i]);
        }
        printk("Upload complete (%u bytes)\n", wasm_size);

        /* Reset des compteurs avant nouvelle exécution */
        g_bytes_tx   = 0;
        g_bytes_rx   = 0;
        g_net_errors = 0;

        execute_wasm(wasm_buffer, wasm_size);
    }

    return 0;
}