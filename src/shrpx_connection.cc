/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2015 Tatsuhiro Tsujikawa
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
#include "shrpx_connection.h"

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif // HAVE_UNISTD_H
#include <netinet/tcp.h>

#include <limits>

#include "ssl_compat.h"

#ifdef NGHTTP2_OPENSSL_IS_WOLFSSL
#  include <wolfssl/options.h>
#  include <wolfssl/openssl/err.h>
#else // !NGHTTP2_OPENSSL_IS_WOLFSSL
#  include <openssl/err.h>
#endif // !NGHTTP2_OPENSSL_IS_WOLFSSL

#include "shrpx_tls.h"
#include "shrpx_log.h"
#include "memchunk.h"
#include "util.h"

using namespace nghttp2;
using namespace std::chrono_literals;

namespace shrpx {

Connection::Connection(struct ev_loop *loop, int fd, SSL *ssl,
                       MemchunkPool *mcpool, ev_tstamp write_timeout,
                       ev_tstamp read_timeout,
                       const RateLimitConfig &write_limit,
                       const RateLimitConfig &read_limit, IOCb writecb,
                       IOCb readcb, TimerCb timeoutcb, void *data,
                       size_t tls_dyn_rec_warmup_threshold,
                       ev_tstamp tls_dyn_rec_idle_timeout, Proto proto)
  :
#ifdef ENABLE_HTTP3
    conn_ref{nullptr, this},
#endif // ENABLE_HTTP3
    tls{DefaultMemchunks(mcpool)},
    wlimit(loop, &wev, write_limit.rate, write_limit.burst),
    rlimit(loop, &rev, read_limit.rate, read_limit.burst, this),
    loop(loop),
    data(data),
    fd(fd),
    tls_dyn_rec_warmup_threshold(tls_dyn_rec_warmup_threshold),
    tls_dyn_rec_idle_timeout(util::duration_from(tls_dyn_rec_idle_timeout)),
    proto(proto),
    read_timeout(read_timeout) {

  ev_io_init(&wev, writecb, fd, EV_WRITE);
  ev_io_init(&rev, readcb, proto == Proto::HTTP3 ? 0 : fd, EV_READ);

  wev.data = this;
  rev.data = this;

  ev_timer_init(&wt, timeoutcb, 0., write_timeout);
  ev_timer_init(&rt, timeoutcb, 0., read_timeout);

  wt.data = this;
  rt.data = this;

  if (ssl) {
    set_ssl(ssl);
  }
}

Connection::~Connection() { disconnect(); }

void Connection::disconnect() {
  if (tls.ssl) {
    if (proto != Proto::HTTP3) {
      SSL_set_shutdown(tls.ssl,
                       SSL_get_shutdown(tls.ssl) | SSL_RECEIVED_SHUTDOWN);
      ERR_clear_error();

      SSL_shutdown(tls.ssl);
    }

    // Unset app data here, so that ngtcp2_conn never be used by
    // libngtcp2_crypto_ossl that may be called by SSL_free.
    SSL_set_app_data(tls.ssl, NULL);
    SSL_free(tls.ssl);
    tls.ssl = nullptr;

    tls.last_write_idle = {};
    tls.warmup_writelen = 0;
    tls.last_writelen = 0;
    tls.last_readlen = 0;
    tls.initial_handshake_done = false;
    tls.reneg_started = false;
    tls.sct_requested = false;
    tls.early_data_finish = false;
  }

  if (proto != Proto::HTTP3 && fd != -1) {
    shutdown(fd, SHUT_WR);
    close(fd);
    fd = -1;
  }

  // Stop watchers here because they could be activated in
  // SSL_shutdown().
  ev_timer_stop(loop, &rt);
  ev_timer_stop(loop, &wt);

  rlimit.stopw();
  wlimit.stopw();
}

void Connection::prepare_client_handshake() {
  SSL_set_connect_state(tls.ssl);
  // This prevents SSL_read_early_data from being called.
  tls.early_data_finish = true;
}

void Connection::prepare_server_handshake() {
  SSL_set_accept_state(tls.ssl);
  tls.server_handshake = true;
}

void Connection::set_ssl(SSL *ssl) {
  tls.ssl = ssl;

  SSL_set_app_data(tls.ssl, this);
}

int Connection::tls_handshake() {
  wlimit.stopw();
  ev_timer_stop(loop, &wt);

  if (tls.initial_handshake_done) {
    return write_tls_pending_handshake();
  }

  if (SSL_get_fd(tls.ssl) == -1) {
    SSL_set_fd(tls.ssl, fd);
  }

  int rv;
#if defined(NGHTTP2_GENUINE_OPENSSL) ||                                        \
  defined(NGHTTP2_OPENSSL_IS_BORINGSSL) ||                                     \
  (defined(NGHTTP2_OPENSSL_IS_WOLFSSL) && defined(WOLFSSL_EARLY_DATA))
  auto &tlsconf = get_config()->tls;
  std::array<uint8_t, 16_k> buf;
#endif // NGHTTP2_GENUINE_OPENSSL || NGHTTP2_OPENSSL_IS_BORINGSSL ||
       // (NGHTTP2_OPENSSL_IS_WOLFSSL && WOLFSSL_EARLY_DATA)

  ERR_clear_error();

#ifdef NGHTTP2_GENUINE_OPENSSL
  if (!tls.server_handshake || tls.early_data_finish) {
    rv = SSL_do_handshake(tls.ssl);
  } else {
    for (;;) {
      size_t nread;

      rv = SSL_read_early_data(tls.ssl, buf.data(), buf.size(), &nread);
      if (rv == SSL_READ_EARLY_DATA_ERROR) {
        // If we have early data, and server sends ServerHello, assume
        // that handshake is completed in server side, and start
        // processing request.  If we don't exit handshake code here,
        // server waits for EndOfEarlyData and Finished message from
        // client, which voids the purpose of 0-RTT data.  The left
        // over of handshake is done through write_tls or read_tls.
        if (tlsconf.no_postpone_early_data && tls.earlybuf.rleft()) {
          rv = 1;
        }

        break;
      }

      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "tls: read early data " << nread << " bytes";
      }

      tls.earlybuf.append(buf.data(), nread);

      if (rv == SSL_READ_EARLY_DATA_FINISH) {
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "tls: read all early data; total "
                    << tls.earlybuf.rleft() << " bytes";
        }
        tls.early_data_finish = true;
        // The same reason stated above.
        if (tlsconf.no_postpone_early_data && tls.earlybuf.rleft()) {
          rv = 1;
        } else {
          ERR_clear_error();
          rv = SSL_do_handshake(tls.ssl);
        }
        break;
      }
    }
  }
