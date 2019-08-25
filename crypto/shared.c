/*
 * ngtcp2
 *
 * Copyright (c) 2019 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <ngtcp2/ngtcp2_crypto.h>

#include <string.h>

#include "ngtcp2_macro.h"

int ngtcp2_crypto_hkdf_expand_label(uint8_t *dest, size_t destlen,
                                    ngtcp2_crypto_md *md, const uint8_t *secret,
                                    size_t secretlen, const uint8_t *label,
                                    size_t labellen) {
  static const uint8_t LABEL[] = "tls13 ";
  uint8_t info[256];
  uint8_t *p = info;

  *p++ = (uint8_t)(destlen / 256);
  *p++ = (uint8_t)(destlen % 256);
  *p++ = (uint8_t)(sizeof(LABEL) - 1 + labellen);
  memcpy(p, LABEL, sizeof(LABEL) - 1);
  p += sizeof(LABEL) - 1;
  memcpy(p, label, labellen);
  p += labellen;
  *p++ = 0;

  return ngtcp2_crypto_hkdf_expand(dest, destlen, md, secret, secretlen, info,
                                   (size_t)(p - info));
}

#define NGTCP2_CRYPTO_INITIAL_SECRETLEN 32

int ngtcp2_crypto_derive_initial_secrets(uint8_t *rx_secret, uint8_t *tx_secret,
                                         uint8_t *initial_secret,
                                         const ngtcp2_cid *client_dcid,
                                         ngtcp2_crypto_side side) {
  static const uint8_t CLABEL[] = "client in";
  static const uint8_t SLABEL[] = "server in";
  uint8_t initial_secret_buf[NGTCP2_CRYPTO_INITIAL_SECRETLEN];
  uint8_t *initial_secret_ptr =
      initial_secret ? initial_secret : initial_secret_buf;
  uint8_t *client_secret;
  uint8_t *server_secret;
  ngtcp2_crypto_ctx ctx;

  ngtcp2_crypto_ctx_initial(&ctx);

  if (ngtcp2_crypto_hkdf_extract(initial_secret_ptr,
                                 NGTCP2_CRYPTO_INITIAL_SECRETLEN, &ctx.md,
                                 client_dcid->data, client_dcid->datalen,
                                 (const uint8_t *)NGTCP2_INITIAL_SALT,
                                 sizeof(NGTCP2_INITIAL_SALT) - 1) != 0) {
    return -1;
  }

  if (side == NGTCP2_CRYPTO_SIDE_SERVER) {
    client_secret = rx_secret;
    server_secret = tx_secret;
  } else {
    client_secret = tx_secret;
    server_secret = rx_secret;
  }

  if (ngtcp2_crypto_hkdf_expand_label(
          client_secret, NGTCP2_CRYPTO_INITIAL_SECRETLEN, &ctx.md,
          initial_secret_ptr, NGTCP2_CRYPTO_INITIAL_SECRETLEN, CLABEL,
          sizeof(CLABEL) - 1) != 0 ||
      ngtcp2_crypto_hkdf_expand_label(
          server_secret, NGTCP2_CRYPTO_INITIAL_SECRETLEN, &ctx.md,
          initial_secret_ptr, NGTCP2_CRYPTO_INITIAL_SECRETLEN, SLABEL,
          sizeof(SLABEL) - 1) != 0) {
    return -1;
  }

  return 0;
}

size_t ngtcp2_crypto_packet_protection_ivlen(ngtcp2_crypto_aead *aead) {
  size_t noncelen = ngtcp2_crypto_aead_noncelen(aead);
  return ngtcp2_max(8, noncelen);
}

int ngtcp2_crypto_derive_packet_protection_key(
    uint8_t *key, uint8_t *iv, uint8_t *hp, ngtcp2_crypto_aead *aead,
    ngtcp2_crypto_md *md, const uint8_t *secret, size_t secretlen) {
  static const uint8_t KEY_LABEL[] = "quic key";
  static const uint8_t IV_LABEL[] = "quic iv";
  static const uint8_t HP_LABEL[] = "quic hp";
  size_t keylen = ngtcp2_crypto_aead_keylen(aead);
  size_t ivlen = ngtcp2_crypto_packet_protection_ivlen(aead);

  if (ngtcp2_crypto_hkdf_expand_label(key, keylen, md, secret, secretlen,
                                      KEY_LABEL, sizeof(KEY_LABEL) - 1) != 0) {
    return -1;
  }

  if (ngtcp2_crypto_hkdf_expand_label(iv, ivlen, md, secret, secretlen,
                                      IV_LABEL, sizeof(IV_LABEL) - 1) != 0) {
    return -1;
  }

  if (hp != NULL &&
      ngtcp2_crypto_hkdf_expand_label(hp, keylen, md, secret, secretlen,
                                      HP_LABEL, sizeof(HP_LABEL) - 1) != 0) {
    return -1;
  }

  return 0;
}

int ngtcp2_crypto_update_traffic_secret(uint8_t *dest, ngtcp2_crypto_md *md,
                                        const uint8_t *secret,
                                        size_t secretlen) {
  static const uint8_t LABEL[] = "traffic upd";

  if (ngtcp2_crypto_hkdf_expand_label(dest, secretlen, md, secret, secretlen,
                                      LABEL, sizeof(LABEL) - 1) != 0) {
    return -1;
  }

  return 0;
}

int ngtcp2_crypto_derive_and_install_key(
    ngtcp2_conn *conn, uint8_t *rx_key, uint8_t *rx_iv, uint8_t *rx_hp,
    uint8_t *tx_key, uint8_t *tx_iv, uint8_t *tx_hp, ngtcp2_crypto_aead *aead,
    ngtcp2_crypto_md *md, ngtcp2_crypto_level level, const uint8_t *rx_secret,
    const uint8_t *tx_secret, size_t secretlen, ngtcp2_crypto_side side) {
  uint8_t rx_keybuf[64], rx_ivbuf[64], rx_hpbuf[64];
  uint8_t tx_keybuf[64], tx_ivbuf[64], tx_hpbuf[64];
  size_t keylen = ngtcp2_crypto_aead_keylen(aead);
  size_t ivlen = ngtcp2_crypto_packet_protection_ivlen(aead);
  size_t hplen = keylen;

  if (!rx_key) {
    rx_key = rx_keybuf;
  }
  if (!rx_iv) {
    rx_iv = rx_ivbuf;
  }
  if (!rx_hp) {
    rx_hp = rx_hpbuf;
  }
  if (!tx_key) {
    tx_key = tx_keybuf;
  }
  if (!tx_iv) {
    tx_iv = tx_ivbuf;
  }
  if (!tx_hp) {
    tx_hp = tx_hpbuf;
  }

  if ((level != NGTCP2_CRYPTO_LEVEL_EARLY ||
       side == NGTCP2_CRYPTO_SIDE_SERVER) &&
      ngtcp2_crypto_derive_packet_protection_key(rx_key, rx_iv, rx_hp, aead, md,
                                                 rx_secret, secretlen) != 0) {
    return -1;
  }

  if ((level != NGTCP2_CRYPTO_LEVEL_EARLY ||
       side == NGTCP2_CRYPTO_SIDE_CLIENT) &&
      ngtcp2_crypto_derive_packet_protection_key(tx_key, tx_iv, tx_hp, aead, md,
                                                 tx_secret, secretlen) != 0) {
    return -1;
  }

  switch (level) {
  case NGTCP2_CRYPTO_LEVEL_EARLY:
    if (side == NGTCP2_CRYPTO_SIDE_CLIENT) {
      ngtcp2_conn_install_early_keys(conn, tx_key, keylen, tx_iv, ivlen, tx_hp,
                                     hplen);
    } else {
      ngtcp2_conn_install_early_keys(conn, rx_key, keylen, rx_iv, ivlen, rx_hp,
                                     hplen);
    }
    break;
  case NGTCP2_CRYPTO_LEVEL_HANDSHAKE:
    ngtcp2_conn_install_handshake_rx_keys(conn, rx_key, keylen, rx_iv, ivlen,
                                          rx_hp, hplen);
    ngtcp2_conn_install_handshake_tx_keys(conn, tx_key, keylen, tx_iv, ivlen,
                                          tx_hp, hplen);
    break;
  case NGTCP2_CRYPTO_LEVEL_APP:
    ngtcp2_conn_install_rx_keys(conn, rx_key, keylen, rx_iv, ivlen, rx_hp,
                                hplen);
    ngtcp2_conn_install_tx_keys(conn, tx_key, keylen, tx_iv, ivlen, tx_hp,
                                hplen);
    break;
  default:
    return -1;
  }

  return 0;
}