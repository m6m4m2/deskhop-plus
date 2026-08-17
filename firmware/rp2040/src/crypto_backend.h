/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DHP_CRYPTO_BACKEND_H
#define DHP_CRYPTO_BACKEND_H

#include "dhp/auth.h"

void crypto_backend_init(void);

/* The primitive set handed to dhp_pair_init(). */
const dhp_crypto_t *crypto_backend(void);

#endif /* DHP_CRYPTO_BACKEND_H */