#elif defined(NGHTTP2_OPENSSL_IS_WOLFSSL) && defined(WOLFSSL_EARLY_DATA)
  if (!tls.server_handshake || tls.early_data_finish) {
    rv = SSL_do_handshake(tls.ssl);
  } else {
    for (;;) {
      size_t nread = 0;

      rv = SSL_read_early_data(tls.ssl, buf.data(), buf.size(), &nread);
      if (rv < 0) {
        if (SSL_get_error(tls.ssl, rv) == SSL_ERROR_WANT_READ) {
          if (tlsconf.no_postpone_early_data && tls.earlybuf.rleft()) {
            rv = 1;
          }

          break;
        }

        /* It looks like we are here if there is no early data. */
        tls.early_data_finish = true;

        ERR_clear_error();
        rv = SSL_do_handshake(tls.ssl);

        break;
      }

      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "tls: read early data " << nread << " bytes";
      }

      tls.earlybuf.append(buf.data(), nread);

      if (rv == 0) {
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "tls: read all early data; total "
                    << tls.earlybuf.rleft() << " bytes";
        }
        tls.early_data_finish = true;
        // The same reason stated above.
        if (tlsconf.no_postpone_early_data && tls.earlybuf.rleft()) {
          rv = 1;
        } else {
          ERR_clear_error();
          rv = SSL_do_handshake(tls.ssl);
        }
        break;
      }
    }
  }
