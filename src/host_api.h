/*
 * src/host_api.h — Declarations de la couche hote WAMR
 *
 * Licence : Apache-2.0
 */

#ifndef WAMR_HOST_API_H
#define WAMR_HOST_API_H

#include <stdbool.h>
#include <stdint.h>

#include "wasm_export.h"

/* Enregistre la table des symboles natifs aupres de WAMR.
 * A appeler apres wasm_runtime_full_init() et avant tout chargement de
 * module. Retourne true en cas de succes.
 */
bool host_register_natives(void);

/* Renseigne les bornes du pool WAMR (resolution de pointeur en dernier
 * recours).
 */
void host_set_pool(void *pool, size_t size);

/* Compteur de redemarrages (M10). Incremente une fois au demarrage. */
void host_reset_counter_init(void);

#endif /* WAMR_HOST_API_H */