#else  // !NGHTTP2_GENUINE_OPENSSL && !(NGHTTP2_OPENSSL_IS_WOLFSSL &&
       // WOLFSSL_EARLY_DATA)
  rv = SSL_do_handshake(tls.ssl);
#endif // !NGHTTP2_GENUINE_OPENSSL && !(NGHTTP2_OPENSSL_IS_WOLFSSL &&
       // WOLFSSL_EARLY_DATA)

  if (rv <= 0) {
    auto err = SSL_get_error(tls.ssl, rv);
    switch (err) {
    case SSL_ERROR_WANT_READ:
      break;
    case SSL_ERROR_WANT_WRITE:
      wlimit.startw();
      ev_timer_again(loop, &wt);
      break;
    case SSL_ERROR_SSL: {
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "tls: handshake libssl error: "
                  << ERR_error_string(ERR_get_error(), nullptr);
      }
      return SHRPX_ERR_NETWORK;
    }
    default:
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "tls: handshake libssl error " << err;
      }
      return SHRPX_ERR_NETWORK;
    }
  }

  if (rv != 1) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "tls: handshake is still in progress";
    }
    return SHRPX_ERR_INPROGRESS;
  }

#ifdef NGHTTP2_OPENSSL_IS_BORINGSSL
  if (!tlsconf.no_postpone_early_data && SSL_in_early_data(tls.ssl) &&
      SSL_in_init(tls.ssl)) {
    auto nread = SSL_read(tls.ssl, buf.data(), buf.size());
    if (nread <= 0) {
      auto err = SSL_get_error(tls.ssl, nread);
      switch (err) {
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_WANT_WRITE:
        break;
      case SSL_ERROR_ZERO_RETURN:
        return SHRPX_ERR_EOF;
      case SSL_ERROR_SSL:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "SSL_read: "
                    << ERR_error_string(ERR_get_error(), nullptr);
        }
        return SHRPX_ERR_NETWORK;
      default:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "SSL_read: SSL_get_error returned " << err;
        }
        return SHRPX_ERR_NETWORK;
      }
    } else {
      tls.earlybuf.append(buf.data(), static_cast<size_t>(nread));
    }

    if (SSL_in_init(tls.ssl)) {
      return SHRPX_ERR_INPROGRESS;
    }
  }
#endif // NGHTTP2_OPENSSL_IS_BORINGSSL

  // Handshake was done

  rv = check_http2_requirement();
  if (rv != 0) {
    return -1;
  }

  tls.initial_handshake_done = true;

  return write_tls_pending_handshake();
}

int Connection::write_tls_pending_handshake() {
#ifdef NGHTTP2_OPENSSL_IS_BORINGSSL
  if (!SSL_in_init(tls.ssl)) {
    // This will send a session ticket.
    auto nwrite = SSL_write(tls.ssl, "", 0);
    if (nwrite < 0) {
      auto err = SSL_get_error(tls.ssl, nwrite);
      switch (err) {
      case SSL_ERROR_WANT_READ:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "Close connection due to TLS renegotiation";
        }
        return SHRPX_ERR_NETWORK;
      case SSL_ERROR_WANT_WRITE:
        break;
      case SSL_ERROR_SSL:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "SSL_write: "
                    << ERR_error_string(ERR_get_error(), nullptr);
        }
        return SHRPX_ERR_NETWORK;
      default:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "SSL_write: SSL_get_error returned " << err;
        }
        return SHRPX_ERR_NETWORK;
      }
    }
  }
#endif // NGHTTP2_OPENSSL_IS_BORINGSSL

  // We have to start read watcher, since later stage of code expects
  // this.
  rlimit.startw();

  // We may have whole request in tls.rbuf.  This means that we don't
  // get notified further read event.  This is especially true for
  // HTTP/1.1.
  handle_tls_pending_read();

  if (LOG_ENABLED(INFO)) {
    LOG(INFO) << "SSL/TLS handshake completed";
    nghttp2::tls::TLSSessionInfo tls_info{};
    if (nghttp2::tls::get_tls_session_info(&tls_info, tls.ssl)) {
      LOG(INFO) << "cipher=" << tls_info.cipher
                << " protocol=" << tls_info.protocol
                << " resumption=" << (tls_info.session_reused ? "yes" : "no")
                << " session_id="
                << util::format_hex(std::span{tls_info.session_id,
                                              tls_info.session_id_length});
    }
  }

  return 0;
}

int Connection::check_http2_requirement() {
  const unsigned char *next_proto = nullptr;
  unsigned int next_proto_len;

  SSL_get0_alpn_selected(tls.ssl, &next_proto, &next_proto_len);
  if (next_proto == nullptr ||
      !util::check_h2_is_selected(as_string_view(next_proto, next_proto_len))) {
    return 0;
  }
  if (!nghttp2::tls::check_http2_tls_version(tls.ssl)) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "TLSv1.2 was not negotiated.  HTTP/2 must not be used.";
    }
    return -1;
  }

  auto check_block_list = false;
  if (tls.server_handshake) {
    check_block_list = !get_config()->tls.no_http2_cipher_block_list;
  } else {
    check_block_list = !get_config()->tls.client.no_http2_cipher_block_list;
  }

  if (check_block_list &&
      nghttp2::tls::check_http2_cipher_block_list(tls.ssl)) {
    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "The negotiated cipher suite is in HTTP/2 cipher suite "
                   "block list.  HTTP/2 must not be used.";
    }
    return -1;
  }

  return 0;
}

namespace {
constexpr size_t SHRPX_SMALL_WRITE_LIMIT = 1300;
} // namespace

size_t Connection::get_tls_write_limit() {
  if (tls_dyn_rec_warmup_threshold == 0) {
    return std::numeric_limits<ssize_t>::max();
  }

  auto t = std::chrono::steady_clock::now();

  if (tls.last_write_idle.time_since_epoch().count() >= 0 &&
      t - tls.last_write_idle > tls_dyn_rec_idle_timeout) {
    // Time out, use small record size
    tls.warmup_writelen = 0;
    return SHRPX_SMALL_WRITE_LIMIT;
  }

  if (tls.warmup_writelen >= tls_dyn_rec_warmup_threshold) {
    return std::numeric_limits<ssize_t>::max();
  }

  return SHRPX_SMALL_WRITE_LIMIT;
}

void Connection::update_tls_warmup_writelen(size_t n) {
  if (tls.warmup_writelen < tls_dyn_rec_warmup_threshold) {
    tls.warmup_writelen += n;
  }
}

void Connection::start_tls_write_idle() {
  if (tls.last_write_idle.time_since_epoch().count() < 0) {
    tls.last_write_idle = std::chrono::steady_clock::now();
  }
}

nghttp2_ssize Connection::write_tls(const void *data, size_t len) {
  // SSL_write requires the same arguments (buf pointer and its
  // length) on SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE.
  // get_write_limit() may return smaller length than previously
  // passed to SSL_write, which violates OpenSSL assumption.  To avoid
  // this, we keep last length passed to SSL_write to
  // tls.last_writelen if SSL_write indicated I/O blocking.
  if (tls.last_writelen == 0) {
    len = std::min(len, wlimit.avail());
    len = std::min(len, get_tls_write_limit());
    if (len == 0) {
      return 0;
    }
  } else {
    len = tls.last_writelen;
    tls.last_writelen = 0;
  }

  tls.last_write_idle = std::chrono::steady_clock::time_point(-1s);

  ERR_clear_error();

#ifdef NGHTTP2_GENUINE_OPENSSL
  int rv;
  if (SSL_is_init_finished(tls.ssl)) {
    rv = SSL_write(tls.ssl, data, static_cast<int>(len));
  } else {
    size_t nwrite;
    rv = SSL_write_early_data(tls.ssl, data, len, &nwrite);
    // Use the same semantics with SSL_write.
    if (rv == 1) {
      rv = static_cast<int>(nwrite);
    }
  }
#else  // !NGHTTP2_GENUINE_OPENSSL
  auto rv = SSL_write(tls.ssl, data, static_cast<int>(len));
#endif // !NGHTTP2_GENUINE_OPENSSL

  if (rv <= 0) {
    auto err = SSL_get_error(tls.ssl, rv);
    switch (err) {
    case SSL_ERROR_WANT_READ:
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "Close connection due to TLS renegotiation";
      }
      return SHRPX_ERR_NETWORK;
    case SSL_ERROR_WANT_WRITE:
      tls.last_writelen = len;
      wlimit.startw();
      ev_timer_again(loop, &wt);

      return 0;
    case SSL_ERROR_SSL:
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "SSL_write: "
                  << ERR_error_string(ERR_get_error(), nullptr);
      }
      return SHRPX_ERR_NETWORK;
    default:
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "SSL_write: SSL_get_error returned " << err;
      }
      return SHRPX_ERR_NETWORK;
    }
  }

  wlimit.drain(static_cast<size_t>(rv));

  if (ev_is_active(&wt)) {
    ev_timer_again(loop, &wt);
  }

  update_tls_warmup_writelen(static_cast<size_t>(rv));

  return rv;
}

nghttp2_ssize Connection::read_tls(void *data, size_t len) {
  ERR_clear_error();

#if defined(NGHTTP2_GENUINE_OPENSSL) ||                                        \
  defined(NGHTTP2_OPENSSL_IS_BORINGSSL) || defined(NGHTTP2_OPENSSL_IS_WOLFSSL)
  if (tls.earlybuf.rleft()) {
    return as_signed(tls.earlybuf.remove(data, len));
  }
#endif // NGHTTP2_GENUINE_OPENSSL || NGHTTP2_OPENSSL_IS_BORINGSSL ||
       // defined(NGHTTP2_OPENSSL_IS_WOLFSSL)

  // SSL_read requires the same arguments (buf pointer and its
  // length) on SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE.
  // rlimit_.avail() or rlimit_.avail() may return different length
  // than the length previously passed to SSL_read, which violates
  // OpenSSL assumption.  To avoid this, we keep last length passed
  // to SSL_read to tls_last_readlen_ if SSL_read indicated I/O
  // blocking.
  if (tls.last_readlen == 0) {
    len = std::min(len, rlimit.avail());
    if (len == 0) {
      return 0;
    }
  } else {
    len = tls.last_readlen;
    tls.last_readlen = 0;
  }

#ifdef NGHTTP2_GENUINE_OPENSSL
  if (!tls.early_data_finish) {
    // TLSv1.3 handshake is still going on.
    size_t nread;
    auto rv = SSL_read_early_data(tls.ssl, data, len, &nread);
    if (rv == SSL_READ_EARLY_DATA_ERROR) {
      auto err = SSL_get_error(tls.ssl, rv);
      switch (err) {
      case SSL_ERROR_WANT_READ:
        tls.last_readlen = len;
        return 0;
      case SSL_ERROR_SSL:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "SSL_read: "
                    << ERR_error_string(ERR_get_error(), nullptr);
        }
        return SHRPX_ERR_NETWORK;
      default:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "SSL_read: SSL_get_error returned " << err;
        }
        return SHRPX_ERR_NETWORK;
      }
    }

    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "tls: read early data " << nread << " bytes";
    }

    if (rv == SSL_READ_EARLY_DATA_FINISH) {
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "tls: read all early data";
      }
      tls.early_data_finish = true;
      // We may have stopped write watcher in write_tls.
      wlimit.startw();
    }

    rlimit.drain(nread);

    return as_signed(nread);
  }
#endif // NGHTTP2_GENUINE_OPENSSL

#if defined(NGHTTP2_OPENSSL_IS_WOLFSSL) && defined(WOLFSSL_EARLY_DATA)
  if (!tls.early_data_finish) {
    // TLSv1.3 handshake is still going on.
    size_t nread = 0;
    auto rv = SSL_read_early_data(tls.ssl, data, len, &nread);
    if (rv < 0) {
      auto err = SSL_get_error(tls.ssl, rv);
      switch (err) {
      case SSL_ERROR_WANT_READ:
        tls.last_readlen = len;
        return 0;
      case SSL_ERROR_SSL:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "SSL_read: "
                    << ERR_error_string(ERR_get_error(), nullptr);
        }
        return SHRPX_ERR_NETWORK;
      default:
        if (LOG_ENABLED(INFO)) {
          LOG(INFO) << "SSL_read: SSL_get_error returned " << err;
        }
        return SHRPX_ERR_NETWORK;
      }
    }

    if (LOG_ENABLED(INFO)) {
      LOG(INFO) << "tls: read early data " << nread << " bytes";
    }

    if (rv == 0) {
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "tls: read all early data";
      }
      tls.early_data_finish = true;
      // We may have stopped write watcher in write_tls.
      wlimit.startw();
    }

    rlimit.drain(nread);

    return as_signed(nread);
  }
#endif // NGHTTP2_OPENSSL_IS_WOLFSSL && WOLFSSL_EARLY_DATA

  auto rv = SSL_read(tls.ssl, data, static_cast<int>(len));

  if (rv <= 0) {
    auto err = SSL_get_error(tls.ssl, rv);
    switch (err) {
    case SSL_ERROR_WANT_READ:
      tls.last_readlen = len;
      return 0;
    case SSL_ERROR_WANT_WRITE:
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "Close connection due to TLS renegotiation";
      }
      return SHRPX_ERR_NETWORK;
    case SSL_ERROR_ZERO_RETURN:
      return SHRPX_ERR_EOF;
    case SSL_ERROR_SSL:
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "SSL_read: " << ERR_error_string(ERR_get_error(), nullptr);
      }
      return SHRPX_ERR_NETWORK;
    default:
      if (LOG_ENABLED(INFO)) {
        LOG(INFO) << "SSL_read: SSL_get_error returned " << err;
      }
      return SHRPX_ERR_NETWORK;
    }
  }

  rlimit.drain(static_cast<size_t>(rv));

  return rv;
}

nghttp2_ssize Connection::write_clear(const void *data, size_t len) {
  len = std::min(len, wlimit.avail());
  if (len == 0) {
    return 0;
  }

  ssize_t nwrite;
  while ((nwrite = write(fd, data, len)) == -1 && errno == EINTR)
    ;
  if (nwrite == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      wlimit.startw();
      ev_timer_again(loop, &wt);
      return 0;
    }
    return SHRPX_ERR_NETWORK;
  }

  wlimit.drain(as_unsigned(nwrite));

  if (ev_is_active(&wt)) {
    ev_timer_again(loop, &wt);
  }

  return nwrite;
}

nghttp2_ssize Connection::writev_clear(struct iovec *iov, int iovcnt) {
  iovcnt = limit_iovec(iov, iovcnt, wlimit.avail());
  if (iovcnt == 0) {
    return 0;
  }

  ssize_t nwrite;
  while ((nwrite = writev(fd, iov, iovcnt)) == -1 && errno == EINTR)
    ;
  if (nwrite == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      wlimit.startw();
      ev_timer_again(loop, &wt);
      return 0;
    }
    return SHRPX_ERR_NETWORK;
  }

  wlimit.drain(as_unsigned(nwrite));

  if (ev_is_active(&wt)) {
    ev_timer_again(loop, &wt);
  }

  return nwrite;
}

nghttp2_ssize Connection::read_clear(void *data, size_t len) {
  len = std::min(len, rlimit.avail());
  if (len == 0) {
    return 0;
  }

  ssize_t nread;
  while ((nread = read(fd, data, len)) == -1 && errno == EINTR)
    ;
  if (nread == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return SHRPX_ERR_NETWORK;
  }

  if (nread == 0) {
    return SHRPX_ERR_EOF;
  }

  rlimit.drain(as_unsigned(nread));

  return nread;
}

nghttp2_ssize Connection::read_nolim_clear(void *data, size_t len) {
  ssize_t nread;
  while ((nread = read(fd, data, len)) == -1 && errno == EINTR)
    ;
  if (nread == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return SHRPX_ERR_NETWORK;
  }

  if (nread == 0) {
    return SHRPX_ERR_EOF;
  }

  return nread;
}

nghttp2_ssize Connection::peek_clear(void *data, size_t len) {
  ssize_t nread;
  while ((nread = recv(fd, data, len, MSG_PEEK)) == -1 && errno == EINTR)
    ;
  if (nread == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return SHRPX_ERR_NETWORK;
  }

  if (nread == 0) {
    return SHRPX_ERR_EOF;
  }

  return nread;
}

void Connection::handle_tls_pending_read() {
  if (!ev_is_active(&rev)) {
    return;
  }
  rlimit.handle_tls_pending_read();
}

int Connection::get_tcp_hint(TCPHint *hint) const {
#if defined(TCP_INFO) && defined(TCP_NOTSENT_LOWAT)
  struct tcp_info tcp_info;
  socklen_t tcp_info_len = sizeof(tcp_info);
  int rv;

  rv = getsockopt(fd, IPPROTO_TCP, TCP_INFO, &tcp_info, &tcp_info_len);

  if (rv != 0) {
    return -1;
  }

  auto avail_packets = tcp_info.tcpi_snd_cwnd > tcp_info.tcpi_unacked
                         ? tcp_info.tcpi_snd_cwnd - tcp_info.tcpi_unacked
                         : 0;

  // http://www.slideshare.net/kazuho/programming-tcp-for-responsiveness

  // TODO 29 (5 (header) + 8 (explicit nonce) + 16 (tag)) is TLS
  // overhead for AES-GCM.  For CHACHA20_POLY1305, it is 21 since it
  // does not need 8 bytes explicit nonce.
  //
  // For TLSv1.3, AES-GCM and CHACHA20_POLY1305 overhead are now 22
  // bytes (5 (header) + 1 (ContentType) + 16 (tag)).
  size_t tls_overhead;
#  ifdef TLS1_3_VERSION
  if (SSL_version(tls.ssl) == TLS1_3_VERSION) {
    tls_overhead = 22;
  } else
#  endif // TLS1_3_VERSION
  {
    tls_overhead = 29;
  }

  auto writable_size =
    (avail_packets + 2) * (tcp_info.tcpi_snd_mss - tls_overhead);
  if (writable_size > 16_k) {
    writable_size = writable_size & ~(16_k - 1);
  } else {
    if (writable_size < 536) {
      LOG(INFO) << "writable_size is too small: " << writable_size;
    }
    // TODO is this required?
    writable_size = std::max(writable_size, static_cast<size_t>(536 * 2));
  }

  // if (LOG_ENABLED(INFO)) {
  //   LOG(INFO) << "snd_cwnd=" << tcp_info.tcpi_snd_cwnd
  //             << ", unacked=" << tcp_info.tcpi_unacked
  //             << ", snd_mss=" << tcp_info.tcpi_snd_mss
  //             << ", rtt=" << tcp_info.tcpi_rtt << "us"
  //             << ", rcv_space=" << tcp_info.tcpi_rcv_space
  //             << ", writable=" << writable_size;
  // }

  hint->write_buffer_size = writable_size;
  // TODO tcpi_rcv_space is considered as rwin, is that correct?
  hint->rwin = tcp_info.tcpi_rcv_space;

  return 0;
#else  // !defined(TCP_INFO) || !defined(TCP_NOTSENT_LOWAT)
  return -1;
#endif // !defined(TCP_INFO) || !defined(TCP_NOTSENT_LOWAT)
}

void Connection::again_rt(ev_tstamp t) {
  read_timeout = t;
  rt.repeat = t;
  ev_timer_again(loop, &rt);
  last_read = std::chrono::steady_clock::now();
}

void Connection::again_rt() {
  rt.repeat = read_timeout;
  ev_timer_again(loop, &rt);
  last_read = std::chrono::steady_clock::now();
}

bool Connection::expired_rt() {
  auto delta = read_timeout - util::ev_tstamp_from(
                                std::chrono::steady_clock::now() - last_read);
  if (delta < 1e-9) {
    return true;
  }
  rt.repeat = delta;
  ev_timer_again(loop, &rt);
  return false;
}

} // namespace shrpx
